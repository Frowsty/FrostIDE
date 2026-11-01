// SPDX-License-Identifier: Apache-2.0
//
/*
 * Reactor: frost::io's asynchronous I/O engine (PLAN.md sections 2 "Scheduler" and 3.3: 1-2 reactor
 * threads for all pipes and sockets; ADR 0020). One asio io_context (IOCP on Windows, epoll on Linux)
 * run by 1 or 2 threads. File watches (file_watcher.hpp) and async child processes (async_process.hpp)
 * bind their OS state to a Reactor; completions resume coroutines through IScheduler::enqueue, so no
 * reactor thread ever runs user code.
 */
/*
 * Model. Every object bound to a reactor owns a shared state with its own asio strand: its OS handles
 * are touched only on that strand, and every asio initiation is posted to it, so speculative reads and
 * writes run on reactor threads (where SIGPIPE is blocked on POSIX). The states are shared_ptr-owned,
 * so a completion that arrives after the public object is gone never touches freed memory. The app may
 * create a second Reactor for watchers, which gives them the "dedicated watcher thread" of PLAN.md 3.3.
 * On Windows asio adds an internal timer thread once a timer is used; it is not a reactor thread for
 * onReactorThread().
 *
 * Lifecycle. A lock guards a `shutDown` flag and the registry of bound states. Binding a state and every
 * post made from outside a state's strand check the flag under that lock: after shutdown a bind fails
 * with Cancelled and a post is not made. A state destroyed after shutdown closes its OS handles inline.
 *
 * Exit order (documented here and asserted where possible):
 *   1. Reactor::shutdown(): consumers' pending next() calls end with Cancelled and their tasks finish;
 *   2. destroy the objects bound to the reactor (watches, then watchers);
 *   3. shut down the schedulers they resume on;
 *   4. destroy the Reactor (asserts that nothing is bound any more).
 * The order matters because ThreadPool::shutdown refuses enqueue() from foreign threads once it has
 * begun (reactor threads, MemFs mutator threads and stop callbacks included), and its drain does not
 * wait for coroutines suspended on external events. Every pool task io starts (polling loops, inotify
 * setup) also watches scheduler.stopToken(), so the drain cannot hang.
 *
 * Thread safety: create(), shutdown() and the destructor run on the owner thread; stats() and
 * onReactorThread() on any thread.
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace frost::io {

struct ReactorOptions {
    unsigned threads = 1;               // 1 or 2 (PLAN.md 3.3); other values fail with InvalidArgument.
    std::string name = "frost-reactor"; // Threads are named "<name>-<i>" (cut to 15 bytes on Linux).
};

// A snapshot for tests and diagnostics; fields may be stale on return.
struct ReactorStats {
    // Live shared states bound to this reactor (watch states, inotify instances, async processes).
    std::int64_t boundObjects = 0;
    std::uint64_t handlersRun = 0; // Completion handlers frost::io ran so far (relaxed counter, tests only).
    bool shutDown = false;
};

class Reactor {
public:
    /*
     * Starts the io_context and its threads (one by one; if one fails to start, the ones already
     * started are stopped and joined). Errors: InvalidArgument (threads not 1 or 2), IoError (the
     * io_context or a thread could not be created). O(threads).
     */
    [[nodiscard]] static Result<std::unique_ptr<Reactor>> create(ReactorOptions options = {});

    // shutdown(), then asserts that no object is bound any more (a contract violation otherwise).
    ~Reactor();
    Reactor(const Reactor&) = delete ("objects bound to a reactor refer to it");
    Reactor& operator=(const Reactor&) = delete ("objects bound to a reactor refer to it");

    /*
     * Idempotent; owner thread only, never on a reactor thread (checked). Sets the shutdown flag, posts
     * every live state's shutdown to its strand (cancel OS operations and timers, close handles, close
     * watch queues with Cancelled), drops the work guard so the io_context drains (every post made
     * before the flag still runs, and outstanding overlapped operations count as work until their
     * cancellations complete), then joins the threads. It blocks only while pending operations report
     * their cancellation. O(bound states). Budget: B20.
     */
    void shutdown();

    // True on one of this reactor's threads.
    [[nodiscard]] bool onReactorThread() const noexcept;

    [[nodiscard]] ReactorStats stats() const noexcept;

    struct Impl; // src/io/src/reactor_impl.hpp; frost::io internals only.
    [[nodiscard]] Impl& impl() noexcept { return *m_impl; }

private:
    explicit Reactor(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

} // namespace frost::io
