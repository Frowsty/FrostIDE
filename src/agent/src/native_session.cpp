// SPDX-License-Identifier: Apache-2.0
// No GUI thread waits, unbounded communicate(), shell shims or detached raw-owner callbacks.
#include "frost/agent/native_session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
namespace frost::agent {
struct NativeSession::State {
    io::AsyncContext context;
    std::shared_ptr<io::IAsyncProcess> process;
    Observer observer;
    std::stop_source stop;
    std::mutex mutex;
    std::deque<std::string> pending;
    std::size_t queuedBytes = 0;
    bool writing = false;
    bool stdoutDone = false, stderrDone = false, exitNotified = false;
    std::optional<io::ExitStatus> exitStatus;
    std::atomic<bool> ended{false};
    State(io::AsyncContext context, std::unique_ptr<io::IAsyncProcess> process, Observer observer)
        : context(context), process(std::move(process)), observer(std::move(observer)) {}
    void maybeExited() {
        std::optional<io::ExitStatus> status;
        {
            std::lock_guard lock(mutex);
            if (!stdoutDone || !stderrDone || !exitStatus || exitNotified)
                return;
            exitNotified = true;
            ended = true;
            status = exitStatus;
        }
        if (observer.exited)
            observer.exited(*status);
    }
    void fail(Error error) {
        if (ended.exchange(true))
            return;
        stop.request_stop();
        process->closeStdin();
        (void)process->terminate();
        if (observer.failure)
            observer.failure(std::move(error));
    }
};
NativeSession::NativeSession(std::shared_ptr<State> state) : m(std::move(state)) {}
Result<std::unique_ptr<NativeSession>> NativeSession::start(io::IProcessLauncher& launcher, io::AsyncContext context,
                                                            const NativeLaunch& launch, const Json& auth,
                                                            Observer observer) {
    auto spec = nativeLaunch(launch, auth);
    if (!spec)
        return std::unexpected(spec.error());
    auto process = launcher.launchAsync(*spec, context);
    if (!process)
        return std::unexpected(process.error());
    auto state = std::make_shared<State>(context, std::move(*process), std::move(observer));
    context.scheduler.spawn(sched::Lane::UserVisible, read(state, false)).detach();
    context.scheduler.spawn(sched::Lane::UserVisible, read(state, true)).detach();
    context.scheduler.spawn(sched::Lane::UserVisible, wait(state)).detach();
    return std::unique_ptr<NativeSession>(new NativeSession(std::move(state)));
}
NativeSession::~NativeSession() { stop(); }
void NativeSession::stop() {
    if (m->ended.exchange(true))
        return;
    m->stop.request_stop();
    m->process->closeStdin();
    (void)m->process->terminate();
}
Result<void> NativeSession::send(const Json& event) {
    if (m->ended || !event.isObject())
        return fail(ErrorCode::Cancelled, "native session is closed");
    auto bytes = event.dump();
    if (bytes.size() > NdjsonFramer::kMaximumLine)
        return fail(ErrorCode::OutOfRange, "native event exceeds line cap");
    bytes += '\n';
    bool startWriter = false;
    {
        std::lock_guard lock(m->mutex);
        if (m->ended)
            return fail(ErrorCode::Cancelled, "native session ended before queue admission");
        if (m->pending.size() >= 32 || m->queuedBytes + bytes.size() > 2 * NdjsonFramer::kMaximumLine)
            return fail(ErrorCode::Busy, "native outbound queue is full");
        m->queuedBytes += bytes.size();
        m->pending.push_back(std::move(bytes));
        if (!m->writing) {
            m->writing = true;
            startWriter = true;
        }
    }
    if (startWriter)
        m->context.scheduler.spawn(sched::Lane::UserVisible, write(m)).detach();
    return {};
}
sched::Task<void> NativeSession::write(std::shared_ptr<State> state) {
    for (;;) {
        std::string bytes;
        {
            std::lock_guard lock(state->mutex);
            if (state->pending.empty() || state->ended) {
                state->writing = false;
                co_return;
            }
            bytes = std::move(state->pending.front());
            state->pending.pop_front();
            state->queuedBytes -= bytes.size();
        }
        auto written = co_await state->process->writeStdin(std::move(bytes), state->stop.get_token());
        if (!written) {
            if (!state->ended)
                state->fail(written.error());
            co_return;
        }
    }
}
sched::Task<void> NativeSession::read(std::shared_ptr<State> state, bool stderrStream) {
    std::array<char, 4096> bytes{};
    NdjsonFramer framer;
    std::size_t stderrBytes = 0;
    for (;;) {
        auto count = stderrStream ? co_await state->process->readStderr(bytes, state->stop.get_token())
                                  : co_await state->process->readStdout(bytes, state->stop.get_token());
        if (!count) {
            if (!state->ended)
                state->fail(count.error());
            co_return;
        }
        if (!*count)
            break;
        if (stderrStream) {
            // Continue draining beyond the display cap; never deadlock the child on stderr.
            const auto admitted = std::min(*count, 65536 - std::min<std::size_t>(65536, stderrBytes));
            if (admitted && state->observer.stderrChunk)
                state->observer.stderrChunk(std::string(bytes.data(), admitted));
            stderrBytes += admitted;
        } else {
            auto events = framer.feed(std::string_view(bytes.data(), *count));
            if (!events) {
                state->fail(events.error());
                co_return;
            }
            for (auto& event : *events)
                if (!state->ended && state->observer.event && !state->observer.event(std::move(event))) {
                    state->fail(Error(ErrorCode::OutOfRange, "native UI event queue exceeded its bounded capacity"));
                    co_return;
                }
        }
    }
    if (!stderrStream && !state->ended) {
        auto finished = framer.finish();
        if (!finished)
            state->fail(finished.error());
    }
    {
        std::lock_guard lock(state->mutex);
        (stderrStream ? state->stderrDone : state->stdoutDone) = true;
    }
    state->maybeExited();
}
sched::Task<void> NativeSession::wait(std::shared_ptr<State> state) {
    const auto exited = co_await state->process->exited(state->stop.get_token());
    if (exited) {
        {
            std::lock_guard lock(state->mutex);
            state->exitStatus = *exited;
        }
        state->maybeExited();
    } else if (!exited && !state->ended)
        state->fail(exited.error());
}
} // namespace frost::agent
