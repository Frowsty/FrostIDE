// SPDX-License-Identifier: Apache-2.0
//
// Test doubles for frost::lang's protocol tests: MemoryPipe (an in-memory duplex byte stream whose
// client end is an IByteStream driven by a scheduler and whose server end is blocking) and
// FakeServer (a scripted language server on its own thread that reads framed JSON-RPC messages and
// can answer, reorder, garble or drop them), plus LangEnv (a two-worker ThreadPool).
#pragma once

#include "frost/core/mutex.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/jsonrpc.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"
#include "frost/sched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace frost::test {

// A ThreadPool for the protocol tests; shut down (drained) on destruction.
class LangEnv {
public:
    LangEnv() : m_pool(sched::ThreadPoolOptions{.workerCount = 2}) {}
    ~LangEnv() { m_pool.shutdown(); }
    LangEnv(const LangEnv&) = delete ("owns a pool");
    LangEnv& operator=(const LangEnv&) = delete ("owns a pool");

    [[nodiscard]] sched::ThreadPool& pool() { return m_pool; }

    // Runs `task` on a pool worker and waits for it on the test thread.
    template<class T>
    T run(sched::Task<T> task) {
        return m_pool.spawn(sched::Lane::UserVisible, std::move(task)).join();
    }

private:
    sched::ThreadPool m_pool;
};

/*
 * Both directions of a connection in memory. Client end (clientStream()): reads suspend until the
 * server writes (woken through the scheduler), writes complete at once. Server end: blocking calls
 * for a test thread. Thread-safe.
 */
class MemoryPipe {
public:
    explicit MemoryPipe(sched::IScheduler& scheduler) : m_state(std::make_shared<State>(scheduler)) {}

    [[nodiscard]] std::unique_ptr<lang::IByteStream> clientStream() { return std::make_unique<ClientStream>(m_state); }

    // Server -> client.
    void serverWrite(std::string_view bytes) {
        std::coroutine_handle<> waiter;
        {
            LockGuard lock(m_state->mutex);
            m_state->toClient.append(bytes);
            waiter = std::exchange(m_state->waiter, {});
        }
        if (waiter) {
            m_state->scheduler.enqueue(waiter, sched::Lane::UserVisible);
        }
    }
    // The client reads EOF once the buffered bytes are gone.
    void serverCloseOutput() {
        std::coroutine_handle<> waiter;
        {
            LockGuard lock(m_state->mutex);
            m_state->toClientClosed = true;
            waiter = std::exchange(m_state->waiter, {});
        }
        if (waiter) {
            m_state->scheduler.enqueue(waiter, sched::Lane::UserVisible);
        }
    }
    // Client -> server: whatever the client wrote, waiting up to `timeout` for at least one byte.
    // nullopt when nothing arrived (timeout) or the client closed its write side and all was read.
    [[nodiscard]] std::optional<std::string> serverRead(std::chrono::milliseconds timeout) {
        UniqueLock lock(m_state->mutex);
        m_state->changed.wait_for(lock.native(), timeout, [&]() FROST_NO_THREAD_SAFETY_ANALYSIS {
            return !m_state->toServer.empty() || m_state->toServerClosed;
        });
        if (m_state->toServer.empty()) {
            return std::nullopt;
        }
        return std::exchange(m_state->toServer, {});
    }
    [[nodiscard]] bool clientClosedWrite() const {
        LockGuard lock(m_state->mutex);
        return m_state->toServerClosed;
    }

private:
    struct State {
        explicit State(sched::IScheduler& s) : scheduler(s) {}
        sched::IScheduler& scheduler;
        mutable Mutex mutex;
        std::condition_variable changed;
        std::string toClient FROST_GUARDED_BY(mutex);
        bool toClientClosed FROST_GUARDED_BY(mutex) = false;
        std::coroutine_handle<> waiter FROST_GUARDED_BY(mutex);
        std::string toServer FROST_GUARDED_BY(mutex);
        bool toServerClosed FROST_GUARDED_BY(mutex) = false;
    };

    // Wakes a suspended read when the read's token is stopped.
    struct Wake {
        std::shared_ptr<State> state;
        void operator()() const {
            std::coroutine_handle<> waiter;
            {
                LockGuard lock(state->mutex);
                waiter = std::exchange(state->waiter, {});
            }
            if (waiter) {
                state->scheduler.enqueue(waiter, sched::Lane::UserVisible);
            }
        }
    };

    struct WaitAwaiter {
        std::shared_ptr<State> state;
        std::stop_token token;
        std::optional<std::stop_callback<Wake>> onStop;

        [[nodiscard]] bool await_ready() const noexcept { return false; }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
            if (token.stop_possible()) {
                onStop.emplace(token, Wake{state}); // Registered first; it finds no waiter if it fires now.
            }
            LockGuard lock(state->mutex);
            if (!state->toClient.empty() || state->toClientClosed || token.stop_requested()) {
                return false;
            }
            state->waiter = handle;
            return true;
        }
        void await_resume() { onStop.reset(); }
    };

    class ClientStream final : public lang::IByteStream {
    public:
        explicit ClientStream(std::shared_ptr<State> state) : m_state(std::move(state)) {}

        sched::Task<Result<std::size_t>> read(std::span<char> buffer, std::stop_token token) override {
            std::shared_ptr<State> state = m_state;
            for (;;) {
                std::size_t copied = 0;
                bool eof = false;
                {
                    LockGuard lock(state->mutex);
                    if (!state->toClient.empty()) {
                        copied = std::min(buffer.size(), state->toClient.size());
                        std::memcpy(buffer.data(), state->toClient.data(), copied);
                        state->toClient.erase(0, copied);
                    } else if (state->toClientClosed) {
                        eof = true;
                    }
                }
                if (copied > 0 || eof) {
                    co_return copied;
                }
                if (token.stop_requested()) {
                    co_return fail(ErrorCode::Cancelled, "read cancelled");
                }
                co_await WaitAwaiter{state, token, std::nullopt};
            }
        }

        sched::Task<Result<void>> write(std::string bytes, std::stop_token /*token*/) override {
            {
                LockGuard lock(m_state->mutex);
                if (m_state->toServerClosed) {
                    co_return fail(ErrorCode::IoError, "write after closeWrite");
                }
                m_state->toServer += bytes;
            }
            m_state->changed.notify_all();
            co_return Result<void>{};
        }

        void closeWrite() noexcept override {
            {
                LockGuard lock(m_state->mutex);
                m_state->toServerClosed = true;
            }
            m_state->changed.notify_all();
        }

    private:
        std::shared_ptr<State> m_state;
    };

    std::shared_ptr<State> m_state;
};

/*
 * A scripted server: a thread that decodes the client's frames and hands each parsed message to
 * the test's script, which answers through the FakeServer's methods. The thread ends when the
 * client closes its write side, when the script returns false, or on destruction.
 */
class FakeServer {
public:
    // Returns false to stop serving (the thread then exits without closing the output).
    using Script = std::function<bool(FakeServer& server, const lang::Json& message)>;

    FakeServer(MemoryPipe& pipe, Script script) : m_pipe(pipe), m_script(std::move(script)) {
        m_thread = std::jthread([this](std::stop_token stop) { serve(stop); });
    }
    ~FakeServer() {
        m_thread.request_stop();
        m_thread.join();
    }
    FakeServer(const FakeServer&) = delete ("owns a thread");
    FakeServer& operator=(const FakeServer&) = delete ("owns a thread");

    void send(const lang::Json& message) { m_pipe.serverWrite(lang::encodeFrame(message.dump())); }
    void sendRaw(std::string_view bytes) { m_pipe.serverWrite(bytes); }
    void closeOutput() { m_pipe.serverCloseOutput(); }

    void respond(const lang::Json& id, lang::Json result) {
        send(lang::Json::object().set("jsonrpc", "2.0").set("id", id).set("result", std::move(result)));
    }
    void notifyClient(std::string_view method, lang::Json params) {
        send(lang::Json::object().set("jsonrpc", "2.0").set("method", method).set("params", std::move(params)));
    }
    void requestClient(std::int64_t id, std::string_view method, lang::Json params) {
        send(lang::Json::object()
                 .set("jsonrpc", "2.0")
                 .set("id", id)
                 .set("method", method)
                 .set("params", std::move(params)));
    }

    // Every message received so far (a copy).
    [[nodiscard]] std::vector<lang::Json> received() const {
        LockGuard lock(m_mutex);
        return m_received;
    }
    // Waits until `count` messages arrived (true) or `timeout` passed (false). The count is checked
    // before the deadline on every round, so the check that ended the wait decides (also for a zero
    // timeout), and a deadline that passes during the sleep still gets one last check.
    [[nodiscard]] bool waitForMessages(std::size_t count,
                                       std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            {
                LockGuard lock(m_mutex);
                if (m_received.size() >= count) {
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

private:
    void serve(const std::stop_token& stop) {
        lang::FrameDecoder decoder;
        while (!stop.stop_requested()) {
            std::optional<std::string> bytes = m_pipe.serverRead(std::chrono::milliseconds(20));
            if (!bytes) {
                if (m_pipe.clientClosedWrite()) {
                    return;
                }
                continue;
            }
            decoder.feed(*bytes);
            while (auto frame = decoder.next()) {
                if (!*frame) {
                    ADD_FAILURE() << "the client sent a malformed frame: " << frame->error().describe();
                    continue;
                }
                auto message = lang::parseJson(**frame);
                if (!message) {
                    ADD_FAILURE() << "the client sent invalid JSON: " << message.error().describe();
                    continue;
                }
                {
                    LockGuard lock(m_mutex);
                    m_received.push_back(*message);
                }
                if (m_script && !m_script(*this, *message)) {
                    return;
                }
            }
        }
    }

    MemoryPipe& m_pipe;
    Script m_script;
    mutable Mutex m_mutex;
    std::vector<lang::Json> m_received FROST_GUARDED_BY(m_mutex);
    std::jthread m_thread;
};

// Polls `condition` every 2 ms until it holds (true) or `timeout` passes (false).
template<class Condition>
[[nodiscard]] bool eventually(Condition condition, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

} // namespace frost::test
