// SPDX-License-Identifier: Apache-2.0
//
/*
 * The shared state behind IAsyncProcess (frost/io/async_process.hpp; ADR 0020): the reactor-bound pipe
 * streams with their read pumps, the stdin writer, the exit waiters and the operation awaiters, common to
 * Windows and POSIX. The platform files (async_process_win.cpp, async_process_posix.cpp with
 * child_reaper_posix.cpp) add the exit watch, terminate() and what happens to the child on release.
 * Includes asio through reactor_impl.hpp: io .cpp files only.
 */
/*
 * Strand discipline. Every member below "strand only" is touched only on the state's strand (a reactor
 * thread). Public calls post a job to the strand and suspend; the job completes the awaiter by writing its
 * result and calling scheduler.enqueue(). A stop request posts a cancel job naming the operation's id, so
 * a stale cancel can never hit a later operation; the start job checks the token itself, so a stop that
 * raced the post is not lost. Writes are cancelled through an asio cancellation slot whose signal is
 * emitted on the strand.
 *
 * Read pumps. Each output pipe keeps one async_read_some in flight into its 64 KiB buffer whenever the
 * buffer is empty and the stream is still open. A waiting reader is served from the buffer; a reader that
 * stops waiting leaves the buffered bytes for the next read.
 *
 * Release. The public object's destructor posts release(): pending operations end with Cancelled, the
 * pipes close, and releaseChild() decides the child's fate. Handlers keep the state alive (bindHandler),
 * so a platform watch that must outlive the public object (the POSIX reaper) simply keeps running.
 */
#pragma once

#include "reactor_impl.hpp" // asio first.

#ifdef _WIN32
#include <asio/windows/stream_handle.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif
#include "frost/core/error.hpp"
#include "frost/io/async_process.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/write.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace frost::io::detail {

#ifdef _WIN32
using PipeStream = asio::windows::stream_handle;
#else
using PipeStream = asio::posix::stream_descriptor;
#endif

// The internal read buffer of each output pipe (and so the most a silent consumer lets the child write
// ahead per stream, beyond the OS pipe buffer).
inline constexpr std::size_t kAsyncReadBufferBytes = 64 * 1024;

enum class OutputStream : unsigned char { Stdout, Stderr };

// A suspended operation: the coroutine, the lane to resume it on, its id and where its result goes (in
// the awaiter, which lives in the suspended frame until it is resumed).
template<class T>
struct Waiter {
    std::coroutine_handle<> handle{};
    sched::Lane lane = sched::Lane::UserVisible;
    std::uint64_t id = 0;
    Result<T>* result = nullptr;
};

class AsyncProcessState : public BoundState {
public:
    AsyncProcessState(Reactor::Impl& reactor, sched::IScheduler& scheduler, ProcessId pid, bool hasStdin,
                      bool hasStdout, bool hasStderr);
    ~AsyncProcessState() override = default;

    // --- Any thread ---
    [[nodiscard]] sched::IScheduler& scheduler() const noexcept { return m_scheduler; }
    [[nodiscard]] ProcessId pid() const noexcept { return m_pid; }
    // Which streams are pipes (fixed at creation).
    [[nodiscard]] bool hasStdin() const noexcept { return m_hasStdin; }
    [[nodiscard]] bool hasOutput(OutputStream stream) const noexcept {
        return stream == OutputStream::Stdout ? m_hasStdout : m_hasStderr;
    }
    // A fresh operation id; relaxed: ids only need to be unique.
    [[nodiscard]] std::uint64_t nextId() noexcept { return m_nextId.fetch_add(1, std::memory_order_relaxed); }
    // Ends the process (see IAsyncProcess::terminate). Thread-safe: it uses only handles that stay open
    // until the state is destroyed.
    [[nodiscard]] virtual Result<void> terminateChild() = 0;
    // The public object is gone: posts release to the strand, or releases inline after shutdown.
    void release() noexcept;

    // --- Strand only ---
    // Takes ownership of the parent's pipe ends (already registered with the reactor) and starts the read
    // pumps and the exit watch. Called once, from the creating thread before the state is shared.
    void adoptPipes(std::optional<PipeStream> in, std::optional<PipeStream> out, std::optional<PipeStream> err);
    void startOnStrand();
    void readStart(OutputStream stream, std::span<char> buffer, Waiter<std::size_t> waiter,
                   const std::stop_token& token);
    void readCancel(OutputStream stream, std::uint64_t id);
    void writeStart(std::shared_ptr<const std::string> bytes, Waiter<void> waiter, const std::stop_token& token);
    void writeCancel(std::uint64_t id);
    void closeStdinOnStrand() noexcept;
    void exitStart(Waiter<ExitStatus> waiter, const std::stop_token& token);
    void exitCancel(std::uint64_t id);
    void shutdownOnStrand() noexcept override;

protected:
    // Platform hooks, on the strand.
    virtual void startExitWatch() = 0;         // Arms the watch that ends in exitObserved().
    virtual void stopExitWatch() noexcept = 0; // Reactor shutdown: cancels it (its handler then does nothing).
    virtual void releaseChild() noexcept = 0;  // The public object is gone (see the platform files).
    // The exit watch saw the exit (or failed): wakes every exit waiter, now and later.
    void exitObserved(Result<ExitStatus> status);
    [[nodiscard]] bool exitKnown() const noexcept { return m_exit.has_value(); }
    [[nodiscard]] bool releasedOrShutDown() const noexcept { return m_closed; }

    template<class T>
    void complete(Waiter<T>& waiter, Result<T> result) {
        *waiter.result = std::move(result);
        const std::coroutine_handle<> handle = std::exchange(waiter.handle, {});
        m_scheduler.enqueue(handle, waiter.lane);
    }

private:
    struct Output {
        std::optional<PipeStream> pipe;
        std::vector<char> buffer;
        std::size_t begin = 0;
        std::size_t end = 0;
        bool reading = false;
        bool eof = false;
        std::optional<Error> error;
        std::optional<Waiter<std::size_t>> waiter;
        std::span<char> waiterBuffer;
    };
    struct WriteOp {
        Waiter<void> waiter;
        std::shared_ptr<const std::string> bytes; // Kept alive until the write's handler ran.
        asio::cancellation_signal cancel;
        bool cancelRequested = false;
    };

    Output& output(OutputStream stream) noexcept { return stream == OutputStream::Stdout ? m_out : m_err; }
    void armRead(OutputStream stream);
    void readCompleted(OutputStream stream, const std::error_code& ec, std::size_t bytes);
    // Serves the waiting reader of `stream` from its buffer, EOF or error, if there is anything to give.
    void serveReader(Output& output);
    void writeCompleted(const std::error_code& ec);
    // Ends every pending operation with Cancelled and closes the pipes (release and shutdown).
    void closeEverything(std::string_view why) noexcept;
    void releaseOnStrand() noexcept;

    sched::IScheduler& m_scheduler;
    const ProcessId m_pid;
    const bool m_hasStdin;
    const bool m_hasStdout;
    const bool m_hasStderr;
    std::atomic<std::uint64_t> m_nextId{1};

    // Strand only.
    std::optional<PipeStream> m_in;
    Output m_out;
    Output m_err;
    std::unique_ptr<WriteOp> m_write;
    std::optional<Result<ExitStatus>> m_exit;
    std::vector<Waiter<ExitStatus>> m_exitWaiters;
    bool m_closed = false; // Released or shut down: no new operation starts.
};

/*
 * The public process object: forwards to coroutines that own a reference to the state, so a task keeps
 * working (and ends with Cancelled) if the object is destroyed while it runs.
 */
class AsyncProcess final : public IAsyncProcess {
public:
    explicit AsyncProcess(std::shared_ptr<AsyncProcessState> state) noexcept : m_state(std::move(state)) {}
    ~AsyncProcess() override; // Budget: B21 (never blocks).
    AsyncProcess(const AsyncProcess&) = delete ("a process has exactly one owner");
    AsyncProcess& operator=(const AsyncProcess&) = delete ("a process has exactly one owner");

    ProcessId pid() const noexcept override { return m_state->pid(); }
    sched::Task<Result<std::size_t>> readStdout(std::span<char> buffer, std::stop_token token) override;
    sched::Task<Result<std::size_t>> readStderr(std::span<char> buffer, std::stop_token token) override;
    sched::Task<Result<void>> writeStdin(std::string bytes, std::stop_token token) override;
    void closeStdin() noexcept override;
    sched::Task<Result<ExitStatus>> exited(std::stop_token token) override;
    Result<void> terminate() override { return m_state->terminateChild(); }
    sched::Task<Result<ProcessResult>> communicate(std::string stdinData, IClock::Duration timeout,
                                                   std::stop_token token) override;

private:
    std::shared_ptr<AsyncProcessState> m_state;
};

// Whether `ec` from a pipe read or write means the other end is gone (EOF for reads).
[[nodiscard]] bool isPipeEnd(const std::error_code& ec) noexcept;

} // namespace frost::io::detail
