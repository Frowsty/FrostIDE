// SPDX-License-Identifier: Apache-2.0
//
// BuildSession: one running build tool process, with its output streamed line by line, its
// diagnostics parsed as they arrive, cancellation of its whole process tree and a final result.
// The MSBuild driver (frost/build/msbuild_driver.hpp) and the CMake service (frost/build/cmake_service.hpp)
// start them; BuildSessionOptions::parser picks the output format.
//
/*
 * Threads. start() launches the process on the calling thread (so launch errors come back at
 * once) and then three threads run the build: one reads stdout, one reads stderr (frost::io's
 * pipes block; TODO(M1): async pipes on the reactor instead), and one waits for the process.
 *
 * Callbacks run on those threads, never on the caller's, and never two at a time: onLine and
 * onDiagnostic are serialized with each other, and onFinished runs last, exactly once, after
 * every line. A callback may call cancel(); it must not call wait() or destroy the session
 * (that would wait for itself). The UI marshals what it needs to its own thread.
 *
 * End of a build. When the process exits, the session deals with whatever is still running in
 * its container (frost::io's Job Object or process group), because a straggler such as a
 * compiler's PDB server that inherited the output pipe would otherwise hold it open, and no output
 * would ever end. By default it ends them all. With BuildSessionOptions::infrastructureImages it
 * ends only the processes whose image is listed (build infrastructure: worker nodes, PDB and
 * compiler servers), releases the container so everything else (a program a post-build step
 * started) keeps running, as in Visual Studio, and when a survivor still holds the pipes after
 * outputGrace, abandons the rest of the output. Then the remaining output is read to its end and
 * the result is published.
 *
 * Cancellation. cancel() ends the whole process tree at once (IProcess::terminate on the Job
 * Object, which every descendant belongs to); the result then has cancelled = true and
 * success = false. A cancel() that comes after the process has exited changes nothing, and one
 * that races with the exit (the process finished on its own just before the cancel reached it)
 * reports the build as it finished: cancelled is set only when the cancel ended the process.
 */
#pragma once

#include "frost/build/diagnostic.hpp"
#include "frost/build/output_parsers.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::build {

enum class OutputStream : unsigned char { Stdout, Stderr };

// What a finished build reports.
struct BuildResult {
    bool success = false;        // The process exited with code 0 and was not cancelled.
    bool cancelled = false;      // cancel() ended the build before the process exited.
    int exitCode = 0;            // As io::ExitStatus::exitCode (io::kTerminatedExitCode after a cancel).
    IClock::Duration duration{}; // From launch to the process's exit, on the session's clock.
    std::size_t errorCount = 0;  // Unique error diagnostics.
    std::size_t warningCount = 0;
    std::vector<Diagnostic> diagnostics{}; // Unique, in arrival order (DiagnosticSet).
};

struct BuildCallbacks {
    // Each output line (UTF-8, without its terminator) as it arrives. May be empty.
    std::function<void(OutputStream stream, std::string_view line)> onLine{};
    // Each new diagnostic, right after the line it came from.
    std::function<void(const Diagnostic& diagnostic)> onDiagnostic{};
    // The result, once; the last callback of the session.
    std::function<void(const BuildResult& result)> onFinished{};
};

// How a session treats the processes left in the build's container once the tool exits.
struct BuildSessionOptions {
    // Empty: end every process left in the container. Otherwise end only those whose image file
    // name equals one of these (ASCII case ignored, "mspdbsrv.exe") and let the others run on.
    std::vector<std::string> infrastructureImages{};
    // How long the output may stay open after the tool exited while survivors run, before the
    // session abandons it (IProcess::abandonOutput) and finishes.
    std::chrono::milliseconds outputGrace{1000};
    // Makes the parser of each output stream (one per stream; output_parsers.hpp). Empty uses
    // makeMsBuildOutputParser, MSBuild's canonical format.
    OutputParserFactory parser{};
};

class BuildSession {
public:
    /*
     * Launches `spec` (stdout and stderr must be Stdio::Pipe; killOnClose must be set, so the
     * process tree can be ended) and starts the session's threads. `trusted` says the user trusts
     * the folder whose code the build runs (PLAN.md section 3.5); without it nothing starts. The
     * clock must outlive the session. Errors: Untrusted; InvalidArgument (the spec breaks the
     * rules above); the launcher's errors.
     */
    [[nodiscard]] static Result<std::unique_ptr<BuildSession>>
    start(io::IProcessLauncher& launcher, const io::ProcessSpec& spec, bool trusted, BuildCallbacks callbacks,
          IClock& clock = systemClock(), BuildSessionOptions options = {});

    // Cancels a build that is still running, then waits for the session's threads.
    ~BuildSession();
    BuildSession(const BuildSession&) = delete ("a session owns its process and threads");
    BuildSession& operator=(const BuildSession&) = delete ("a session owns its process and threads");

    // Ends the build's whole process tree (see the header comment). Thread-safe and idempotent.
    void cancel() noexcept;

    // Whether the result is published (onFinished has returned). Thread-safe.
    [[nodiscard]] bool finished() const;

    // Blocks until the result is published and returns it. Thread-safe; not from a callback.
    [[nodiscard]] const BuildResult& wait() const;

    // As wait(), but gives up after `timeout` and returns nullopt.
    [[nodiscard]] std::optional<BuildResult> waitFor(IClock::Duration timeout) const;

    // The build process's id (the root of its process tree).
    [[nodiscard]] io::ProcessId processId() const noexcept;

    struct State; // Shared with the session's threads; defined in the implementation.

private:
    explicit BuildSession(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};

} // namespace frost::build
