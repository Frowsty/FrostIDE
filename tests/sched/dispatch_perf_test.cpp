// SPDX-License-Identifier: Apache-2.0
//
// Interactive dispatch latency under Background saturation (label `perf`).
// M0 exit criterion (PLAN.md section 7, "M0 Exit"): with 10k Background tasks
// queued and the Background workers saturated, an Interactive task starts
// within 1 ms of submission at p99. Asserted in optimised, unsanitised builds;
// Debug and sanitizer builds only report the numbers.
//
// Method: a default-sized pool (N-1 workers, cap N-4). lowLaneCap() Background
// tasks are always running (each busy for kBackgroundSlice, then respawning a
// successor), with 10k more queued. Samples alternate between two submitters:
//   - external: the test thread spawns the probe (the UI-thread path: injection
//     queue plus waking a parked worker);
//   - from a worker: a running Background task spawns the probe into its own
//     deque, so another worker must be woken and steal it.
// Latency = probe's first instruction minus the submit time (steady_clock).
// Between samples the test thread waits 100-600 us so idle workers park again.

#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"
#include "frost/sched/thread_pool.hpp"

#include "sched/sched_test_support.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::sched::Lane;
using frost::sched::Task;
using frost::sched::ThreadPool;
using frost::sched::test::kAssertBudgets;
using Clock = std::chrono::steady_clock;

constexpr int kQueuedBackground = 10'000;
constexpr auto kBackgroundSlice = 500us; // Busy time of one Background task.

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct Shared {
    std::atomic<bool> stop{false};
    std::atomic<bool> workerSubmitRequested{false};
    std::atomic<std::int64_t> submitNs{0};
    std::atomic<std::int64_t> probeStartNs{0};
};

Task<void> probe(Shared& shared) {
    shared.probeStartNs.store(nowNs(), std::memory_order_release);
    co_return;
}

Task<void> background(ThreadPool& pool, Shared& shared) {
    const auto until = Clock::now() + kBackgroundSlice;
    while (Clock::now() < until) {
        if (shared.workerSubmitRequested.exchange(false, std::memory_order_acq_rel)) {
            shared.submitNs.store(nowNs(), std::memory_order_release);
            pool.spawn(Lane::Interactive, probe(shared)).detach();
        }
    }
    if (!shared.stop.load(std::memory_order_acquire)) {
        pool.spawn(Lane::Background, background(pool, shared)).detach(); // Keep the queue at ~10k.
    }
    co_return;
}

struct Percentiles {
    double p50us;
    double p99us;
    double maxUs;
};

Percentiles percentiles(std::vector<std::int64_t> samples) {
    std::sort(samples.begin(), samples.end());
    auto at = [&](double q) {
        const std::size_t index =
            std::min(samples.size() - 1, static_cast<std::size_t>(q * static_cast<double>(samples.size())));
        return static_cast<double>(samples[index]) / 1000.0;
    };
    return {at(0.50), at(0.99), static_cast<double>(samples.back()) / 1000.0};
}

TEST(SchedPerf, InteractiveDispatchP99UnderBackgroundSaturation) {
    // M0 exit criterion: interactive dispatch p99 <= 1 ms with 10k Background tasks queued
    // and Background workers saturated (PLAN.md section 7, "M0 Exit"; no B ID assigned yet).
    const int samplesPerKind = kAssertBudgets ? 2'000 : 300;
    Shared shared;
    ThreadPool pool;
    const unsigned cap = pool.lowLaneCap();
    for (int i = 0; i < kQueuedBackground + static_cast<int>(cap); ++i) {
        pool.spawn(Lane::Background, background(pool, shared)).detach();
    }
    ASSERT_TRUE(frost::sched::test::waitFor([&] { return pool.stats().lowRunning == static_cast<std::int64_t>(cap); }));

    std::vector<std::int64_t> external;
    std::vector<std::int64_t> fromWorker;
    int saturated = 0;
    std::int64_t occupancy = 0; // Sum of lowRunning over the samples.
    std::uint64_t pause = 12345;
    for (int sample = 0; sample < 2 * samplesPerKind; ++sample) {
        // Saturated: ~10k queued and the low-lane slots busy. With cap tasks of
        // kBackgroundSlice each, one slot is mid-handoff (finished, successor not yet
        // started) a few percent of the time, so one free slot still counts.
        const frost::sched::ThreadPoolStats stats = pool.stats();
        const std::int64_t queuedBackground = stats.queued[frost::sched::laneIndex(Lane::Background)];
        occupancy += stats.lowRunning;
        saturated +=
            stats.lowRunning + 1 >= static_cast<std::int64_t>(cap) && queuedBackground >= kQueuedBackground - 100 ? 1
                                                                                                                  : 0;

        shared.probeStartNs.store(0, std::memory_order_relaxed);
        const bool externalKind = sample % 2 == 0;
        if (externalKind) {
            shared.submitNs.store(nowNs(), std::memory_order_relaxed);
            pool.spawn(Lane::Interactive, probe(shared)).detach();
        } else {
            shared.submitNs.store(0, std::memory_order_relaxed);
            shared.workerSubmitRequested.store(true, std::memory_order_release);
        }
        ASSERT_TRUE(
            frost::sched::test::waitFor([&] { return shared.probeStartNs.load(std::memory_order_acquire) != 0; }, 10s))
            << "probe " << sample << " never started";
        const std::int64_t latency =
            shared.probeStartNs.load(std::memory_order_acquire) - shared.submitNs.load(std::memory_order_acquire);
        (externalKind ? external : fromWorker).push_back(latency);

        // 100-600 us pause (busy, because sleep_for is far coarser on Windows).
        pause = pause * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto until = Clock::now() + std::chrono::microseconds(100 + static_cast<int>((pause >> 33) % 500));
        while (Clock::now() < until) {
        }
    }
    shared.stop.store(true, std::memory_order_release);

    const Percentiles ext = percentiles(external);
    const Percentiles wrk = percentiles(fromWorker);
    const double saturatedShare = static_cast<double>(saturated) / static_cast<double>(2 * samplesPerKind);
    const double meanOccupancy = static_cast<double>(occupancy) / static_cast<double>(2 * samplesPerKind);
    std::printf("[sched perf] workers %u, low-lane cap %u, mean low-lane occupancy %.2f, saturated %.1f%% of "
                "samples, %s build\n",
                pool.workerCount(), cap, meanOccupancy, saturatedShare * 100.0,
                kAssertBudgets ? "budgeted" : "report-only");
    std::printf("[sched perf] external submit:  p50 %.1f us, p99 %.1f us, max %.1f us (%d samples)\n", ext.p50us,
                ext.p99us, ext.maxUs, samplesPerKind);
    std::printf("[sched perf] from a worker:    p50 %.1f us, p99 %.1f us, max %.1f us (%d samples)\n", wrk.p50us,
                wrk.p99us, wrk.maxUs, samplesPerKind);
    RecordProperty("external_p99_us", std::to_string(ext.p99us));
    RecordProperty("worker_p99_us", std::to_string(wrk.p99us));

    // The measurement is only valid while Background really was saturated.
    EXPECT_GE(saturatedShare, 0.99);
    if (kAssertBudgets) {
        EXPECT_LE(ext.p99us, 1000.0) << "M0 exit: interactive dispatch p99 <= 1 ms (external submit)";
        EXPECT_LE(wrk.p99us, 1000.0) << "M0 exit: interactive dispatch p99 <= 1 ms (submit from a worker)";
    }
}

} // namespace
