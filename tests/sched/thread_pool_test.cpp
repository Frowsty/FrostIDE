// SPDX-License-Identifier: Apache-2.0
//
// Tests for ThreadPool: sizing, spawning and joining, lane hops, lane
// priority and aging on a single worker, the Background+Idle cap (with
// Interactive work still running), work stealing, cancellation before start and
// during a run, timers on the system clock and on a ManualClock, shutdown with
// pending work, and lost-wakeup hunting.

#include "frost/core/clock.hpp"
#include "frost/sched/lane_policy.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"
#include "frost/sched/thread_pool.hpp"

#include "sched/sched_test_support.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::Result;
using frost::sched::IScheduler;
using frost::sched::JoinHandle;
using frost::sched::kAllLanes;
using frost::sched::Lane;
using frost::sched::laneIndex;
using frost::sched::LaneSelector;
using frost::sched::syncWait;
using frost::sched::Task;
using frost::sched::ThreadPool;
using frost::sched::ThreadPoolOptions;
using frost::sched::test::waitFor;

// Blocks one worker until `release` is set; `started` reports that it runs.
Task<void> gate(std::atomic<bool>& started, std::atomic<bool>& release) {
    started.store(true);
    while (!release.load()) {
        std::this_thread::yield();
    }
    co_return;
}

// Records the order in which tasks run.
class Recorder {
public:
    Task<void> record(Lane lane, int id) {
        std::lock_guard lock(m_mutex);
        m_order.emplace_back(lane, id);
        co_return;
    }
    std::vector<std::pair<Lane, int>> order() {
        std::lock_guard lock(m_mutex);
        return m_order;
    }

private:
    std::mutex m_mutex;
    std::vector<std::pair<Lane, int>> m_order;
};

TEST(ThreadPool, SizingFollowsHardwareAndClampsTheCap) {
    const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
    EXPECT_EQ(ThreadPool::defaultWorkerCount(), std::max(1U, hardware - 1));
    EXPECT_EQ(ThreadPool::defaultLowLaneCap(), hardware > 4 ? hardware - 4 : 1U);
    ThreadPool small(ThreadPoolOptions{.workerCount = 4, .lowLaneCap = 100});
    EXPECT_EQ(small.workerCount(), 4U);
    EXPECT_EQ(small.lowLaneCap(), 3U); // Always one worker free of low-lane work.
    ThreadPool single(ThreadPoolOptions{.workerCount = 1});
    EXPECT_EQ(single.lowLaneCap(), 1U);
}

Task<int> square(IScheduler& scheduler, int value) {
    co_await scheduler.schedule(Lane::Background);
    co_return value* value;
}

Task<int> sumOfSquares(IScheduler& scheduler, int count) {
    std::vector<JoinHandle<int>> handles;
    for (int i = 0; i < count; ++i) {
        handles.push_back(scheduler.spawn(Lane::UserVisible, square(scheduler, i)));
    }
    int sum = 0;
    for (JoinHandle<int>& handle : handles) {
        sum += co_await handle;
    }
    co_return sum;
}

TEST(ThreadPool, SpawnedTasksJoinFromThreadsAndCoroutines) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 4});
    JoinHandle<int> handle = pool.spawn(Lane::Interactive, square(pool, 12));
    EXPECT_EQ(handle.join(), 144);
    EXPECT_TRUE(handle.ready());
    EXPECT_FALSE(handle.cancelledBeforeStart());
    // Started inline on this thread by syncWait, then continued on the pool.
    EXPECT_EQ(syncWait(sumOfSquares(pool, 100)), 328'350);
}

Task<std::vector<std::optional<Lane>>> hopThroughLanes(IScheduler& scheduler) {
    std::vector<std::optional<Lane>> seen;
    seen.push_back(scheduler.currentLane());
    for (const Lane lane : kAllLanes) {
        co_await scheduler.schedule(lane);
        seen.push_back(scheduler.currentLane());
    }
    co_return seen;
}

TEST(ThreadPool, ScheduleMovesTheCoroutineToTheLane) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 2});
    const std::vector<std::optional<Lane>> seen = syncWait(hopThroughLanes(pool));
    ASSERT_EQ(seen.size(), 5U);
    EXPECT_FALSE(seen[0].has_value()); // syncWait starts it on this (non-pool) thread.
    for (std::size_t i = 0; i < kAllLanes.size(); ++i) {
        EXPECT_EQ(seen[i + 1], kAllLanes[i]);
    }
}

TEST(ThreadPool, SingleWorkerRunsLanesInPriorityOrder) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 1});
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    pool.spawn(Lane::Interactive, gate(started, release)).detach();
    ASSERT_TRUE(waitFor([&] { return started.load(); }));
    Recorder recorder;
    for (const Lane lane : {Lane::Idle, Lane::Background, Lane::UserVisible, Lane::Interactive}) {
        for (int id = 0; id < 3; ++id) {
            pool.spawn(lane, recorder.record(lane, id)).detach();
        }
    }
    release.store(true);
    pool.waitIdle();
    std::vector<std::pair<Lane, int>> expected;
    for (const Lane lane : kAllLanes) {
        for (int id = 0; id < 3; ++id) {
            expected.emplace_back(lane, id);
        }
    }
    EXPECT_EQ(recorder.order(), expected);
}

TEST(ThreadPool, AgingServesBackgroundUnderInteractiveLoad) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 1});
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    pool.spawn(Lane::Interactive, gate(started, release)).detach();
    ASSERT_TRUE(waitFor([&] { return started.load(); }));
    Recorder recorder;
    for (int id = 0; id < 40; ++id) {
        pool.spawn(Lane::Interactive, recorder.record(Lane::Interactive, id)).detach();
    }
    pool.spawn(Lane::Background, recorder.record(Lane::Background, 0)).detach();
    pool.spawn(Lane::Background, recorder.record(Lane::Background, 1)).detach();
    release.store(true);
    pool.waitIdle();
    const std::vector<std::pair<Lane, int>> order = recorder.order();
    ASSERT_EQ(order.size(), 42U);
    // Background is served after exactly kAgingPasses Interactive dispatches, twice.
    const std::size_t passes = LaneSelector::kAgingPasses[laneIndex(Lane::Background)];
    EXPECT_EQ(order[passes], std::make_pair(Lane::Background, 0));
    EXPECT_EQ(order[2 * passes + 1], std::make_pair(Lane::Background, 1));
}

Task<void> lowLaneWork(std::atomic<int>& running, std::atomic<int>& peak, std::atomic<bool>& release) {
    const int now = running.fetch_add(1) + 1;
    int previous = peak.load();
    while (previous < now && !peak.compare_exchange_weak(previous, now)) {
    }
    while (!release.load()) {
        std::this_thread::yield();
    }
    running.fetch_sub(1);
    co_return;
}

Task<int> quick(int value) { co_return value; }

TEST(ThreadPool, LowLaneCapHoldsAndInteractiveStillRuns) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 6, .lowLaneCap = 3});
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    std::atomic<bool> release{false};
    std::vector<JoinHandle<void>> low;
    for (int i = 0; i < 20; ++i) {
        low.push_back(pool.spawn(i % 2 == 0 ? Lane::Background : Lane::Idle, lowLaneWork(running, peak, release)));
    }
    ASSERT_TRUE(waitFor([&] { return running.load() == 3; }));
    // Low lanes are saturated; Interactive and UserVisible work must still run now.
    JoinHandle<int> interactive = pool.spawn(Lane::Interactive, quick(7));
    JoinHandle<int> visible = pool.spawn(Lane::UserVisible, quick(8));
    ASSERT_TRUE(waitFor([&] { return interactive.ready() && visible.ready(); }, 10s));
    EXPECT_EQ(interactive.join() + visible.join(), 15);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(running.load(), 3);
    EXPECT_EQ(pool.stats().lowRunning, 3);
    release.store(true);
    for (JoinHandle<void>& handle : low) {
        handle.join();
    }
    EXPECT_EQ(peak.load(), 3);
}

Task<void> recordThread(std::mutex& mutex, std::set<std::thread::id>& threads) {
    // Enough work per task for idle workers to steal the rest.
    const auto until = std::chrono::steady_clock::now() + 50us;
    while (std::chrono::steady_clock::now() < until) {
    }
    std::lock_guard lock(mutex);
    threads.insert(std::this_thread::get_id());
    co_return;
}

Task<void> fanOutFromOneWorker(IScheduler& scheduler, std::mutex& mutex, std::set<std::thread::id>& threads) {
    // Children spawned from a worker go to that worker's own deque; the others must steal them.
    std::vector<JoinHandle<void>> children;
    for (int i = 0; i < 400; ++i) {
        children.push_back(scheduler.spawn(Lane::UserVisible, recordThread(mutex, threads)));
    }
    for (JoinHandle<void>& child : children) {
        co_await child;
    }
}

TEST(ThreadPool, IdleWorkersStealFromABusyOne) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 4});
    std::mutex mutex;
    std::set<std::thread::id> threads;
    pool.spawn(Lane::UserVisible, fanOutFromOneWorker(pool, mutex, threads)).join();
    std::lock_guard lock(mutex);
    EXPECT_GE(threads.size(), 2U);
}

Task<Result<int>> cancellable(std::stop_token token, std::atomic<bool>& ran) {
    ran.store(true);
    if (token.stop_requested()) {
        co_return frost::fail(ErrorCode::Cancelled, "observed in body");
    }
    co_return 1;
}

Task<void> cancellableVoid(std::atomic<bool>& ran) {
    ran.store(true);
    co_return;
}

TEST(ThreadPool, CancellationBeforeStartSkipsTheBody) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 2});
    std::stop_source source;
    source.request_stop();
    std::atomic<bool> ran{false};
    JoinHandle<Result<int>> handle =
        pool.spawn(Lane::Background, cancellable(source.get_token(), ran), source.get_token());
    const Result<int> result = handle.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE(handle.cancelledBeforeStart());
    JoinHandle<void> voidHandle = pool.spawn(Lane::Idle, cancellableVoid(ran), source.get_token());
    voidHandle.join();
    EXPECT_TRUE(voidHandle.cancelledBeforeStart());
    EXPECT_FALSE(ran.load());
}

TEST(ThreadPool, CancellationWhileQueuedSkipsTheBody) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 1});
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    pool.spawn(Lane::Interactive, gate(started, release)).detach();
    ASSERT_TRUE(waitFor([&] { return started.load(); }));
    std::stop_source source;
    std::atomic<bool> ran{false};
    JoinHandle<Result<int>> handle =
        pool.spawn(Lane::UserVisible, cancellable(source.get_token(), ran), source.get_token());
    source.request_stop(); // Still queued behind the gate.
    release.store(true);
    EXPECT_EQ(handle.join().error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE(handle.cancelledBeforeStart());
    EXPECT_FALSE(ran.load());
}

Task<Result<int>> sleepLong(IScheduler& scheduler, std::stop_token token, std::atomic<bool>& sleeping) {
    sleeping.store(true);
    const bool expired = co_await scheduler.sleepFor(1h, token);
    if (!expired) {
        co_return frost::fail(ErrorCode::Cancelled, "stopped while sleeping");
    }
    co_return 1;
}

Task<Result<int>> spinUntilStopped(std::stop_token token, std::atomic<bool>& running) {
    running.store(true);
    while (!token.stop_requested()) {
        std::this_thread::yield();
    }
    co_return frost::fail(ErrorCode::Cancelled, "stopped while running");
}

TEST(ThreadPool, CancellationDuringRunEndsSleepsAndLoops) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 2});
    std::stop_source source;
    std::atomic<bool> sleeping{false};
    std::atomic<bool> running{false};
    const auto start = std::chrono::steady_clock::now();
    JoinHandle<Result<int>> sleeper =
        pool.spawn(Lane::Background, sleepLong(pool, source.get_token(), sleeping), source.get_token());
    JoinHandle<Result<int>> spinner =
        pool.spawn(Lane::UserVisible, spinUntilStopped(source.get_token(), running), source.get_token());
    ASSERT_TRUE(waitFor([&] { return sleeping.load() && running.load(); }));
    std::this_thread::sleep_for(20ms); // Let the sleep register its timer.
    source.request_stop();
    EXPECT_EQ(sleeper.join().error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(spinner.join().error().code(), ErrorCode::Cancelled);
    EXPECT_FALSE(sleeper.cancelledBeforeStart());
    EXPECT_LT(std::chrono::steady_clock::now() - start, 30s);
}

Task<frost::IClock::Duration> timedSleep(IScheduler& scheduler, frost::IClock::Duration duration, bool& expired) {
    const auto before = scheduler.clock().steadyNow();
    expired = co_await scheduler.sleepFor(duration);
    co_return scheduler.clock().steadyNow() - before;
}

TEST(ThreadPool, SleepForWaitsAtLeastTheDuration) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 2});
    bool expired = false;
    const frost::IClock::Duration slept = syncWait(timedSleep(pool, 20ms, expired));
    EXPECT_TRUE(expired);
    EXPECT_GE(slept, 20ms);
    // Zero and negative durations complete without suspending.
    EXPECT_LT(syncWait(timedSleep(pool, -5ms, expired)), 1s);
    EXPECT_TRUE(expired);
}

Task<frost::IClock::SteadyTime> sleepUntilThenReadClock(IScheduler& scheduler, frost::IClock::SteadyTime deadline,
                                                        bool& expired) {
    expired = co_await scheduler.sleepUntil(deadline);
    co_return scheduler.clock().steadyNow();
}

TEST(ThreadPool, TimersFollowAnInjectedClock) {
    frost::ManualClock clock;
    ThreadPool pool(ThreadPoolOptions{.workerCount = 2, .clock = &clock});
    bool expired = false;
    // An absolute deadline, so it does not matter whether the sleep registers before or after the advance.
    const frost::IClock::SteadyTime deadline = clock.steadyNow() + 10s;
    JoinHandle<frost::IClock::SteadyTime> handle =
        pool.spawn(Lane::Background, sleepUntilThenReadClock(pool, deadline, expired));
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(handle.ready()); // The manual clock has not moved.
    clock.advance(10s);
    pool.notifyClockChanged();
    EXPECT_EQ(handle.join(), deadline);
    EXPECT_TRUE(expired);
}

Task<void> detachedWork(std::atomic<int>& counter) {
    counter.fetch_add(1);
    co_return;
}

TEST(ThreadPool, DetachedTasksStillRun) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 3});
    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i) {
        pool.spawn(kAllLanes[static_cast<std::size_t>(i) % 4], detachedWork(counter)).detach();
    }
    pool.waitIdle();
    EXPECT_EQ(counter.load(), 100);
    EXPECT_EQ(pool.stats().outstanding, 0);
}

struct ShutdownCounters {
    std::atomic<int> finished{0};
    std::atomic<int> cutShort{0};
    std::atomic<int> sawStop{0};
};

Task<void> sleepThenCount(IScheduler& scheduler, ShutdownCounters& counters, bool spawnChild) {
    const bool expired = co_await scheduler.sleepFor(1h);
    if (!expired) {
        counters.cutShort.fetch_add(1);
    }
    if (scheduler.stopToken().stop_requested()) {
        counters.sawStop.fetch_add(1);
    }
    if (spawnChild) {
        // Work spawned while draining runs too; its sleep completes at once.
        scheduler.spawn(Lane::Idle, sleepThenCount(scheduler, counters, false)).detach();
    }
    counters.finished.fetch_add(1);
}

TEST(ThreadPool, ShutdownDrainsPendingWorkAndCutsSleepsShort) {
    ShutdownCounters counters;
    {
        ThreadPool pool(ThreadPoolOptions{.workerCount = 3});
        // Some of these are sleeping when shutdown starts, some have not started yet.
        for (int i = 0; i < 50; ++i) {
            pool.spawn(i % 2 == 0 ? Lane::Background : Lane::UserVisible, sleepThenCount(pool, counters, true))
                .detach();
        }
        const auto start = std::chrono::steady_clock::now();
        pool.shutdown(); // The destructor would do the same.
        EXPECT_LT(std::chrono::steady_clock::now() - start, 30s);
        pool.shutdown(); // Idempotent.
    }
    EXPECT_EQ(counters.finished.load(), 100);
    EXPECT_EQ(counters.cutShort.load(), 100);
    EXPECT_EQ(counters.sawStop.load(), 100);
}

Task<void> setFlag(std::atomic<bool>& flag) {
    flag.store(true, std::memory_order_release);
    co_return;
}

// Submits work from several threads with pauses long enough for workers to
// park; a lost wakeup leaves a task unrun and fails the bounded wait.
TEST(ThreadPool, NoLostWakeups) {
    ThreadPool pool(ThreadPoolOptions{.workerCount = 4, .lowLaneCap = 2});
    constexpr int kSubmitters = 4;
    constexpr int kRounds = 2'000;
    std::atomic<int> failures{0};
    std::vector<std::thread> submitters;
    for (int s = 0; s < kSubmitters; ++s) {
        submitters.emplace_back([&pool, &failures, s] {
            for (int round = 0; round < kRounds; ++round) {
                std::atomic<bool> done{false};
                pool.spawn(kAllLanes[static_cast<std::size_t>(round + s) % 4], setFlag(done)).detach();
                if (!waitFor([&] { return done.load(std::memory_order_acquire); }, 10s)) {
                    failures.fetch_add(1);
                    return;
                }
                // Pause 0-60 us so workers alternate between spinning and parking.
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((round * 7 + s) % 61);
                while (std::chrono::steady_clock::now() < until) {
                }
            }
        });
    }
    for (std::thread& submitter : submitters) {
        submitter.join();
    }
    EXPECT_EQ(failures.load(), 0);
    pool.waitIdle();
}

} // namespace
