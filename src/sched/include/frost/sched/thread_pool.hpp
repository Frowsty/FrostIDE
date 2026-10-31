// SPDX-License-Identifier: Apache-2.0
//
// ThreadPool: the real IScheduler (PLAN.md section 2, "Scheduler"; ADR 0002).
//
/*
 * Structure. N-1 worker threads by default (N = hardware threads). Each worker
 * owns one Chase-Lev deque per lane; work enqueued from a worker goes to its own
 * deque, work enqueued from any other thread (the UI thread, the timer thread,
 * later the io reactor) goes to a per-lane injection queue. A worker looking for
 * work asks its LaneSelector for a lane order (priority plus aging) and, per
 * lane, tries its own deque (LIFO), the lane's injection queue (FIFO), then
 * steals (FIFO) from the other workers, starting at a random victim. Every
 * kFifoPeriod-th dispatch takes the oldest element instead (injection queue
 * first, then the FIFO end of its own deque), so old work in a busy lane cannot
 * be overtaken forever by newer work.
 *
 * Low-lane cap. At most lowLaneCap() Background+Idle segments run at once
 * (default: N-4, at least 1, and at most workers-1), so at least one worker is
 * always free of low-lane work and Interactive work never waits behind
 * indexing. A "segment" is one resume of a coroutine until it next suspends.
 *
 * Parking. Idle workers spin briefly, then park on an event count (m_epoch,
 * std::atomic::wait). Producers bump it after publishing work when a worker
 * is parked and the work is runnable under the cap; a worker that takes work
 * while more eligible work remains wakes one more. The seq_cst handshake on
 * m_sleepers and the queue counters guarantees no lost wakeups (see park()).
 *
 * Timers run on one internal timer thread reading the injected IClock.
 * TODO(M1): move timers onto the io reactor's timer queue once src/io exists, and use
 * high-resolution waits on Windows (condition-variable timeouts round to the 15.6 ms system tick).
 * TODO(M1): helper-process priorities (BELOW_NORMAL + EcoQoS / SCHED_IDLE) belong to the process
 * launcher in src/io; worker threads keep normal OS priority.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/mutex.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace frost::sched {

struct ThreadPoolOptions {
    // Worker threads; 0 selects ThreadPool::defaultWorkerCount().
    unsigned workerCount = 0;
    // Concurrent Background+Idle segments; 0 selects ThreadPool::defaultLowLaneCap().
    // The effective cap is clamped to [1, workerCount - 1] (1 when there is one worker).
    unsigned lowLaneCap = 0;
    // Timer clock; nullptr selects systemClock(). Must outlive the pool.
    const IClock* clock = nullptr;
    // Seeds the per-worker victim-selection generators (only affects steal order).
    std::uint64_t seed = 0x5EED'F00D'CAFE'0001ULL;
};

// A point-in-time snapshot for tests and diagnostics; every field may be stale on return.
struct ThreadPoolStats {
    std::array<std::int64_t, kLaneCount> queued{}; // Enqueued, not yet dispatched, per lane.
    std::int64_t lowRunning = 0;                   // Background+Idle segments running now.
    std::int64_t outstanding = 0;                  // Queued + running segments + pending timers.
    std::int64_t parked = 0;                       // Workers parked or about to park.
};

class ThreadPool final : public IScheduler {
public:
    // Every kFifoPeriod-th dispatch of a worker takes the oldest available element (in dispatches).
    static constexpr std::uint32_t kFifoPeriod = 32;
    // Rounds of re-searching (each followed by a CPU pause burst) before a worker parks.
    static constexpr unsigned kSpinRounds = 16;
    // Steal attempts per victim and lane after losing a race (StealStatus::Abort).
    static constexpr unsigned kStealRetries = 2;

    // hardware_concurrency() - 1, at least 1.
    [[nodiscard]] static unsigned defaultWorkerCount() noexcept;
    // hardware_concurrency() - 4, at least 1 (before clamping to the worker count).
    [[nodiscard]] static unsigned defaultLowLaneCap() noexcept;

    // Starts the workers and the timer thread. Allocation or thread-creation failure terminates.
    explicit ThreadPool(ThreadPoolOptions options = {});

    // Calls shutdown(). Precondition: not called from one of this pool's workers.
    ~ThreadPool();

    // Drains and stops the pool (idempotent):
    //  1. requests stopToken();
    //  2. completes every pending sleep immediately with `false`; later sleeps do the same;
    //  3. keeps dispatching until nothing is queued, running or sleeping, so every
    //     queued task runs (observing the stop if it checks) and every JoinHandle completes;
    //  4. joins the workers and the timer thread.
    // Tasks may spawn and schedule more work while draining; work that never
    // finishes (an infinite loop ignoring stopToken()) makes shutdown hang.
    // Preconditions: not called from a worker; after shutdown has returned, no
    // thread may enqueue work. Thread-safe against concurrent enqueue from tasks.
    void shutdown();

    // Blocks until nothing is queued, running or sleeping (a moment of quiescence;
    // work may be enqueued again right after). Precondition: not on a worker.
    void waitIdle();

    // Wakes the timer thread to re-read the clock; call after advancing a
    // ManualClock passed in the options. Real clocks never need it.
    void notifyClockChanged();

    [[nodiscard]] unsigned workerCount() const noexcept { return static_cast<unsigned>(m_workers.size()); }
    [[nodiscard]] unsigned lowLaneCap() const noexcept { return m_lowCap; }
    [[nodiscard]] ThreadPoolStats stats() const noexcept;

    // IScheduler. enqueue() is thread-safe: from a worker of this pool it pushes to
    // that worker's deque, from any other thread to the lane's injection queue. O(1)
    // amortised, plus one futex/WaitOnAddress wake when a worker is parked.
    void enqueue(std::coroutine_handle<> handle, Lane lane) noexcept override;
    [[nodiscard]] const IClock& clock() const noexcept override { return *m_clock; }
    [[nodiscard]] std::optional<Lane> currentLane() const noexcept override;
    [[nodiscard]] std::stop_token stopToken() const noexcept override { return m_stopSource.get_token(); }
    void addTimer(TimerRequest& request, const std::stop_token& token) noexcept override;
    void cancelTimer(TimerRequest& request) noexcept override;

    struct Worker; // Defined in thread_pool.cpp.

private:
    struct alignas(64) PaddedCounter {
        std::atomic<std::int64_t> value{0};
    };
    struct alignas(64) InjectionQueue {
        Mutex mutex;
        std::deque<void*> items FROST_GUARDED_BY(mutex);
        // Mirrors items.size() for lock-free emptiness checks; written under the mutex.
        std::atomic<std::int64_t> size{0};
    };
    struct Found {
        void* address;
        Lane lane;
    };
    using TimerKey = std::pair<IClock::Duration::rep, std::uint64_t>; // (deadline ticks, registration id)
    using DueTimer = std::pair<std::coroutine_handle<>, Lane>;

    void workerMain(Worker& worker);
    void timerMain();
    [[nodiscard]] std::optional<Found> findWork(Worker& worker);
    [[nodiscard]] void* takeFromLane(Worker& worker, Lane lane, bool oldestFirst);
    [[nodiscard]] void* popInjection(Lane lane);
    [[nodiscard]] void* stealFromOthers(Worker& worker, Lane lane);
    void runSegment(Found found);
    [[nodiscard]] bool park();
    [[nodiscard]] bool hasEligibleWork() const noexcept;
    [[nodiscard]] bool tryReserveLowSlot() noexcept;
    void releaseLowSlot() noexcept;
    void wakeOne() noexcept;
    void finishUnit() noexcept;
    void signalExit() noexcept;
    // Moves due timers (all of them when `all`, the shutdown flush) from m_timers to `due`.
    void collectDueTimers(std::vector<DueTimer>& due, bool all) FROST_REQUIRES(m_timerMutex);

    const IClock* m_clock;
    unsigned m_lowCap = 1;
    std::vector<std::unique_ptr<Worker>> m_workers;

    std::array<InjectionQueue, kLaneCount> m_injection;
    // Per lane: elements enqueued and not yet dispatched (all queues). Incremented
    // before an element becomes visible, decremented after it is taken.
    std::array<PaddedCounter, kLaneCount> m_queued;
    // Units of outstanding work: +1 per enqueue and per registered timer, -1 when a
    // segment ends or a timer's unit is handed to the queue. Zero means quiescent.
    alignas(64) std::atomic<std::int64_t> m_outstanding{0};
    alignas(64) std::atomic<std::int64_t> m_lowRunning{0};
    // Event count for parking: bumped by every wake; parked workers wait on its value.
    alignas(64) std::atomic<std::uint32_t> m_epoch{0};
    std::atomic<std::int64_t> m_sleepers{0};
    std::atomic<bool> m_draining{false}; // shutdown() has flushed timers and waits for quiescence.
    std::atomic<bool> m_exit{false};     // Quiescent while draining: workers leave.

    std::stop_source m_stopSource;
    bool m_shutDown = false; // Touched only by the owning thread (shutdown() and the destructor).

    // Idle waiters (waitIdle()).
    Mutex m_idleMutex;
    std::condition_variable m_idleCv;
    std::atomic<std::int64_t> m_idleWaiters{0};

    // Timers: ordered by deadline, then registration order.
    Mutex m_timerMutex;
    std::condition_variable m_timerCv;
    std::map<TimerKey, TimerRequest*> m_timers FROST_GUARDED_BY(m_timerMutex);
    std::uint64_t m_nextTimerId FROST_GUARDED_BY(m_timerMutex) = 0;
    bool m_timerStopping FROST_GUARDED_BY(m_timerMutex) = false;
    std::thread m_timerThread;
};

} // namespace frost::sched
