// SPDX-License-Identifier: Apache-2.0
//
// RunSession: one running user program (W1, ADR 0040): its output streamed in UTF-8 chunks, input
// written to its stdin, Stop ending its whole process tree, and the exit code and run time at the
// end. RunService starts one per run; it is not a build (no line splitting, no diagnostics).
//
/*
 * Threads. start() launches the program on the calling thread, so launch errors come back at once.
 * Then a reader per piped output stream, a writer for stdin (when it is a pipe) and a waiter run
 * the session. Output is passed on as it arrives, not by line, so a prompt without a newline
 * shows at once: each chunk is valid UTF-8 (bytes that are not become U+FFFD) and never ends in
 * the middle of a character (the rest waits for the next read). With Stdio::Pipe the C runtime
 * of most programs buffers stdout fully, so a program that does not flush shows its output late:
 * the external console (run_target.hpp) is the mode for interactive programs.
 *
 * Callbacks run on the session's threads, never on the caller's and never two at a time; onExited
 * runs last, once, after all output. A callback may call stop(), writeInput() and closeInput();
 * it must not destroy the session or wait for it.
 *
 * End of a run. By default when the program exits by itself, processes it started are left running, as a
 * program started from Explorer or Visual Studio would leave them: the container is released
 * (IProcess::releaseContainer) when anything else is still in it, and if such a process still
 * holds the output pipes after outputGrace, the rest of the output is abandoned. stop() instead
 * ends the whole tree at once (the Job Object or process group); the explicit helper option also
 * ends descendants on natural exit. `stopped` is reported only when
 * that is what ended the program (a stop that loses the race against its exit is not a stop).
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace frost::build {

enum class RunStream : std::uint8_t { Stdout, Stderr };

// How a program ended.
struct RunExit {
    int exitCode = 0;            // As io::ExitStatus::exitCode (io::kTerminatedExitCode after stop()).
    bool stopped = false;        // stop() ended it (header comment).
    bool waitFailed = false;     // The exit status could not be read; exitCode is -1.
    bool cleanupFailed = false;  // An owned descendant tree could not be terminated.
    bool decodingFailed = false; // A strict stream contained malformed UTF-8.
    IClock::Duration duration{}; // From launch to exit, on the session's clock.

    friend bool operator==(const RunExit&, const RunExit&) = default;
};

struct RunSessionCallbacks {
    // A chunk of output (valid UTF-8, never empty, see the header comment).
    std::function<void(RunStream stream, std::string_view text)> onOutput{};
    // How the program ended; the session's last callback.
    std::function<void(const RunExit& exit)> onExited{};
};

struct RunSessionOptions {
    // How long processes the program left behind may hold the output open after it exited.
    std::chrono::milliseconds outputGrace{500};
    // Helper processes own their descendants even after the helper exits naturally. User runs
    // keep the default, allowing children to outlive the launched program.
    bool terminateDescendantsOnExit = false;
    // Treat malformed UTF-8 on either output stream as a protocol failure and end the tree.
    bool rejectInvalidUtf8 = false;
};

class RunSession {
public:
    /*
     * Launches `spec` (killOnClose must be set, so Stop can end the tree) and starts the session's
     * threads. The caller has passed the trust gate. The clock must outlive the session. Errors:
     * InvalidArgument (killOnClose is off), the launcher's errors.
     */
    [[nodiscard]] static Result<std::unique_ptr<RunSession>>
    start(io::IProcessLauncher& launcher, const io::ProcessSpec& spec, RunSessionCallbacks callbacks,
          IClock& clock = systemClock(), RunSessionOptions options = {});

    // Stops a program that is still running, then waits for the session's threads.
    ~RunSession();
    RunSession(const RunSession&) = delete ("a session owns its process and threads");
    RunSession& operator=(const RunSession&) = delete ("a session owns its process and threads");

    // Ends the program's whole process tree. Thread-safe and idempotent; nothing after it exited.
    void stop() noexcept;

    /*
     * Queues `bytes` for the program's stdin; the writer thread writes them in order. Thread-safe.
     * Errors: InvalidArgument (stdin is not a pipe, or closeInput() was called), NotFound (the
     * program has exited).
     */
    [[nodiscard]] Result<void> writeInput(std::string bytes);

    // Closes stdin once the queued input is written, so the program reads end of file. Idempotent.
    void closeInput() noexcept;

    // Whether stdin is an open pipe the program can still read from.
    [[nodiscard]] bool acceptsInput() const;

    // Whether onExited has returned.
    [[nodiscard]] bool finished() const;

    // Blocks until the program's end is published, or `timeout` passes (nullopt). Not from a callback.
    [[nodiscard]] std::optional<RunExit> waitFor(IClock::Duration timeout) const;

    // The program's process id (the root of its tree).
    [[nodiscard]] io::ProcessId processId() const noexcept;

    struct State; // Shared with the session's threads; defined in the implementation.

private:
    explicit RunSession(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};

/*
 * Splits a byte stream into chunks of valid UTF-8 that never end inside a character: feed()
 * returns the complete part of what it has (invalid bytes as U+FFFD) and keeps an incomplete
 * trailing sequence (at most 3 bytes) for the next call; finish() returns the rest. Not
 * thread-safe; one per stream. O(bytes).
 */
class Utf8ChunkDecoder {
public:
    [[nodiscard]] std::string feed(std::string_view bytes);
    [[nodiscard]] std::string finish();
    [[nodiscard]] bool invalid() const noexcept { return m_invalid; }

private:
    std::string m_pending;
    bool m_invalid = false;
};

} // namespace frost::build
