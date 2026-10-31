// SPDX-License-Identifier: Apache-2.0
//
// Tests for LatestWins: a new request stops the previous one, only the latest
// may commit, cancel() refuses everything, generations stay strictly increasing
// under concurrent next(), and an end-to-end "type ahead" run on the simulator
// where superseded requests are skipped or cut short and only the last commits.

#include "frost/sched/latest_wins.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/task.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::Result;
using frost::sched::JoinHandle;
using frost::sched::Lane;
using frost::sched::LatestWins;
using frost::sched::SimExecutor;
using frost::sched::Task;

TEST(LatestWins, NextStopsThePreviousRequest) {
    LatestWins latest;
    EXPECT_EQ(latest.generation(), 0U);
    const LatestWins::Ticket first = latest.next();
    EXPECT_EQ(first.generation, 1U);
    EXPECT_TRUE(latest.isCurrent(first));
    EXPECT_FALSE(first.token.stop_requested());
    const LatestWins::Ticket second = latest.next();
    EXPECT_EQ(second.generation, 2U);
    EXPECT_TRUE(first.token.stop_requested());
    EXPECT_FALSE(second.token.stop_requested());
    EXPECT_FALSE(latest.isCurrent(first));
    EXPECT_TRUE(latest.isCurrent(second));
}

TEST(LatestWins, OnlyTheLatestTicketCommits) {
    LatestWins latest;
    const LatestWins::Ticket old = latest.next();
    const LatestWins::Ticket current = latest.next();
    int published = 0;
    EXPECT_FALSE(latest.commitIfCurrent(old, [&] { published = 1; }));
    EXPECT_EQ(published, 0);
    EXPECT_TRUE(latest.commitIfCurrent(current, [&] { published = 2; }));
    EXPECT_EQ(published, 2);
}

TEST(LatestWins, CancelRefusesEveryOutstandingTicket) {
    LatestWins latest;
    const LatestWins::Ticket ticket = latest.next();
    latest.cancel();
    EXPECT_TRUE(ticket.token.stop_requested());
    EXPECT_FALSE(latest.isCurrent(ticket));
    EXPECT_FALSE(latest.commitIfCurrent(ticket, [] {}));
    // A new request after cancel() works normally.
    const LatestWins::Ticket fresh = latest.next();
    EXPECT_TRUE(latest.commitIfCurrent(fresh, [] {}));
}

TEST(LatestWins, ConcurrentRequestsGetUniqueIncreasingGenerations) {
    LatestWins latest;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 2'000;
    std::vector<std::vector<std::uint64_t>> seen(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&latest, &seen, t] {
            std::uint64_t previous = 0;
            for (int i = 0; i < kPerThread; ++i) {
                const LatestWins::Ticket ticket = latest.next();
                EXPECT_GT(ticket.generation, previous);
                previous = ticket.generation;
                seen[static_cast<std::size_t>(t)].push_back(ticket.generation);
                (void)latest.commitIfCurrent(ticket, [] {});
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    std::vector<std::uint64_t> all;
    for (const std::vector<std::uint64_t>& part : seen) {
        all.insert(all.end(), part.begin(), part.end());
    }
    std::sort(all.begin(), all.end());
    EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end());
    EXPECT_EQ(all.back(), static_cast<std::uint64_t>(kThreads * kPerThread));
    EXPECT_EQ(latest.generation(), all.back());
}

// A completion request: debounce 30 ms, then "compute" and publish if still current.
Task<Result<int>> completion(SimExecutor& sim, LatestWins& latest, LatestWins::Ticket ticket, int& published) {
    if (!co_await sim.sleepFor(30ms, ticket.token)) {
        co_return frost::fail(ErrorCode::Cancelled, "superseded while debouncing");
    }
    const int value = static_cast<int>(ticket.generation) * 10;
    if (!latest.commitIfCurrent(ticket, [&] { published = value; })) {
        co_return frost::fail(ErrorCode::Cancelled, "stale");
    }
    co_return value;
}

// Keystrokes every 10 ms supersede the previous request; only the last survives.
Task<void> typist(SimExecutor& sim, LatestWins& latest, int& published, std::vector<JoinHandle<Result<int>>>& out) {
    for (int key = 0; key < 5; ++key) {
        const LatestWins::Ticket ticket = latest.next();
        out.push_back(sim.spawn(Lane::Interactive, completion(sim, latest, ticket, published), ticket.token));
        (void)co_await sim.sleepFor(10ms);
    }
}

TEST(LatestWins, SupersededRequestsAreCancelledAndTheLastCommits) {
    for (std::uint64_t seed = 0; seed < 8; ++seed) {
        int published = 0;
        LatestWins latest;
        std::vector<JoinHandle<Result<int>>> requests;
        SimExecutor sim(frost::sched::SimOptions{.seed = seed});
        sim.spawn(Lane::Interactive, typist(sim, latest, published, requests)).detach();
        sim.runUntilIdle();
        ASSERT_EQ(requests.size(), 5U);
        for (std::size_t i = 0; i + 1 < requests.size(); ++i) {
            const Result<int> result = requests[i].join();
            ASSERT_FALSE(result.has_value()) << "request " << i;
            EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
        }
        EXPECT_EQ(requests.back().join(), 50);
        EXPECT_EQ(published, 50);
        // The last keystroke happened at 40 ms; its debounce ends at 70 ms.
        EXPECT_EQ(sim.steadyNow().time_since_epoch(), 70ms);
    }
}

} // namespace
