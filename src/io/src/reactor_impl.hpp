// SPDX-License-Identifier: Apache-2.0
//
/*
 * The Reactor's internals (frost::io only; ADR 0020): the asio io_context and its threads, the
 * lifecycle lock, and BoundState, the base of every shared state bound to a reactor (watch states, the
 * inotify instance, async processes). This is the only header that includes asio; it is included by
 * io .cpp files only, never by tests or public headers, and before <windows.h> (asio includes winsock2.h
 * first).
 */
/*
 * asio is used standalone and header-only (ASIO_STANDALONE, ASIO_NO_DEPRECATED,
 * ASIO_NO_DEFAULT_LINKED_LIBS; src/io/CMakeLists.txt) and only through the error_code overloads. The
 * few calls without one (steady_timer::expires_after, strand and executor construction, the
 * allocations of post and overlapped_ptr) throw only on allocation failure or an OS timer failure: in
 * a handler that is a bug (each reactor thread catches it and reports a contract violation); in a
 * Task<Result<T>> body it becomes ErrorCode::Internal (task.hpp). No exception leaves frost::io.
 *
 * The code follows asio's documentation and examples (BSL-1.0; https://think-async.com/Asio/), which
 * may be consulted; no example code is copied.
 */
#pragma once

#include "frost/core/assert.hpp"
#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"

#include <asio/bind_executor.hpp>
#include <asio/dispatch.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace frost::io {

namespace detail {
class BoundState;
} // namespace detail

using ReactorStrand = asio::strand<asio::io_context::executor_type>;

struct Reactor::Impl {
    // Throws std::system_error if the OS refuses the completion port or epoll instance (Reactor::create
    // catches it). `threads` is the concurrency hint: other threads post too, so never the UNSAFE hints.
    explicit Impl(ReactorOptions reactorOptions);
    Impl(const Impl&) = delete ("the io_context has one owner");
    Impl& operator=(const Impl&) = delete ("the io_context has one owner");
    ~Impl() = default;

    // Registers `state` (constructed with this reactor) unless shutdown has begun. Errors: Cancelled.
    [[nodiscard]] Result<void> bind(const std::shared_ptr<detail::BoundState>& state);

    // Counts one completion handler for stats(); relaxed: a statistic only, no ordering needed.
    void countHandler() noexcept { handlersRun.fetch_add(1, std::memory_order_relaxed); }

    ReactorOptions options;
    asio::io_context context;
    // Keeps run() going while nothing is pending; reset by shutdown() so run() drains and returns.
    // Touched only by the owner thread (create, shutdown).
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::vector<std::thread> threads; // Owner thread only.
    bool joined = false;              // Owner thread only: shutdown() has joined the threads.

    // Guards the shutdown flag and the registry. Every post from outside a state's strand checks the
    // flag under it, so nothing is posted after shutdown() has started its drain.
    Mutex lifecycle;
    bool shutDown FROST_GUARDED_BY(lifecycle) = false;
    std::vector<std::weak_ptr<detail::BoundState>> bound FROST_GUARDED_BY(lifecycle);
    std::size_t pruneAt FROST_GUARDED_BY(lifecycle) = 16; // Registry size at which expired entries are pruned.

    // Live states; incremented in BoundState's constructor and decremented in its destructor. Relaxed
    // increments; the decrement is release and ~Reactor's read acquire, so a state's teardown
    // happens-before the reactor's own.
    std::atomic<std::int64_t> boundObjects{0};
    std::atomic<std::uint64_t> handlersRun{0}; // Relaxed statistic.
    std::atomic<bool> shutDownFlag{false};     // Mirrors shutDown for stats() (relaxed; a snapshot).
};

namespace detail {

// The reactor whose handler the calling thread is running, or null (set by the reactor threads).
[[nodiscard]] Reactor::Impl* currentReactor() noexcept;

/*
 * The base of every shared state bound to a reactor. It owns a strand; derived classes touch their OS
 * state only on it. Created with std::make_shared and then registered with Reactor::Impl::bind (see
 * makeBound). Lifetime: shared by the public object, every in-flight handler and Background task; the
 * destructor runs wherever the last reference drops (normally a reactor thread) and must close any OS
 * handle still open, because after shutdown no reactor thread runs any more.
 */
class BoundState : public std::enable_shared_from_this<BoundState> {
public:
    explicit BoundState(Reactor::Impl& reactor) : m_reactor(reactor), m_strand(asio::make_strand(reactor.context)) {
        // Relaxed: only the final count matters (see Reactor::Impl::boundObjects).
        reactor.boundObjects.fetch_add(1, std::memory_order_relaxed);
    }
    BoundState(const BoundState&) = delete ("a bound state is shared by pointer");
    BoundState& operator=(const BoundState&) = delete ("a bound state is shared by pointer");
    virtual ~BoundState() {
        // Release: the teardown above happens-before ~Reactor's acquire load of the count.
        m_reactor.boundObjects.fetch_sub(1, std::memory_order_release);
    }

    // Runs on the strand during Reactor::shutdown: cancel OS operations and timers, close handles and
    // close watch queues with Cancelled. Must leave the state so that no handler re-arms.
    virtual void shutdownOnStrand() noexcept = 0;

    [[nodiscard]] Reactor::Impl& reactor() const noexcept { return m_reactor; }
    [[nodiscard]] ReactorStrand& strand() noexcept { return m_strand; }
    [[nodiscard]] bool onStrand() const noexcept { return m_strand.running_in_this_thread(); }

    /*
     * Posts `fn` to the strand from any thread, keeping this state alive until it has run. Returns false
     * (and posts nothing) once shutdown has begun. Handlers already on the strand may post with
     * postFromStrand instead, which skips the check: the drain still runs them.
     */
    template<class F>
    [[nodiscard]] bool post(F&& fn) {
        std::shared_ptr<BoundState> self = shared_from_this();
        LockGuard lock(m_reactor.lifecycle);
        if (m_reactor.shutDown) {
            return false;
        }
        asio::post(m_strand, [self = std::move(self), fn = std::forward<F>(fn)]() mutable {
            self->m_reactor.countHandler();
            fn();
        });
        return true;
    }

    // Posts `fn` to the strand from a handler already running on it (precondition, checked).
    template<class F>
    void postFromStrand(F&& fn) {
        FROST_PRE(onStrand());
        asio::post(m_strand, [self = shared_from_this(), fn = std::forward<F>(fn)]() mutable {
            self->m_reactor.countHandler();
            fn();
        });
    }

    // Wraps a completion handler so it runs on the strand, keeps this state alive and is counted.
    template<class F>
    [[nodiscard]] auto bindHandler(F&& fn) {
        return asio::bind_executor(m_strand,
                                   [self = shared_from_this(), fn = std::forward<F>(fn)](auto&&... args) mutable {
                                       self->m_reactor.countHandler();
                                       fn(std::forward<decltype(args)>(args)...);
                                   });
    }

private:
    Reactor::Impl& m_reactor;
    ReactorStrand m_strand;
};

// Constructs a T (derived from BoundState; its first constructor argument is the Reactor::Impl&) and
// binds it. Errors: Cancelled once shutdown has begun (the state is then destroyed unbound).
template<class T, class... Args>
[[nodiscard]] Result<std::shared_ptr<T>> makeBound(Reactor::Impl& reactor, Args&&... args) {
    auto state = std::make_shared<T>(reactor, std::forward<Args>(args)...);
    if (auto bound = reactor.bind(state); !bound) {
        return std::unexpected(std::move(bound.error()));
    }
    return state;
}

/*
 * `co_await RunOnStrand(state, scheduler, fn)` runs `fn` (Result<void>() noexcept-ish, called on the
 * strand) and resumes the awaiting coroutine through scheduler.enqueue on the lane it was on
 * (UserVisible off-scheduler). If the post is refused (the reactor is shutting down) it completes at once
 * with Cancelled. `fn` itself must also report Cancelled when it finds the state shut down, since a post
 * made just before shutdown still runs during the drain.
 */
template<class F>
class RunOnStrand {
public:
    RunOnStrand(BoundState& state, sched::IScheduler& scheduler, F fn)
        : m_state(state), m_scheduler(scheduler), m_fn(std::move(fn)) {}
    RunOnStrand(const RunOnStrand&) = delete ("the strand handler refers to the awaiter");
    RunOnStrand& operator=(const RunOnStrand&) = delete ("the strand handler refers to the awaiter");

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        m_handle = handle;
        m_lane = m_scheduler.currentLane().value_or(sched::Lane::UserVisible);
        const bool posted = m_state.post([this] {
            m_result = m_fn();
            // After enqueue the coroutine may run (and destroy this awaiter) on another thread at once,
            // so copy what enqueue needs first.
            sched::IScheduler& scheduler = m_scheduler;
            const std::coroutine_handle<> handle = m_handle;
            const sched::Lane lane = m_lane;
            scheduler.enqueue(handle, lane);
        });
        if (!posted) {
            m_result = fail(ErrorCode::Cancelled, "the reactor is shut down");
            return false;
        }
        return true;
    }
    [[nodiscard]] Result<void> await_resume() { return std::move(m_result); }

private:
    BoundState& m_state;
    sched::IScheduler& m_scheduler;
    F m_fn;
    std::coroutine_handle<> m_handle;
    sched::Lane m_lane = sched::Lane::UserVisible;
    Result<void> m_result;
};

} // namespace detail
} // namespace frost::io
