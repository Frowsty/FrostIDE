// SPDX-License-Identifier: Apache-2.0
//
// IScheduler: the scheduling seam of frost::sched (PLAN.md sections 3.3 and
// 6.2). ThreadPool is the real implementation; SimExecutor is the seeded,
// single-threaded, virtual-time double. Code that needs to run work
// asynchronously takes an IScheduler& and uses only what is declared here:
//
//   co_await scheduler.schedule(Lane::Background);        // hop onto a lane
//   bool expired = co_await scheduler.sleepFor(50ms, token);  // timer via the scheduler's IClock
//   JoinHandle<int> h = scheduler.spawn(Lane::UserVisible, compute(token), token);
//   int value = co_await std::move(h);                     // or h.join() from a plain thread
//   int other = syncWait(compute(token));                  // blocking, for tests and non-coroutine callers
//
// Lanes of continuations. A coroutine stays on the lane it was last dispatched
// on: awaiting a Task runs it inline in the same lane, and only schedule(),
// timers and JoinHandle completions re-dispatch. A timer or a JoinHandle
// resumes its awaiter on the lane the awaiter was running on when it
// suspended, or on Lane::UserVisible if it was not running on this scheduler.
//
// Cancellation is cooperative: tasks take a std::stop_token and check it.
// The scheduler helps in three places: spawn(lane, task, token) skips a task
// whose token is stopped before it starts; sleeps end early when their token is
// stopped; and stopToken() is requested when the scheduler shuts down.
// LatestWins (latest_wins.hpp) builds "latest request wins" on top.
//
// Shutdown (both implementations, see ThreadPool::shutdown): the scheduler's
// stop token is requested, pending and future sleeps complete immediately with
// `false`, and all remaining work runs to completion before shutdown returns
// ("drain"). Nothing is destroyed half-run, so every JoinHandle completes.
//
// Not in this slice:
// TODO(M1): MainThreadDispatcher (Qt event per frame) lives in src/ui; it will implement IScheduler's
//           enqueue for a "main thread" target.
// TODO(M1): the asio reactor (src/io) completes I/O awaiters by calling enqueue() from its threads.
#pragma once

#include "frost/core/assert.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/task.hpp"

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace frost::sched {

template<class T>
class JoinHandle;
class ScheduleAwaiter;
class SleepAwaiter;

namespace detail {
// True when the calling thread is a ThreadPool worker. Blocking waits are refused
// there, because a worker blocked on work queued behind it can deadlock the pool.
[[nodiscard]] bool onPoolWorkerThread() noexcept;
} // namespace detail

// One pending sleep, owned by the awaiting coroutine's SleepAwaiter. The
// scheduler fills `expired` and resumes `handle` on `lane` exactly once.
struct TimerRequest {
    std::coroutine_handle<> handle;
    IClock::SteadyTime deadline{};
    Lane lane = Lane::UserVisible;
    bool expired = false; // Written by the scheduler before resuming: true if the deadline passed.
    std::uint64_t id = 0; // Scheduler bookkeeping; 0 = not registered.
};

class IScheduler {
public:
    // --- Awaitables and spawning (implemented on top of the primitives below) ---

    // `co_await schedule(lane)` suspends and resumes the coroutine on `lane`.
    // Always re-dispatches, so it doubles as a yield point. O(1).
    [[nodiscard]] ScheduleAwaiter schedule(Lane lane) noexcept;

    // Suspends until clock().steadyNow() >= deadline. Yields true if the deadline
    // passed, false if `token` was stopped first or the scheduler shut down.
    // Completes without suspending if the deadline has passed or `token` is
    // already stopped. O(log timers).
    [[nodiscard]] SleepAwaiter sleepUntil(IClock::SteadyTime deadline, std::stop_token token = {}) noexcept;

    // sleepUntil(clock().steadyNow() + duration, token); durations <= 0 complete at once.
    [[nodiscard]] SleepAwaiter sleepFor(IClock::Duration duration, std::stop_token token = {}) noexcept;

    // Starts `task` on `lane` and returns its join handle. Dropping the handle (or
    // detach()) lets the task run on with its result discarded. Allocates the
    // spawn frame; O(1) otherwise. Precondition: task.valid().
    template<class T>
    JoinHandle<T> spawn(Lane lane, Task<T> task);

    // As above, but if `token` is stopped by the time the task is dispatched, its
    // body never runs: the frame is destroyed unstarted, cancelledBeforeStart()
    // becomes true and a Result task yields ErrorCode::Cancelled. The task body
    // should take the same token to observe stops while running.
    template<class T>
        requires(kIsResult<T> || std::is_void_v<T>)
    JoinHandle<T> spawn(Lane lane, Task<T> task, std::stop_token token);

    // --- Primitives, implemented by each scheduler ---

    // Makes `handle` runnable on `lane`; it is resumed exactly once, possibly on
    // another thread before enqueue() returns. Thread-safety is per implementation
    // (ThreadPool: any thread; SimExecutor: its owner thread). Never fails;
    // allocation failure terminates.
    virtual void enqueue(std::coroutine_handle<> handle, Lane lane) noexcept = 0;

    // The time source for timers. Valid for the scheduler's lifetime.
    [[nodiscard]] virtual const IClock& clock() const noexcept = 0;

    // The lane of the work running on the calling thread, if that work belongs to this scheduler.
    [[nodiscard]] virtual std::optional<Lane> currentLane() const noexcept = 0;

    // Requested when shutdown begins; long-running loops can watch it.
    [[nodiscard]] virtual std::stop_token stopToken() const noexcept = 0;

    // Timer primitives used by SleepAwaiter. addTimer() registers `request`
    // (handle, deadline and lane filled in) and resumes it at the deadline, or
    // at once with expired = false if `token` is already stopped or the
    // scheduler is shutting down. cancelTimer() resumes a registered request
    // early with expired = false; it is a no-op if the request already fired or
    // is not registered yet. Both are safe to race with each other and with firing.
    virtual void addTimer(TimerRequest& request, const std::stop_token& token) noexcept = 0;
    virtual void cancelTimer(TimerRequest& request) noexcept = 0;

    // Schedulers driven by their caller (SimExecutor) return true and run one
    // step per driveOne() call, returning false when nothing is left to run.
    // JoinHandle::join() uses this to make progress instead of blocking.
    [[nodiscard]] virtual bool callerDriven() const noexcept { return false; }
    virtual bool driveOne() { return false; }

    IScheduler(const IScheduler&) = delete ("schedulers are shared by reference");
    IScheduler& operator=(const IScheduler&) = delete ("schedulers are shared by reference");

protected:
    IScheduler() noexcept = default;
    // Never deleted through the interface (see IClock for the rationale).
    ~IScheduler() = default;
};

class ScheduleAwaiter {
public:
    ScheduleAwaiter(IScheduler& scheduler, Lane lane) noexcept : m_scheduler(&scheduler), m_lane(lane) {}
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    // After enqueue() the coroutine may already be running elsewhere; nothing touches `this` afterwards.
    void await_suspend(std::coroutine_handle<> handle) const noexcept { m_scheduler->enqueue(handle, m_lane); }
    void await_resume() const noexcept {}

private:
    IScheduler* m_scheduler;
    Lane m_lane;
};

class SleepAwaiter {
public:
    SleepAwaiter(IScheduler& scheduler, IClock::SteadyTime deadline, std::stop_token token) noexcept
        : m_scheduler(&scheduler), m_token(std::move(token)) {
        m_request.deadline = deadline;
    }
    SleepAwaiter(const SleepAwaiter&) = delete ("an armed sleep is referenced by its scheduler");
    SleepAwaiter& operator=(const SleepAwaiter&) = delete ("an armed sleep is referenced by its scheduler");

    [[nodiscard]] bool await_ready() noexcept {
        if (m_token.stop_requested()) {
            m_request.expired = false;
            return true;
        }
        if (m_scheduler->clock().steadyNow() >= m_request.deadline) {
            m_request.expired = true;
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
        m_request.handle = handle;
        m_request.lane = m_scheduler->currentLane().value_or(Lane::UserVisible);
        // The callback is registered before the timer, so a stop that races with
        // registration is seen by addTimer() (which checks the token under its lock).
        if (m_token.stop_possible()) {
            m_callback.emplace(m_token, CancelTimer{m_scheduler, &m_request});
        }
        // From here the coroutine may resume on another thread at any moment.
        m_scheduler->addTimer(m_request, m_token);
    }

    // Destroying the callback waits for a concurrently running cancelTimer(), so
    // the request outlives every scheduler access to it.
    bool await_resume() noexcept {
        m_callback.reset();
        return m_request.expired;
    }

private:
    struct CancelTimer {
        IScheduler* scheduler;
        TimerRequest* request;
        void operator()() const noexcept { scheduler->cancelTimer(*request); }
    };

    IScheduler* m_scheduler;
    TimerRequest m_request;
    std::stop_token m_token;
    std::optional<std::stop_callback<CancelTimer>> m_callback;
};

inline ScheduleAwaiter IScheduler::schedule(Lane lane) noexcept { return ScheduleAwaiter(*this, lane); }

inline SleepAwaiter IScheduler::sleepUntil(IClock::SteadyTime deadline, std::stop_token token) noexcept {
    return SleepAwaiter(*this, deadline, std::move(token));
}

inline SleepAwaiter IScheduler::sleepFor(IClock::Duration duration, std::stop_token token) noexcept {
    const IClock::SteadyTime now = clock().steadyNow();
    // Saturate instead of overflowing for huge durations ("sleep until shutdown").
    const IClock::SteadyTime deadline =
        duration >= IClock::SteadyTime::max() - now ? IClock::SteadyTime::max() : now + duration;
    return SleepAwaiter(*this, deadline, std::move(token));
}

namespace detail {

// Join-state values. Transitions: Pending -> Awaiting | Blocking (the single
// consumer arrives first), then -> Done (the producer finishes); or Pending -> Done.
inline constexpr std::uint32_t kJoinPending = 0;
inline constexpr std::uint32_t kJoinAwaiting = 1; // A coroutine waits; m_continuation is set.
inline constexpr std::uint32_t kJoinBlocking = 2; // A thread waits on m_state.
inline constexpr std::uint32_t kJoinDone = 3;     // The result is stored.

// Markers returned by the spawn frame's body.
struct CancelledBeforeStart {};
struct VoidCompleted {};

// Result storage for the spawn frame.
template<class T>
struct JoinStorage {
    void return_value(T&& value) { m_value.emplace(std::move(value)); }
    void return_value(CancelledBeforeStart) {
        m_cancelled = true;
        if constexpr (kIsResult<T>) {
            m_value.emplace(std::unexpected(Error(ErrorCode::Cancelled, "cancelled before start")));
        }
    }
    // Moves the result out; a second take is a contract violation (not a moved-from value).
    [[nodiscard]] T takeValue() {
        FROST_ASSERT(m_value.has_value());
        T value = std::move(*m_value);
        m_value.reset();
        return value;
    }
    std::optional<T> m_value;
    bool m_cancelled = false;
};

template<>
struct JoinStorage<void> {
    void return_value(VoidCompleted) noexcept {}
    void return_value(CancelledBeforeStart) noexcept { m_cancelled = true; }
    void takeValue() const noexcept {}
    bool m_cancelled = false;
};

template<class T>
class JoinPromise;

// The spawn frame: a coroutine that awaits the user's task and publishes the
// result to one JoinHandle. It is reference-counted (the frame itself and the
// handle), so whichever finishes last frees it.
template<class T>
struct JoinFrame {
    using promise_type = JoinPromise<T>;
    std::coroutine_handle<JoinPromise<T>> handle;
};

template<class T>
class JoinPromise final : public JoinStorage<T> {
public:
    JoinFrame<T> get_return_object() noexcept {
        return JoinFrame<T>{std::coroutine_handle<JoinPromise>::from_promise(*this)};
    }
    // Suspended until the scheduler (or syncWait) resumes it.
    [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

    struct FinalAwaiter {
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        // Publishes the result, wakes the consumer, then drops the frame's own
        // reference. Returning false when it was the last one lets the coroutine
        // run off its end, which destroys the frame.
        [[nodiscard]] bool await_suspend(std::coroutine_handle<JoinPromise> self) const noexcept {
            JoinPromise& promise = self.promise();
            // Acq_rel: release publishes the stored result to the consumer; acquire
            // makes the consumer's m_continuation (written before its CAS) visible.
            const std::uint32_t previous = promise.m_state.exchange(kJoinDone, std::memory_order_acq_rel);
            if (previous == kJoinAwaiting) {
                promise.m_scheduler->enqueue(promise.m_continuation, promise.m_continuationLane);
            } else if (previous == kJoinBlocking) {
                promise.m_state.notify_all();
            }
            return !promise.releaseIsLast();
        }
        void await_resume() const noexcept {}
    };
    [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }

    // The body only awaits a Task, which never lets an exception out.
    void unhandled_exception() const noexcept { abortOnEscapedException(); }

    // Drops one reference; returns true if it was the last.
    [[nodiscard]] bool releaseIsLast() noexcept {
        // Acq_rel: the last releaser must see every write the other owner made to the frame.
        return m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }

    // Two owners: the running frame and the JoinHandle.
    std::atomic<std::uint32_t> m_refs{2};
    std::atomic<std::uint32_t> m_state{kJoinPending};
    std::coroutine_handle<> m_continuation;
    Lane m_continuationLane = Lane::UserVisible;
    // Resumes awaiting coroutines and drives caller-driven schedulers; null for syncWait frames.
    IScheduler* m_scheduler = nullptr;
};

// The spawn frame's body. A stopped token skips the task: it is destroyed unstarted.
template<class T>
JoinFrame<T> runJoined(Task<T> task, std::stop_token token) {
    if (token.stop_requested()) {
        {
            [[maybe_unused]] Task<T> unstarted = std::move(task);
        } // Frees the task's frame now.
        co_return CancelledBeforeStart{};
    }
    // Moved into a local so the task's frame is freed when the body ends, not with the spawn frame.
    Task<T> local = std::move(task);
    if constexpr (std::is_void_v<T>) {
        co_await local;
        co_return VoidCompleted{};
    } else {
        co_return co_await local;
    }
}

} // namespace detail

// The single consumer of a spawned task's result. Move-only. Destroying it (or
// detach()) gives up the result; the task keeps running. Thread safety: one
// thread uses a JoinHandle at a time; it may race freely with the task finishing.
// Lifetime: a ready handle may outlive its scheduler (every handle is ready once
// its scheduler has shut down); a pending one must not (see join()).
template<class T>
class [[nodiscard]] JoinHandle {
public:
    using Frame = std::coroutine_handle<detail::JoinPromise<T>>;

    JoinHandle() noexcept = default;
    // Adopts the consumer reference of `frame`; used by spawn() and syncWait().
    explicit JoinHandle(Frame frame) noexcept : m_frame(frame) {}
    JoinHandle(JoinHandle&& other) noexcept : m_frame(std::exchange(other.m_frame, {})) {}
    JoinHandle& operator=(JoinHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_frame = std::exchange(other.m_frame, {});
        }
        return *this;
    }
    JoinHandle(const JoinHandle&) = delete ("a task's result has one consumer");
    JoinHandle& operator=(const JoinHandle&) = delete ("a task's result has one consumer");
    ~JoinHandle() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(m_frame); }

    // True once the result is available. Precondition: valid().
    [[nodiscard]] bool ready() const noexcept {
        FROST_PRE(valid());
        // Acquire: pairs with the producer's exchange, so the result is visible.
        return m_frame.promise().m_state.load(std::memory_order_acquire) == detail::kJoinDone;
    }

    // True if spawn(lane, task, token) skipped the body. Precondition: ready().
    [[nodiscard]] bool cancelledBeforeStart() const noexcept {
        FROST_PRE(ready());
        return m_frame.promise().m_cancelled;
    }

    // Gives up the result without waiting.
    void detach() noexcept { reset(); }

    // Waits for the result and moves it out. On a caller-driven scheduler
    // (SimExecutor) it runs the scheduler until the task is done; otherwise it
    // blocks the thread. The handle stays valid (ready() is true afterwards), but
    // the result can be taken only once, by join() or co_await (a second take is
    // a contract violation). Preconditions: valid(); not on a ThreadPool worker
    // when blocking; on a caller-driven scheduler, the task must be able to
    // finish (an idle scheduler with the task pending is a contract violation,
    // i.e. a deadlock).
    // Lifetime: a ready handle may outlive its scheduler (shutdown drains, so
    // every handle is ready once the scheduler is gone) and join() then only
    // takes the result. A pending handle must not outlive its scheduler.
    T join() {
        FROST_PRE(valid());
        detail::JoinPromise<T>& promise = m_frame.promise();
        // Acquire: pairs with the producer's exchange to Done. A finished task needs
        // no scheduler, which may already be destroyed, so it is never touched here.
        if (promise.m_state.load(std::memory_order_acquire) == detail::kJoinDone) {
            return promise.takeValue();
        }
        if (promise.m_scheduler != nullptr && promise.m_scheduler->callerDriven()) {
            while (promise.m_state.load(std::memory_order_acquire) != detail::kJoinDone) {
                const bool progressed = promise.m_scheduler->driveOne();
                FROST_ASSERT(progressed);
            }
        } else {
            FROST_PRE(!detail::onPoolWorkerThread());
            std::uint32_t expected = detail::kJoinPending;
            // Acq_rel on success (we become the blocked consumer); acquire on
            // failure (the state is Done and we must see the result).
            if (promise.m_state.compare_exchange_strong(expected, detail::kJoinBlocking, std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
                // Acquire: pairs with the producer's exchange to Done.
                while (promise.m_state.load(std::memory_order_acquire) == detail::kJoinBlocking) {
                    promise.m_state.wait(detail::kJoinBlocking, std::memory_order_acquire);
                }
            }
        }
        return promise.takeValue();
    }

    // `co_await std::move(handle)` suspends until the result is available and
    // resumes on the awaiter's lane (see the top of this file).
    struct Awaiter {
        Frame frame;
        [[nodiscard]] bool await_ready() const noexcept {
            // Acquire: as in ready().
            return frame.promise().m_state.load(std::memory_order_acquire) == detail::kJoinDone;
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> awaiting) const noexcept {
            detail::JoinPromise<T>& promise = frame.promise();
            FROST_PRE(promise.m_scheduler != nullptr);
            promise.m_continuation = awaiting;
            promise.m_continuationLane = promise.m_scheduler->currentLane().value_or(Lane::UserVisible);
            std::uint32_t expected = detail::kJoinPending;
            // Acq_rel on success: release publishes m_continuation to the producer.
            // Acquire on failure: the task finished; resume at once and read the result.
            return promise.m_state.compare_exchange_strong(expected, detail::kJoinAwaiting, std::memory_order_acq_rel,
                                                           std::memory_order_acquire);
        }
        T await_resume() const { return frame.promise().takeValue(); }
    };
    // Precondition: valid() and neither awaited nor joined before.
    [[nodiscard]] Awaiter operator co_await() & noexcept {
        FROST_PRE(valid());
        return Awaiter{m_frame};
    }
    [[nodiscard]] Awaiter operator co_await() && noexcept {
        FROST_PRE(valid());
        return Awaiter{m_frame};
    }

private:
    void reset() noexcept {
        if (m_frame) {
            Frame frame = std::exchange(m_frame, {});
            if (frame.promise().releaseIsLast()) {
                frame.destroy();
            }
        }
    }

    Frame m_frame;
};

template<class T>
JoinHandle<T> IScheduler::spawn(Lane lane, Task<T> task) {
    FROST_PRE(task.valid());
    detail::JoinFrame<T> frame = detail::runJoined(std::move(task), std::stop_token{});
    frame.handle.promise().m_scheduler = this;
    JoinHandle<T> handle(frame.handle);
    enqueue(frame.handle, lane);
    return handle;
}

template<class T>
    requires(kIsResult<T> || std::is_void_v<T>)
JoinHandle<T> IScheduler::spawn(Lane lane, Task<T> task, std::stop_token token) {
    FROST_PRE(task.valid());
    detail::JoinFrame<T> frame = detail::runJoined(std::move(task), std::move(token));
    frame.handle.promise().m_scheduler = this;
    JoinHandle<T> handle(frame.handle);
    enqueue(frame.handle, lane);
    return handle;
}

// Runs `task` to completion and returns its result, blocking the calling
// thread. The task starts inline on the calling thread and continues wherever
// it schedules itself (e.g. a ThreadPool). For tests and non-coroutine callers;
// drive a SimExecutor with spawn(...).join() instead. Precondition: task.valid()
// and not called on a ThreadPool worker.
template<class T>
T syncWait(Task<T> task) {
    FROST_PRE(task.valid());
    FROST_PRE(!detail::onPoolWorkerThread());
    detail::JoinFrame<T> frame = detail::runJoined(std::move(task), std::stop_token{});
    JoinHandle<T> handle(frame.handle);
    frame.handle.resume();
    return handle.join();
}

} // namespace frost::sched
