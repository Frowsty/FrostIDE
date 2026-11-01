// SPDX-License-Identifier: Apache-2.0
//
/*
 * IAsyncProcess: a child process whose pipes are driven by the Reactor (IOCP on Windows, epoll on Linux;
 * ADR 0020), started through IProcessLauncher::launchAsync (process.hpp). Reads, writes and the exit
 * notification are coroutines that suspend without blocking a thread and resume through the scheduler,
 * so a UI-side coroutine can stream a tool's output (git, clangd, build logs) with bounded memory.
 */
/*
 * Model. Each pipe the spec configures as Stdio::Pipe is bound to the reactor. Every OS operation runs on
 * the process's strand on a reactor thread, never on the caller's thread (so a write to a dead child can
 * never raise SIGPIPE on a pool worker: reactor threads block SIGPIPE). Each output stream keeps one read
 * in flight into an internal 64 KiB buffer whenever that buffer is empty; readStdout/readStderr copy out
 * of it. The child therefore blocks on a full pipe once 64 KiB are buffered and nobody reads: memory
 * stays bounded however much the child writes. A read whose token is stopped abandons only its own wait,
 * never the OS read, so no byte is lost: the next read returns it.
 *
 * Lifetime. The process object may be destroyed at any time, on any thread, also with operations
 * pending (they end with Cancelled). Destruction never blocks (Budget: B21): it posts the close to the
 * strand. With killOnClose the job (Windows) or the process group (POSIX) is killed; on POSIX the child is
 * then reaped by its pidfd (or a timer) once it has exited, never with waitpid(-1) (ADR 0003). Without
 * killOnClose the child keeps running and is reaped when it exits. A Reactor::shutdown ends pending
 * operations with Cancelled; a POSIX child still running then is not reaped (it is reparented when
 * frost exits). Exit order as for watches (reactor.hpp): shut the reactor down, destroy the processes,
 * then shut the schedulers down.
 *
 * Thread safety: one reader per output stream and one writer at a time (a second concurrent call is a
 * contract violation, checked); exited() may have any number of waiters; pid(), closeStdin() and
 * terminate() may run on any thread at any time. communicate() must not overlap any other call.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <cstddef>
#include <span>
#include <stop_token>
#include <string>

namespace frost::io {

class Reactor;

/*
 * Where an async process's I/O runs (the reactor) and where its coroutines resume (the scheduler).
 * Both outlive the process object and every task it returned. The real launchers refuse a caller-driven
 * scheduler (SimExecutor), which cannot take wakeups from reactor threads; ScriptedProcessLauncher accepts
 * any scheduler and does not use the reactor.
 */
struct AsyncContext {
    Reactor& reactor;
    sched::IScheduler& scheduler;
};

class IAsyncProcess {
public:
    // Never blocks (Budget: B21); see the header comment for what happens to the child.
    virtual ~IAsyncProcess() = default;
    IAsyncProcess(const IAsyncProcess&) = delete ("a process has exactly one owner");
    IAsyncProcess& operator=(const IAsyncProcess&) = delete ("a process has exactly one owner");

    [[nodiscard]] virtual ProcessId pid() const noexcept = 0;

    /*
     * Up to buffer.size() bytes of the child's stdout (stderr), at least one; 0 means EOF. `buffer` must
     * stay valid until the task completes. A stop that arrives after data was received returns the data,
     * never Cancelled. Resumes on the lane the awaiter was on (Lane::UserVisible off-scheduler).
     * Errors: InvalidArgument (not a pipe; empty buffer), Cancelled (token, the process destroyed, the
     * reactor shut down), IoError.
     */
    [[nodiscard]] virtual sched::Task<Result<std::size_t>> readStdout(std::span<char> buffer,
                                                                      std::stop_token token = {}) = 0;
    [[nodiscard]] virtual sched::Task<Result<std::size_t>> readStderr(std::span<char> buffer,
                                                                      std::stop_token token = {}) = 0;

    /*
     * Writes all of `bytes` to the child's stdin. A stop mid-write returns Cancelled and closes stdin,
     * because a torn write cannot be resumed safely. Errors: InvalidArgument (stdin is not an open pipe),
     * IoError (the child closed its stdin or exited), Cancelled.
     */
    [[nodiscard]] virtual sched::Task<Result<void>> writeStdin(std::string bytes, std::stop_token token = {}) = 0;

    // Closes the parent's end of the stdin pipe (a pending write ends with IoError). Idempotent; any thread.
    virtual void closeStdin() noexcept = 0;

    // Completes when the process has exited; later calls return the same status at once.
    // Errors: Cancelled (token, the process destroyed, the reactor shut down), IoError.
    [[nodiscard]] virtual sched::Task<Result<ExitStatus>> exited(std::stop_token token = {}) = 0;

    // As IProcess::terminate: ends the process (with killOnClose its whole container). Does not wait.
    [[nodiscard]] virtual Result<void> terminate() = 0;

    /*
     * Writes `stdinData` and closes stdin, reads stdout and stderr to EOF and waits for the exit, all
     * concurrently and bounded by `timeout` from the call. As IProcess::communicate: if the child closes
     * stdin early the rest is dropped; on timeout the process is terminated, the call waits up to 5 s for
     * the exit and fails with Timeout. It accumulates all output: use the streaming reads for bounded
     * memory. Errors: InvalidArgument (stdinData given but stdin is not a pipe), Timeout, Cancelled, IoError.
     */
    [[nodiscard]] virtual sched::Task<Result<ProcessResult>>
    communicate(std::string stdinData, IClock::Duration timeout, std::stop_token token = {}) = 0;

protected:
    IAsyncProcess() = default;
};

} // namespace frost::io
