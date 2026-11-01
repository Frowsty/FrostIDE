// SPDX-License-Identifier: Apache-2.0
//
// IAsyncProcess's platform-independent part (async_process_impl.hpp; ADR 0020): the read pumps, the
// stdin writer, the exit waiters, release and shutdown, the operation awaiters, and communicate().

#include "frost/core/assert.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/reactor.hpp"

#include "async_process_impl.hpp" // asio first.
#include "process_internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::io::detail {
namespace {

using namespace std::chrono_literals;

// How long communicate() waits for a terminated process to exit before returning Timeout (as IProcess).
constexpr IClock::Duration kReapGrace = 5s;

// An English Error for an OS error code from asio (system category: a Win32 code or an errno value).
[[nodiscard]] Error osError(const std::error_code& ec, std::string_view operation) {
#ifdef _WIN32
    return win32Error(static_cast<std::uint32_t>(ec.value()), operation);
#else
    return errnoError(ec.value(), operation);
#endif
}

[[nodiscard]] std::string_view streamName(OutputStream stream) noexcept {
    return stream == OutputStream::Stdout ? "readStdout" : "readStderr";
}

enum class OpKind : unsigned char { ReadStdout, ReadStderr, Write, Exit };

// A stop callback's body: posts the cancel of operation `id` to the strand (a no-op once it completed).
struct CancelOperation {
    std::shared_ptr<AsyncProcessState> state;
    std::uint64_t id = 0;
    OpKind kind = OpKind::Exit;

    void operator()() const noexcept {
        static_cast<void>(state->post([state = state, id = id, kind = kind] {
            switch (kind) {
            case OpKind::ReadStdout: state->readCancel(OutputStream::Stdout, id); break;
            case OpKind::ReadStderr: state->readCancel(OutputStream::Stderr, id); break;
            case OpKind::Write: state->writeCancel(id); break;
            case OpKind::Exit: state->exitCancel(id); break;
            }
        }));
    }
};

/*
 * `co_await Operation<T>(state, kind, token, start)` posts start(state, waiter, token) to the strand and
 * suspends until the strand completes the waiter (see async_process_impl.hpp). A token already stopped
 * completes at once with Cancelled, as does a refused post (the reactor is shutting down).
 */
template<class T, class Start>
class Operation {
public:
    Operation(std::shared_ptr<AsyncProcessState> state, OpKind kind, std::stop_token token, Start start)
        : m_state(std::move(state)), m_kind(kind), m_token(std::move(token)), m_start(std::move(start)) {}
    Operation(const Operation&) = delete ("the strand completes the awaiter in place");
    Operation& operator=(const Operation&) = delete ("the strand completes the awaiter in place");

    [[nodiscard]] bool await_ready() {
        if (m_token.stop_requested()) {
            m_result = fail(ErrorCode::Cancelled, "the operation was cancelled");
            return true;
        }
        return false;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        AsyncProcessState& state = *m_state;
        const Waiter<T> waiter{handle, state.scheduler().currentLane().value_or(sched::Lane::UserVisible),
                               state.nextId(), &m_result};
        // Before the post: a stop from now on posts a cancel, and the start job checks the token itself.
        if (m_token.stop_possible()) {
            m_callback.emplace(m_token, CancelOperation{m_state, waiter.id, m_kind});
        }
        const bool posted = state.post([state = m_state, waiter, token = m_token,
                                        start = std::move(m_start)]() mutable { start(*state, waiter, token); });
        if (!posted) {
            m_result = fail(ErrorCode::Cancelled, "the reactor is shut down");
            return false;
        }
        return true; // The coroutine may already run elsewhere: `this` is not touched any more.
    }

    [[nodiscard]] Result<T> await_resume() { return std::move(m_result); }

private:
    std::shared_ptr<AsyncProcessState> m_state;
    OpKind m_kind;
    std::stop_token m_token;
    Start m_start;
    Result<T> m_result{};
    std::optional<std::stop_callback<CancelOperation>> m_callback;
};

sched::Task<Result<std::size_t>> readTask(std::shared_ptr<AsyncProcessState> state, OutputStream stream,
                                          std::span<char> buffer, std::stop_token token) {
    if (!state->hasOutput(stream)) {
        co_return fail(ErrorCode::InvalidArgument, std::format("{}: the stream is not a pipe", streamName(stream)));
    }
    if (buffer.empty()) {
        co_return fail(ErrorCode::InvalidArgument, std::format("{}: the buffer is empty", streamName(stream)));
    }
    const OpKind kind = stream == OutputStream::Stdout ? OpKind::ReadStdout : OpKind::ReadStderr;
    auto start = [stream, buffer](AsyncProcessState& s, Waiter<std::size_t> waiter, const std::stop_token& t) {
        s.readStart(stream, buffer, waiter, t);
    };
    co_return co_await Operation<std::size_t, decltype(start)>(std::move(state), kind, std::move(token), start);
}

sched::Task<Result<void>> writeTask(std::shared_ptr<AsyncProcessState> state, std::string bytes,
                                    std::stop_token token) {
    if (!state->hasStdin()) {
        co_return fail(ErrorCode::InvalidArgument, "writeStdin: stdin is not a pipe");
    }
    auto shared = std::make_shared<const std::string>(std::move(bytes));
    auto start = [shared](AsyncProcessState& s, Waiter<void> waiter, const std::stop_token& t) {
        s.writeStart(shared, waiter, t);
    };
    co_return co_await Operation<void, decltype(start)>(std::move(state), OpKind::Write, std::move(token), start);
}

sched::Task<Result<ExitStatus>> exitTask(std::shared_ptr<AsyncProcessState> state, std::stop_token token) {
    auto start = [](AsyncProcessState& s, Waiter<ExitStatus> waiter, const std::stop_token& t) {
        s.exitStart(waiter, t);
    };
    co_return co_await Operation<ExitStatus, decltype(start)>(std::move(state), OpKind::Exit, std::move(token), start);
}

// Forwards a stop request into another stop source.
struct Forward {
    std::stop_source* target;
    void operator()() const noexcept { static_cast<void>(target->request_stop()); }
};

// Reads `stream` to EOF into a string (communicate()).
sched::Task<Result<std::string>> drainTask(std::shared_ptr<AsyncProcessState> state, OutputStream stream,
                                           std::stop_token token) {
    std::string collected;
    std::vector<char> chunk(kAsyncReadBufferBytes);
    for (;;) {
        auto got = co_await readTask(state, stream, chunk, token);
        if (!got) {
            co_return std::unexpected(std::move(got.error()));
        }
        if (*got == 0) {
            co_return collected;
        }
        collected.append(chunk.data(), *got);
    }
}

// Writes `data` and closes stdin (communicate()); a child that closed stdin early is not an error.
sched::Task<Result<void>> feedTask(std::shared_ptr<AsyncProcessState> state, std::string data, std::stop_token token) {
    Result<void> written{};
    if (!data.empty()) {
        written = co_await writeTask(state, std::move(data), token);
    }
    static_cast<void>(state->post([state] { state->closeStdinOnStrand(); }));
    if (!written && written.error().code() == ErrorCode::IoError) {
        co_return Result<void>{}; // The child closed its stdin: the rest is dropped (as IProcess::communicate).
    }
    co_return written;
}

// Requests `stop` and sets `fired` when `deadline` passes, unless `token` stops first.
sched::Task<void> deadlineTask(sched::IScheduler& scheduler, IClock::SteadyTime deadline, std::stop_token token,
                               std::stop_source& stop, std::atomic<bool>& fired) {
    if (co_await scheduler.sleepUntil(deadline, token)) {
        // Relaxed: the joiner reads it after co_await-ing this task, which orders it.
        fired.store(true, std::memory_order_relaxed);
        static_cast<void>(stop.request_stop());
    }
}

sched::Task<Result<ProcessResult>> communicateTask(std::shared_ptr<AsyncProcessState> state, std::string stdinData,
                                                   IClock::Duration timeout, std::stop_token token) {
    if (!stdinData.empty() && !state->hasStdin()) {
        co_return fail(ErrorCode::InvalidArgument, "communicate: stdin data given but stdin is not a pipe");
    }
    sched::IScheduler& scheduler = state->scheduler();
    const sched::Lane lane = sched::Lane::Background;
    // Every part stops on the caller's token or at the deadline.
    std::stop_source stop;
    std::stop_callback<Forward> fromCaller(token, Forward{&stop});
    std::stop_source timerStop;
    std::atomic<bool> timedOut{false};
    auto timer = scheduler.spawn(
        lane, deadlineTask(scheduler, scheduler.clock().steadyNow() + timeout, timerStop.get_token(), stop, timedOut));
    std::optional<sched::JoinHandle<Result<void>>> feeder;
    if (state->hasStdin()) {
        feeder.emplace(scheduler.spawn(lane, feedTask(state, std::move(stdinData), stop.get_token())));
    }
    std::optional<sched::JoinHandle<Result<std::string>>> outReader;
    std::optional<sched::JoinHandle<Result<std::string>>> errReader;
    if (state->hasOutput(OutputStream::Stdout)) {
        outReader.emplace(scheduler.spawn(lane, drainTask(state, OutputStream::Stdout, stop.get_token())));
    }
    if (state->hasOutput(OutputStream::Stderr)) {
        errReader.emplace(scheduler.spawn(lane, drainTask(state, OutputStream::Stderr, stop.get_token())));
    }
    Result<void> fed{};
    if (feeder) {
        fed = co_await std::move(*feeder);
    }
    Result<std::string> out{};
    if (outReader) {
        out = co_await std::move(*outReader);
    }
    Result<std::string> err{};
    if (errReader) {
        err = co_await std::move(*errReader);
    }
    Result<ExitStatus> status = co_await exitTask(state, stop.get_token());
    static_cast<void>(timerStop.request_stop());
    co_await std::move(timer);

    if (timedOut.load(std::memory_order_relaxed)) {
        static_cast<void>(state->terminateChild());
        // Wait up to kReapGrace for the terminated child to exit, then report the timeout either way.
        std::stop_source grace;
        std::atomic<bool> graceOver{false};
        std::stop_source graceTimerStop;
        auto graceTimer = scheduler.spawn(lane, deadlineTask(scheduler, scheduler.clock().steadyNow() + kReapGrace,
                                                             graceTimerStop.get_token(), grace, graceOver));
        static_cast<void>(co_await exitTask(state, grace.get_token()));
        static_cast<void>(graceTimerStop.request_stop());
        co_await std::move(graceTimer);
        co_return fail(ErrorCode::Timeout,
                       std::format("communicate: process {} did not finish within the timeout", state->pid()));
    }
    if (token.stop_requested()) {
        co_return fail(ErrorCode::Cancelled, "communicate was cancelled");
    }
    if (!fed) {
        co_return std::unexpected(std::move(fed.error()));
    }
    if (!out) {
        co_return std::unexpected(std::move(out.error()));
    }
    if (!err) {
        co_return std::unexpected(std::move(err.error()));
    }
    if (!status) {
        co_return std::unexpected(std::move(status.error()));
    }
    co_return ProcessResult{status->exitCode, status->signal, std::move(*out), std::move(*err)};
}

} // namespace

bool isPipeEnd(const std::error_code& ec) noexcept {
    if (ec == asio::error::eof || ec == asio::error::broken_pipe || ec == asio::error::connection_reset) {
        return true;
    }
#ifdef _WIN32
    if (ec.category() == std::system_category()) {
        switch (static_cast<DWORD>(ec.value())) {
        case ERROR_BROKEN_PIPE:
        case ERROR_HANDLE_EOF:
        case ERROR_NO_DATA:
        case ERROR_PIPE_NOT_CONNECTED: return true;
        default: break;
        }
    }
#endif
    return false;
}

Result<void> checkAsyncContext(const AsyncContext& context) {
    if (context.scheduler.callerDriven()) {
        return fail(ErrorCode::InvalidArgument,
                    "launchAsync: a caller-driven scheduler cannot take wakeups from reactor threads "
                    "(use ScriptedProcessLauncher)");
    }
    if (context.reactor.stats().shutDown) {
        return fail(ErrorCode::Cancelled, "launchAsync: the reactor is shut down");
    }
    return {};
}

// --- AsyncProcessState ---

AsyncProcessState::AsyncProcessState(Reactor::Impl& reactor, sched::IScheduler& scheduler, ProcessId pid, bool hasStdin,
                                     bool hasStdout, bool hasStderr)
    : BoundState(reactor), m_scheduler(scheduler), m_pid(pid), m_hasStdin(hasStdin), m_hasStdout(hasStdout),
      m_hasStderr(hasStderr) {}

void AsyncProcessState::adoptPipes(std::optional<PipeStream> in, std::optional<PipeStream> out,
                                   std::optional<PipeStream> err) {
    m_in = std::move(in);
    m_out.pipe = std::move(out);
    m_err.pipe = std::move(err);
    for (Output* output : {&m_out, &m_err}) {
        if (output->pipe) {
            output->buffer.resize(kAsyncReadBufferBytes);
        }
    }
}

void AsyncProcessState::startOnStrand() {
    FROST_PRE(onStrand());
    if (m_closed) {
        return;
    }
    armRead(OutputStream::Stdout);
    armRead(OutputStream::Stderr);
    startExitWatch();
}

void AsyncProcessState::armRead(OutputStream stream) {
    Output& o = output(stream);
    if (m_closed || !o.pipe || !o.pipe->is_open() || o.reading || o.eof || o.error || o.begin < o.end) {
        return;
    }
    o.reading = true;
    o.pipe->async_read_some(asio::buffer(o.buffer),
                            bindHandler([this, stream](const std::error_code& ec, std::size_t bytes) {
                                readCompleted(stream, ec, bytes);
                            }));
}

void AsyncProcessState::readCompleted(OutputStream stream, const std::error_code& ec, std::size_t bytes) {
    Output& o = output(stream);
    o.reading = false;
    if (!ec) {
        o.begin = 0;
        o.end = bytes; // A zero-byte read (the child wrote zero bytes) just re-arms.
    } else if (isPipeEnd(ec)) {
        o.eof = true;
    } else if (ec == asio::error::operation_aborted) {
        // The pipe was closed (release, shutdown): the waiter, if any, was completed then.
        return;
    } else {
        o.error = osError(ec, streamName(stream));
    }
    serveReader(o);
    armRead(stream);
}

void AsyncProcessState::serveReader(Output& o) {
    if (!o.waiter) {
        return;
    }
    if (o.begin < o.end) {
        const std::size_t count = std::min(o.end - o.begin, o.waiterBuffer.size());
        std::memcpy(o.waiterBuffer.data(), o.buffer.data() + o.begin, count);
        o.begin += count;
        if (o.begin == o.end) {
            o.begin = o.end = 0;
        }
        complete(*o.waiter, Result<std::size_t>(count));
    } else if (o.error) {
        complete(*o.waiter, Result<std::size_t>(std::unexpected(*o.error)));
    } else if (o.eof) {
        complete(*o.waiter, Result<std::size_t>(std::size_t{0}));
    } else {
        return;
    }
    o.waiter.reset();
}

void AsyncProcessState::readStart(OutputStream stream, std::span<char> buffer, Waiter<std::size_t> waiter,
                                  const std::stop_token& token) {
    Output& o = output(stream);
    if (token.stop_requested() || m_closed) {
        complete(waiter,
                 Result<std::size_t>(fail(ErrorCode::Cancelled, std::format("{} was cancelled", streamName(stream)))));
        return;
    }
    FROST_PRE(!o.waiter); // One reader per stream (async_process.hpp).
    o.waiter = waiter;
    o.waiterBuffer = buffer;
    serveReader(o);
    armRead(stream);
}

void AsyncProcessState::readCancel(OutputStream stream, std::uint64_t id) {
    Output& o = output(stream);
    if (o.waiter && o.waiter->id == id) {
        // The OS read stays in flight; what it brings is kept for the next read.
        complete(*o.waiter,
                 Result<std::size_t>(fail(ErrorCode::Cancelled, std::format("{} was cancelled", streamName(stream)))));
        o.waiter.reset();
    }
}

void AsyncProcessState::writeStart(std::shared_ptr<const std::string> bytes, Waiter<void> waiter,
                                   const std::stop_token& token) {
    if (token.stop_requested() || m_closed) {
        complete(waiter, Result<void>(fail(ErrorCode::Cancelled, "writeStdin was cancelled")));
        return;
    }
    if (!m_in || !m_in->is_open()) {
        complete(waiter, Result<void>(fail(ErrorCode::InvalidArgument, "writeStdin: stdin is not an open pipe")));
        return;
    }
    FROST_PRE(!m_write); // One writer at a time (async_process.hpp).
    if (bytes->empty()) {
        complete(waiter, Result<void>{});
        return;
    }
    m_write = std::make_unique<WriteOp>();
    m_write->waiter = waiter;
    m_write->bytes = std::move(bytes);
    // Initiated on the strand, so the speculative first write runs on this reactor thread (SIGPIPE blocked).
    asio::async_write(*m_in, asio::buffer(*m_write->bytes),
                      asio::bind_cancellation_slot(
                          m_write->cancel.slot(),
                          bindHandler([this](const std::error_code& ec, std::size_t) { writeCompleted(ec); })));
}

void AsyncProcessState::writeCompleted(const std::error_code& ec) {
    std::unique_ptr<WriteOp> op = std::move(m_write);
    if (!op || !op->waiter.handle) {
        return; // Completed already by release or shutdown; `op` kept the bytes alive until now.
    }
    if (!ec) {
        complete(op->waiter, Result<void>{});
    } else if (op->cancelRequested && ec == asio::error::operation_aborted) {
        // A torn write cannot be resumed: stdin closes (async_process.hpp).
        closeStdinOnStrand();
        complete(op->waiter, Result<void>(fail(ErrorCode::Cancelled, "writeStdin was cancelled; stdin is closed")));
    } else if (ec == asio::error::operation_aborted) {
        complete(op->waiter, Result<void>(fail(ErrorCode::IoError, "writeStdin: stdin was closed")));
    } else if (isPipeEnd(ec)) {
        complete(op->waiter, Result<void>(fail(ErrorCode::IoError, "writeStdin: the child closed its stdin")));
    } else {
        complete(op->waiter, Result<void>(std::unexpected(osError(ec, "writeStdin"))));
    }
}

void AsyncProcessState::writeCancel(std::uint64_t id) {
    if (m_write && m_write->waiter.handle && m_write->waiter.id == id && !m_write->cancelRequested) {
        m_write->cancelRequested = true;
        m_write->cancel.emit(asio::cancellation_type::terminal);
    }
}

void AsyncProcessState::closeStdinOnStrand() noexcept {
    if (m_in && m_in->is_open()) {
        std::error_code ignored;
        static_cast<void>(m_in->close(ignored)); // A pending write completes with operation_aborted.
    }
}

void AsyncProcessState::exitStart(Waiter<ExitStatus> waiter, const std::stop_token& token) {
    if (m_exit) {
        complete(waiter, Result<ExitStatus>(*m_exit));
        return;
    }
    if (token.stop_requested() || m_closed) {
        complete(waiter, Result<ExitStatus>(fail(ErrorCode::Cancelled, "exited() was cancelled")));
        return;
    }
    m_exitWaiters.push_back(waiter);
}

void AsyncProcessState::exitCancel(std::uint64_t id) {
    const auto it = std::ranges::find(m_exitWaiters, id, &Waiter<ExitStatus>::id);
    if (it != m_exitWaiters.end()) {
        Waiter<ExitStatus> waiter = *it;
        m_exitWaiters.erase(it);
        complete(waiter, Result<ExitStatus>(fail(ErrorCode::Cancelled, "exited() was cancelled")));
    }
}

void AsyncProcessState::exitObserved(Result<ExitStatus> status) {
    m_exit = std::move(status);
    std::vector<Waiter<ExitStatus>> waiters = std::exchange(m_exitWaiters, {});
    for (Waiter<ExitStatus>& waiter : waiters) {
        complete(waiter, Result<ExitStatus>(*m_exit));
    }
}

void AsyncProcessState::closeEverything(std::string_view why) noexcept {
    m_closed = true;
    const auto cancelled = [why] { return Error(ErrorCode::Cancelled, std::string(why)); };
    for (Output* o : {&m_out, &m_err}) {
        if (o->waiter) {
            complete(*o->waiter, Result<std::size_t>(std::unexpected(cancelled())));
            o->waiter.reset();
        }
        if (o->pipe && o->pipe->is_open()) {
            std::error_code ignored;
            static_cast<void>(o->pipe->close(ignored));
        }
    }
    if (m_write && m_write->waiter.handle) {
        // Its handler still runs (with operation_aborted) and drops the bytes then.
        complete(m_write->waiter, Result<void>(std::unexpected(cancelled())));
    }
    closeStdinOnStrand();
    std::vector<Waiter<ExitStatus>> waiters = std::exchange(m_exitWaiters, {});
    for (Waiter<ExitStatus>& waiter : waiters) {
        complete(waiter, Result<ExitStatus>(std::unexpected(cancelled())));
    }
}

void AsyncProcessState::releaseOnStrand() noexcept {
    closeEverything("the process object was destroyed");
    releaseChild();
}

void AsyncProcessState::shutdownOnStrand() noexcept {
    closeEverything("the reactor is shut down");
    stopExitWatch();
}

void AsyncProcessState::release() noexcept {
    // Budget: B21: a post, never a wait. After shutdown nothing runs on the strand any more: the
    // platform destructor then deals with the child when the last reference drops.
    static_cast<void>(post([this] { releaseOnStrand(); }));
}

// --- AsyncProcess ---

AsyncProcess::~AsyncProcess() { m_state->release(); }

sched::Task<Result<std::size_t>> AsyncProcess::readStdout(std::span<char> buffer, std::stop_token token) {
    return readTask(m_state, OutputStream::Stdout, buffer, std::move(token));
}

sched::Task<Result<std::size_t>> AsyncProcess::readStderr(std::span<char> buffer, std::stop_token token) {
    return readTask(m_state, OutputStream::Stderr, buffer, std::move(token));
}

sched::Task<Result<void>> AsyncProcess::writeStdin(std::string bytes, std::stop_token token) {
    return writeTask(m_state, std::move(bytes), std::move(token));
}

void AsyncProcess::closeStdin() noexcept {
    static_cast<void>(m_state->post([state = m_state] { state->closeStdinOnStrand(); }));
}

sched::Task<Result<ExitStatus>> AsyncProcess::exited(std::stop_token token) {
    return exitTask(m_state, std::move(token));
}

sched::Task<Result<ProcessResult>> AsyncProcess::communicate(std::string stdinData, IClock::Duration timeout,
                                                             std::stop_token token) {
    return communicateTask(m_state, std::move(stdinData), timeout, std::move(token));
}

} // namespace frost::io::detail
