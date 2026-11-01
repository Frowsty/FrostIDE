// SPDX-License-Identifier: Apache-2.0
//
// Stress tests for frost::sched (label `stress`; skipped in the plain Debug
// configuration, run in the configurations -Full adds: Release and ASan on
// Windows, ASan+UBSan and TSan in WSL). M0 exit criteria (PLAN.md
// section 7, "M0 Exit"):
//   - SimExecutor seeds: 10k seeds of the randomised scenario, invariants and
//     the lane-policy oracle checked for each, determinism re-checked on a sample.
//   - Real pool: 1M tasks across all lanes with cancellation, 100 iterations.
// The default scale keeps a TSan -Full gate within a few minutes; set
// FROST_STRESS_FULL=1 for the exit scale (the seed test always runs it). Each
// test prints its scale and wall time.

#include "frost/sched/chase_lev_deque.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"
#include "frost/sched/thread_pool.hpp"

#include "sched/sched_test_support.hpp"
#include "sched/sim_scenario.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::Result;
using frost::sched::ChaseLevDeque;
using frost::sched::JoinHandle;
using frost::sched::kAllLanes;
using frost::sched::Lane;
using frost::sched::Task;
using frost::sched::ThreadPool;
using frost::sched::detail::SplitMix64;
using frost::sched::test::stressFull;

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// --- 10k SimExecutor seeds ---

TEST(SchedStress, SimExecutorSeeds) {
    // The exit scale takes about a second even under TSan, so it is also the default.
    constexpr std::uint64_t seeds = 10'000;
    // Seeds are independent simulators, so they run on several threads; this
    // also checks that SimExecutor instances share no hidden state.
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1U, 8U);
    std::atomic<std::uint64_t> next{0};
    std::atomic<std::uint64_t> dispatches{0};
    std::atomic<int> failedSeeds{0};
    std::mutex mutex;
    std::string firstFailure;
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::uint64_t seed = next.fetch_add(1); seed < seeds; seed = next.fetch_add(1)) {
                const frost::sched::test::ScenarioResult result = frost::sched::test::runScenario(seed, seed);
                dispatches.fetch_add(result.dispatches, std::memory_order_relaxed);
                std::string failure;
                if (!result.violations.empty()) {
                    failure = result.violations.front();
                } else if (seed % 97 == 0 &&
                           frost::sched::test::runScenario(seed, seed).traceHash != result.traceHash) {
                    failure = "trace differs between two runs";
                }
                if (!failure.empty()) {
                    failedSeeds.fetch_add(1);
                    std::lock_guard lock(mutex);
                    if (firstFailure.empty()) {
                        firstFailure = "seed " + std::to_string(seed) + ": " + failure;
                    }
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::printf("[sched stress] SimExecutor: %llu seeds, %llu dispatches, %u threads, %.1f s\n",
                static_cast<unsigned long long>(seeds), static_cast<unsigned long long>(dispatches.load()), threads,
                secondsSince(start));
    EXPECT_EQ(failedSeeds.load(), 0) << firstFailure;
}

// --- Real pool: tasks across all lanes with cancellation ---

struct PoolStress {
    explicit PoolStress(std::uint32_t leaves)
        : count(leaves), runs(std::make_unique<std::atomic<std::uint8_t>[]>(leaves)) {}
    std::uint32_t count;
    std::unique_ptr<std::atomic<std::uint8_t>[]> runs; // Body entries per leaf: must end 0 (skipped) or 1.
    std::atomic<std::uint64_t> valueSum{0};            // Sum of every successful leaf result.
    std::atomic<std::uint64_t> cutShort{0};            // Leaves that saw their stop while running.
};

Task<Result<int>> leaf(ThreadPool& pool, PoolStress& state, std::uint32_t id, std::stop_token token) {
    state.runs[id].fetch_add(1, std::memory_order_relaxed);
    switch (id % 16) {
    case 0:
        co_await pool.schedule(kAllLanes[(id / 16) % 4]); // Lane hop (local push, then dispatch or steal).
        break;
    case 1:
        if (id % 1024 == 1 && !co_await pool.sleepFor(200us, token)) {
            state.cutShort.fetch_add(1, std::memory_order_relaxed);
            co_return frost::fail(ErrorCode::Cancelled, "cut short while sleeping");
        }
        break;
    case 2:
        co_await pool.schedule(Lane::Idle);
        if (token.stop_requested()) {
            state.cutShort.fetch_add(1, std::memory_order_relaxed);
            co_return frost::fail(ErrorCode::Cancelled, "stopped while running");
        }
        break;
    default: break;
    }
    const int value = static_cast<int>(id & 0xFFU);
    state.valueSum.fetch_add(static_cast<std::uint64_t>(value), std::memory_order_relaxed);
    co_return value;
}

struct BranchTotals {
    std::uint64_t valueSum = 0;
    std::uint64_t skipped = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t badSkips = 0; // Skipped leaves whose body ran anyway.
};

// Spawns leaves [first, first + count) in batches; a quarter of the batches
// is cancelled halfway through spawning, so leaves are skipped before start,
// stopped while running, or complete, depending on the interleaving.
Task<BranchTotals> branch(ThreadPool& pool, PoolStress& state, std::uint32_t first, std::uint32_t count,
                          std::uint64_t seed) {
    constexpr std::uint32_t kBatch = 256;
    SplitMix64 rng(seed);
    BranchTotals totals;
    std::vector<JoinHandle<Result<int>>> handles;
    handles.reserve(kBatch);
    for (std::uint32_t batch = first; batch < first + count; batch += kBatch) {
        const std::uint32_t end = std::min(batch + kBatch, first + count);
        std::stop_source source;
        const bool cancelBatch = rng.below(4) == 0;
        handles.clear();
        for (std::uint32_t id = batch; id < end; ++id) {
            const Lane lane = kAllLanes[rng.below(4)];
            if (cancelBatch && id % 2 == 0) {
                handles.push_back(pool.spawn(lane, leaf(pool, state, id, source.get_token()), source.get_token()));
            } else {
                handles.push_back(pool.spawn(lane, leaf(pool, state, id, {})));
            }
            if (cancelBatch && id == batch + (end - batch) / 2) {
                source.request_stop();
            }
        }
        for (std::size_t i = 0; i < handles.size(); ++i) {
            const Result<int> result = co_await handles[i];
            if (handles[i].cancelledBeforeStart()) {
                ++totals.skipped;
                totals.badSkips += state.runs[batch + i].load(std::memory_order_relaxed) == 0 ? 0 : 1;
            } else if (result) {
                totals.valueSum += static_cast<std::uint64_t>(*result);
            } else {
                ++totals.cancelled;
            }
        }
    }
    co_return totals;
}

TEST(SchedStress, RealPoolMillionTasksWithCancellation) {
    // 1M tasks per iteration always; FROST_STRESS_FULL=1 runs the exit scale of 100
    // iterations (145 s under TSan in WSL on REF-W1), the default 20 (29 s).
    constexpr std::uint32_t leaves = 1'000'000;
    const int iterations = stressFull() ? 100 : 20;
    constexpr std::uint32_t kRoots = 64;
    ThreadPool pool;
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t totalSkipped = 0;
    std::uint64_t totalCut = 0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        PoolStress state(leaves);
        std::vector<JoinHandle<BranchTotals>> roots;
        const std::uint32_t perRoot = leaves / kRoots;
        for (std::uint32_t r = 0; r < kRoots; ++r) {
            roots.push_back(
                pool.spawn(kAllLanes[r % 4], branch(pool, state, r * perRoot, perRoot,
                                                    static_cast<std::uint64_t>(iteration) * 1'000'003ULL + r)));
        }
        BranchTotals sum;
        for (JoinHandle<BranchTotals>& root : roots) {
            const BranchTotals part = root.join();
            sum.valueSum += part.valueSum;
            sum.skipped += part.skipped;
            sum.cancelled += part.cancelled;
            sum.badSkips += part.badSkips;
        }
        pool.waitIdle();
        // Every leaf body ran at most once; exactly the skipped ones never ran.
        std::uint64_t ran = 0;
        std::uint64_t ranTwice = 0;
        for (std::uint32_t id = 0; id < leaves; ++id) {
            const std::uint8_t runs = state.runs[id].load(std::memory_order_relaxed);
            ran += runs;
            ranTwice += runs > 1 ? 1 : 0;
        }
        ASSERT_EQ(ranTwice, 0U) << "iteration " << iteration;
        ASSERT_EQ(sum.badSkips, 0U) << "iteration " << iteration;
        ASSERT_EQ(ran + sum.skipped, leaves) << "iteration " << iteration;
        ASSERT_EQ(sum.cancelled, state.cutShort.load()) << "iteration " << iteration;
        ASSERT_EQ(sum.valueSum, state.valueSum.load()) << "iteration " << iteration;
        ASSERT_GT(sum.skipped + sum.cancelled, 0U) << "cancellation never happened";
        totalSkipped += sum.skipped;
        totalCut += sum.cancelled;
    }
    std::printf("[sched stress] ThreadPool: %d x %u tasks on %u workers (cap %u), %llu skipped, %llu cut short, "
                "%.1f s\n",
                iterations, leaves, pool.workerCount(), pool.lowLaneCap(),
                static_cast<unsigned long long>(totalSkipped), static_cast<unsigned long long>(totalCut),
                secondsSince(start));
}

// --- Long deque run: one owner, several thieves ---

TEST(SchedStress, DequeOwnerAndThievesLongRun) {
    const int items = stressFull() ? 20'000'000 : 2'000'000;
    constexpr int kThieves = 6;
    std::vector<std::uint32_t> values(static_cast<std::size_t>(items));
    for (int i = 0; i < items; ++i) {
        values[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(i);
    }
    std::unique_ptr<std::atomic<std::uint8_t>[]> taken = std::make_unique<std::atomic<std::uint8_t>[]>(values.size());
    ChaseLevDeque<std::uint32_t> deque(2);
    std::atomic<bool> ownerDone{false};
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> thieves;
    for (int t = 0; t < kThieves; ++t) {
        thieves.emplace_back([&] {
            for (;;) {
                const auto result = deque.steal();
                if (result.status == ChaseLevDeque<std::uint32_t>::StealStatus::Success) {
                    taken[*result.item].fetch_add(1, std::memory_order_relaxed);
                } else if (result.status == ChaseLevDeque<std::uint32_t>::StealStatus::Empty &&
                           ownerDone.load(std::memory_order_acquire)) {
                    return;
                }
            }
        });
    }
    SplitMix64 rng(42);
    for (std::uint32_t& value : values) {
        deque.push(&value);
        // Bursts of pops keep the deque near empty, where owner/thief races on the last element happen.
        if (rng.below(3) == 0) {
            for (std::uint64_t n = rng.below(4); n > 0; --n) {
                if (std::uint32_t* item = deque.pop()) {
                    taken[*item].fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    while (std::uint32_t* item = deque.pop()) {
        taken[*item].fetch_add(1, std::memory_order_relaxed);
    }
    ownerDone.store(true, std::memory_order_release);
    for (std::thread& thief : thieves) {
        thief.join();
    }
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        wrong += taken[i].load(std::memory_order_relaxed) == 1 ? 0 : 1;
    }
    std::printf("[sched stress] deque: %d items, %d thieves, %.1f s\n", items, kThieves, secondsSince(start));
    EXPECT_EQ(wrong, 0U);
}

} // namespace
