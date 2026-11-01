// SPDX-License-Identifier: Apache-2.0
//
// ScriptedProcessLauncher: the deterministic IProcessLauncher double (PLAN.md
// section 6.2). Tests queue the processes they expect to be launched, with
// their output and exit status, and afterwards inspect what was launched and
// what was written to each process's stdin. No real process is started and no
// real time passes.
//
/*
 * Behaviour:
 *   - launch() validates the spec exactly like the real launcher
 *     (validateProcessSpec with the platform's rules), so code that would be
 *     rejected for real is rejected here too. Every launch, valid or not, is
 *     recorded with its whole spec, the image-path form and desktop included.
 *   - Each valid launch takes the next ScriptedProcess from the queue (FIFO).
 *     If that script has an `expectExecutable` and the launch names a
 *     different executable (exact string comparison), launch() fails with
 *     InvalidArgument ("unexpected launch") and the script stays queued; an
 *     empty queue fails with NotFound. A script with `launchError` is consumed
 *     and makes launch() fail with that error, as a missing tool would.
 *   - The returned IProcess serves the scripted stdout/stderr bytes to reads
 *     (only for streams configured as Stdio::Pipe; MergeIntoStdout appends the
 *     scripted stderr to stdout), records stdin writes, and reports the
 *     scripted exit status.
 *   - A script with `neverExits` models a hung child: wait(timeout) and
 *     communicate() fail with Timeout immediately (no sleeping), wait()
 *     without a timeout fails with Internal instead of blocking forever, and
 *     after terminate() the process reports kTerminatedExitCode (and SIGKILL
 *     on POSIX).
 *
 * Thread safety: all members may be called concurrently; records are
 * returned as snapshots. Launched processes share the recorded state with the
 * launcher, so they may outlive it.
 *
 * terminate() changes the reported status only for neverExits scripts; any
 * other scripted process counts as already exited, as with a real child that
 * finished before terminate() was called.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frost::io {

struct ScriptedProcess {
    std::optional<std::string> expectExecutable{}; // If set, the launch must use exactly this executable.
    std::optional<Error> launchError{};            // If set, launch() fails with this error.
    int exitCode = 0;
    int signal = 0;
    std::string stdoutBytes{};
    std::string stderrBytes{};
    bool neverExits = false;
};

// What the code under test did with one launch.
struct LaunchRecord {
    ProcessSpec spec{};
    bool launched = false;    // False if launch() failed (bad spec, empty queue, unexpected executable, launchError).
    std::string stdinBytes{}; // Everything written to stdin, through writeStdin or communicate.
    bool stdinClosed = false; // closeStdin() was called (communicate closes it too).
    bool terminated = false;  // terminate() was called.
};

class ScriptedProcessLauncher final : public IProcessLauncher {
public:
    ScriptedProcessLauncher();
    ~ScriptedProcessLauncher();
    ScriptedProcessLauncher(const ScriptedProcessLauncher&) = delete ("records are shared with launched processes");
    ScriptedProcessLauncher&
    operator=(const ScriptedProcessLauncher&) = delete ("records are shared with launched processes");

    // Appends a script to the queue consumed by launch().
    void expect(ScriptedProcess script);

    // Scripts not yet consumed.
    [[nodiscard]] std::size_t pendingScripts() const;

    // Every launch() call so far, in order.
    [[nodiscard]] std::vector<LaunchRecord> launches() const;

    Result<std::unique_ptr<IProcess>> launch(const ProcessSpec& spec) override;

    struct State; // Shared with the scripted processes; defined in the implementation.

private:
    std::shared_ptr<State> m_state;
};

} // namespace frost::io
