// SPDX-License-Identifier: Apache-2.0
//
// Tests for SimExecutor: virtual-time timers (jumps, exact deadlines, equal
// deadlines, cancellation in virtual time), lane priority and aging identical
// to the pool's policy, join() driving the simulator, shutdown draining, and
// determinism (same seed, same trace, on every platform; different seeds,
// different interleavings).

#include "frost/sched/lane_policy.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/task.hpp"

#include "sched/sim_scenario.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <ios>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::IClock;
using frost::Result;
using frost::sched::JoinHandle;
using frost::sched::kAllLanes;
using frost::sched::Lane;
using frost::sched::laneIndex;
using frost::sched::LaneSelector;
using frost::sched::SimExecutor;
using frost::sched::SimOptions;
using frost::sched::Task;

Task<void> sleeper(SimExecutor& sim, IClock::Duration duration, std::string name,
                   std::vector<std::pair<std::string, IClock::Duration>>& log) {
    const bool expired = co_await sim.sleepFor(duration);
    log.emplace_back(name + (expired ? "" : " cut"), sim.steadyNow().time_since_epoch());
}

TEST(SimExecutor, TimeJumpsToTheNextTimerOnlyWhenIdle) {
    SimExecutor sim;
    std::vector<std::pair<std::string, IClock::Duration>> log;
    sim.spawn(Lane::Background, sleeper(sim, 300ms, "c", log)).detach();
    sim.spawn(Lane::Background, sleeper(sim, 100ms, "a", log)).detach();
    sim.spawn(Lane::Interactive, sleeper(sim, 200ms, "b", log)).detach();
    EXPECT_EQ(sim.steadyNow().time_since_epoch(), 0ns);
    sim.runUntilIdle();
    using Entry = std::pair<std::string, IClock::Duration>;
    EXPECT_EQ(log, (std::vector<Entry>{{"a", 100ms}, {"b", 200ms}, {"c", 300ms}}));
    EXPECT_EQ(sim.steadyNow().time_since_epoch(), 300ms);
    EXPECT_EQ(sim.systemNow().time_since_epoch(), 300ms);
    EXPECT_EQ(sim.timerCount(), 0U);
}

TEST(SimExecutor, RunUntilStopsAtTheGivenTime) {
    SimExecutor sim(SimOptions{.steadyStart = IClock::SteadyTime(1h)});
    std::vector<std::pair<std::string, IClock::Duration>> log;
    sim.spawn(Lane::UserVisible, sleeper(sim, 50ms, "early", log)).detach();
    sim.spawn(Lane::UserVisible, sleeper(sim, 500ms, "late", log)).detach();
    sim.runFor(100ms);
    ASSERT_EQ(log.size(), 1U);
    EXPECT_EQ(log[0].second, 1h + 50ms);
    EXPECT_EQ(sim.steadyNow().time_since_epoch(), 1h + 100ms);
    EXPECT_EQ(sim.timerCount(), 1U);
    sim.runUntilIdle();
    EXPECT_EQ(log.size(), 2U);
}

Task<void> stepper(SimExecutor& sim, std::vector<int>& log) {
    // Runnable work goes first: time does not move while anything is ready.
    for (int i = 0; i < 3; ++i) {
        log.push_back(static_cast<int>(sim.steadyNow().time_since_epoch() / 1ms));
        co_await sim.schedule(Lane::Idle);
    }
}

TEST(SimExecutor, TimeStandsStillWhileWorkIsReady) {
    SimExecutor sim;
    std::vector<int> log;
    std::vector<std::pair<std::string, IClock::Duration>> sleeps;
    sim.spawn(Lane::Interactive, sleeper(sim, 10ms, "s", sleeps)).detach();
    sim.spawn(Lane::Idle, stepper(sim, log)).detach();
    sim.runUntilIdle();
    EXPECT_EQ(log, (std::vector<int>{0, 0, 0}));
    ASSERT_EQ(sleeps.size(), 1U);
    EXPECT_EQ(sleeps[0].second, 10ms);
}

Task<Result<int>> sleepWithToken(SimExecutor& sim, std::stop_token token) {
    const bool expired = co_await sim.sleepFor(1h, token);
    if (!expired) {
        co_return frost::fail(ErrorCode::Cancelled, "stopped");
    }
    co_return 1;
}

Task<void> stopAt(SimExecutor& sim, IClock::Duration when, std::stop_source& source) {
    (void)co_await sim.sleepFor(when);
    source.request_stop();
}

TEST(SimExecutor, StoppingATokenEndsASleepInVirtualTime) {
    SimExecutor sim;
    std::stop_source source;
    JoinHandle<Result<int>> handle = sim.spawn(Lane::Background, sleepWithToken(sim, source.get_token()));
    sim.spawn(Lane::Interactive, stopAt(sim, 250ms, source)).detach();
    const Result<int> result = handle.join(); // Drives the simulator.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(sim.steadyNow().time_since_epoch(), 250ms); // Not one hour.
    EXPECT_EQ(sim.timerCount(), 0U);
}

class Recorder {
public:
    Task<void> record(Lane lane, int id) {
        m_order.emplace_back(lane, id);
        co_return;
    }
    std::vector<std::pair<Lane, int>> m_order;
};

TEST(SimExecutor, LanePriorityAndAgingMatchThePool) {
    for (std::uint64_t seed = 0; seed < 20; ++seed) {
        SimExecutor sim(SimOptions{.seed = seed});
        Recorder recorder;
        for (int id = 0; id < 40; ++id) {
            sim.spawn(Lane::Interactive, recorder.record(Lane::Interactive, id)).detach();
        }
        sim.spawn(Lane::Background, recorder.record(Lane::Background, 0)).detach();
        sim.spawn(Lane::Idle, recorder.record(Lane::Idle, 0)).detach();
        sim.runUntilIdle();
        ASSERT_EQ(recorder.m_order.size(), 42U);
        // The seed reorders within Interactive, but Background still comes after
        // exactly kAgingPasses dispatches, and Idle after its own threshold or at the end.
        const std::size_t passes = LaneSelector::kAgingPasses[laneIndex(Lane::Background)];
        EXPECT_EQ(recorder.m_order[passes].first, Lane::Background) << "seed " << seed;
        EXPECT_EQ(recorder.m_order.back().first, Lane::Idle) << "seed " << seed;
    }
}

Task<int> doubled(SimExecutor& sim, int value) {
    co_await sim.schedule(Lane::UserVisible);
    co_return value * 2;
}

TEST(SimExecutor, JoinDrivesTheSimulator) {
    SimExecutor sim;
    JoinHandle<int> handle = sim.spawn(Lane::Background, doubled(sim, 21));
    EXPECT_FALSE(handle.ready());
    EXPECT_EQ(handle.join(), 42);
    EXPECT_EQ(sim.dispatches(), 2U); // Start, then the hop.
}

Task<void> sleepUntilShutdown(SimExecutor& sim, int& cutShort, int& sawStop) {
    const bool expired = co_await sim.sleepFor(24h);
    cutShort += expired ? 0 : 1;
    sawStop += sim.stopToken().stop_requested() ? 1 : 0;
}

TEST(SimExecutor, ShutdownDrainsWithoutMovingTime) {
    int cutShort = 0;
    int sawStop = 0;
    {
        SimExecutor sim;
        for (int i = 0; i < 5; ++i) {
            sim.spawn(kAllLanes[static_cast<std::size_t>(i) % 4], sleepUntilShutdown(sim, cutShort, sawStop)).detach();
        }
        sim.runFor(1s); // All five are now sleeping.
        EXPECT_EQ(sim.timerCount(), 5U);
        sim.shutdown();
        EXPECT_EQ(sim.steadyNow().time_since_epoch(), 1s);
        EXPECT_EQ(cutShort, 5);
        // Later sleeps complete immediately.
        sim.spawn(Lane::Idle, sleepUntilShutdown(sim, cutShort, sawStop)).detach();
    } // The destructor drains the last one.
    EXPECT_EQ(cutShort, 6);
    EXPECT_EQ(sawStop, 6);
}

TEST(SimExecutor, SameSeedGivesTheSameTrace) {
    for (std::uint64_t seed : {1ULL, 7ULL, 12345ULL}) {
        const frost::sched::test::ScenarioResult first = frost::sched::test::runScenario(99, seed);
        const frost::sched::test::ScenarioResult second = frost::sched::test::runScenario(99, seed);
        EXPECT_TRUE(first.violations.empty()) << first.violations.front();
        EXPECT_GT(first.dispatches, 20U);
        EXPECT_EQ(first.traceHash, second.traceHash);
        EXPECT_EQ(first.trace, second.trace);
        EXPECT_EQ(first.log, second.log);
    }
}

// The simulator's decisions use only its own PRNG and virtual time, so a seed gives
// the same trace on every platform and standard library. This golden fingerprint
// of 100 scenario traces is the same on Windows (MSVC STL) and Linux (libstdc++).
// It changes only when the scenario, SimExecutor or LaneSelector changes on
// purpose; then update it from the failure message.
TEST(SimExecutor, TracesAreTheSameOnEveryPlatform) {
    constexpr std::uint64_t kGoldenFingerprint = 0x2c73'7477'447b'b0abULL;
    std::uint64_t fingerprint = 0;
    for (std::uint64_t seed = 0; seed < 100; ++seed) {
        fingerprint = (fingerprint * 0x100000001B3ULL) ^ frost::sched::test::runScenario(seed, seed).traceHash;
    }
    EXPECT_EQ(fingerprint, kGoldenFingerprint) << std::hex << "fingerprint 0x" << fingerprint;
}

TEST(SimExecutor, DifferentSeedsExploreDifferentInterleavings) {
    // The same workload under 16 seeds: the order in which tasks ran must differ.
    std::set<std::vector<std::string>> logs;
    std::set<std::uint64_t> hashes;
    for (std::uint64_t seed = 0; seed < 16; ++seed) {
        const frost::sched::test::ScenarioResult result = frost::sched::test::runScenario(99, seed);
        EXPECT_TRUE(result.violations.empty()) << "seed " << seed << ": " << result.violations.front();
        logs.insert(result.log);
        hashes.insert(result.traceHash);
    }
    EXPECT_GE(logs.size(), 8U);
    EXPECT_GE(hashes.size(), 8U);
}

} // namespace
