// SPDX-License-Identifier: Apache-2.0
//
// A randomised SimExecutor workload with invariant checks, shared by the
// determinism unit tests and the 10k-seed stress test.
//
// The workload (roots, their operations, lanes, sleeps, cancellations, throws,
// latest-wins requests) is generated up front from `workloadSeed`; the
// interleaving comes from the simulator's own seed. Running a scenario checks:
//   - every spawned task either ran to completion or was skipped before start
//     (and a skipped one never ran);
//   - a sleep that expired resumes exactly at its deadline in virtual time, and
//     a cut-short one no later;
//   - schedule(lane) resumes on that lane;
//   - each root's checksum is at least its interleaving-independent part
//     (compute chains and children that can be neither cancelled nor throw),
//     and exceptions in Result tasks arrive as ErrorCode::Internal;
//   - latest-wins: commits arrive in increasing generation order and the last
//     request is the one that sticks;
//   - the dispatch trace obeys the lane policy, checked by an independent
//     oracle (priority, aging, and the LaneSelector::maxWait() bound).
#pragma once

#include "frost/core/error.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/sched/lane_policy.hpp"
#include "frost/sched/latest_wins.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/task.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace frost::sched::test {

struct ScenarioResult {
    std::uint64_t traceHash = 0;
    std::uint64_t dispatches = 0;
    std::vector<std::string> log;        // What the tasks did, in execution order.
    std::vector<SimTraceEvent> trace;    // The simulator's dispatch trace.
    std::vector<std::string> violations; // Failed invariants (empty when everything held).
};

namespace scenario {

enum class OpKind : std::uint8_t { Compute, Hop, Sleep, SpawnJoin, Latest };
enum class StopMode : std::uint8_t { None, RightAfterSpawn, AfterAYield };

struct Op {
    OpKind kind = OpKind::Compute;
    Lane lane = Lane::Interactive;
    int value = 0;
    int sleepMs = 0;
    StopMode stop = StopMode::None;
    bool throws = false;
};

// State the scenario's tasks share. Declared before the SimExecutor (which
// drains its tasks when destroyed), so it outlives every task that uses it.
struct Context {
    SimExecutor* sim = nullptr; // Set right after the simulator is constructed.
    std::vector<std::string> log;
    std::vector<std::string> violations;
    LatestWins latest;
    std::uint64_t lastCommitted = 0;
    int spawned = 0;
    int bodiesRun = 0;
    int skipped = 0;

    void check(bool ok, const std::string& what) {
        if (!ok) {
            violations.push_back(what);
        }
    }
};

inline constexpr std::array<Lane, kLaneCount> kLanes = kAllLanes;

// Pure, so every interleaving yields the same value.
inline Task<int> computeChain(int depth, int value) {
    if (depth == 0) {
        co_return value;
    }
    const int inner = co_await computeChain(depth - 1, value + 1);
    co_return inner * 2 + 1;
}

inline int expectedChain(int depth, int value) {
    if (depth == 0) {
        return value;
    }
    return expectedChain(depth - 1, value + 1) * 2 + 1;
}

// Sleeps until now + ms and checks the virtual-time contract.
inline Task<bool> checkedSleep(Context& ctx, int ms, std::stop_token token, std::string who) {
    const IClock::SteadyTime deadline = ctx.sim->steadyNow() + std::chrono::milliseconds(ms);
    const bool expired = co_await ctx.sim->sleepUntil(deadline, std::move(token));
    const IClock::SteadyTime now = ctx.sim->steadyNow();
    if (expired) {
        ctx.check(now == deadline, who + ": expired sleep resumed off its deadline");
    } else {
        ctx.check(now <= deadline, who + ": cut-short sleep resumed after its deadline");
    }
    co_return expired;
}

inline Task<Result<int>> child(Context& ctx, std::string name, Lane lane, int value, int sleepMs, bool throws,
                               std::stop_token token) {
    ++ctx.bodiesRun;
    ctx.check(ctx.sim->currentLane() == lane, name + ": child started on the wrong lane");
    ctx.log.push_back(name + " start");
    if (sleepMs > 0) {
        const bool expired = co_await checkedSleep(ctx, sleepMs, token, name);
        if (!expired) {
            ctx.log.push_back(name + " cut");
            co_return frost::fail(ErrorCode::Cancelled, "cut short");
        }
    }
    if (throws) {
        throw std::runtime_error("scenario throw");
    }
    ctx.log.push_back(name + " done");
    co_return value * 3;
}

inline Task<void> latestRequest(Context& ctx, LatestWins::Ticket ticket, int sleepMs) {
    const std::string name = "latest#" + std::to_string(ticket.generation);
    const bool expired = co_await checkedSleep(ctx, sleepMs, ticket.token, name);
    if (!expired) {
        ctx.log.push_back(name + " superseded");
        co_return;
    }
    const bool committed = ctx.latest.commitIfCurrent(ticket, [&] {
        ctx.check(ticket.generation > ctx.lastCommitted, name + ": commits out of order");
        ctx.lastCommitted = ticket.generation;
    });
    ctx.log.push_back(name + (committed ? " committed" : " stale"));
}

inline Task<int> root(Context& ctx, int id, std::vector<Op> plan) {
    int checksum = 0;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const Op& op = plan[i];
        const std::string name = "r" + std::to_string(id) + "." + std::to_string(i);
        switch (op.kind) {
        case OpKind::Compute: checksum += co_await computeChain(op.value % 4, op.value); break;
        case OpKind::Hop:
            co_await ctx.sim->schedule(op.lane);
            ctx.check(ctx.sim->currentLane() == op.lane, name + ": hop resumed on the wrong lane");
            ctx.log.push_back(name + " hop " + std::string(toString(op.lane)));
            break;
        case OpKind::Sleep:
            (void)co_await checkedSleep(ctx, op.sleepMs, {}, name);
            ctx.log.push_back(name + " slept");
            break;
        case OpKind::SpawnJoin: {
            std::stop_source source;
            const std::stop_token token = op.stop == StopMode::None ? std::stop_token{} : source.get_token();
            ++ctx.spawned;
            JoinHandle<Result<int>> handle =
                ctx.sim->spawn(op.lane, child(ctx, name + "c", op.lane, op.value, op.sleepMs, op.throws, token), token);
            if (op.stop == StopMode::RightAfterSpawn) {
                source.request_stop();
            } else if (op.stop == StopMode::AfterAYield) {
                co_await ctx.sim->schedule(op.lane);
                source.request_stop();
            }
            const Result<int> result = co_await handle;
            if (handle.cancelledBeforeStart()) {
                ++ctx.skipped;
                ctx.check(op.stop != StopMode::None, name + ": skipped without a stop");
                ctx.check(!result && result.error().code() == ErrorCode::Cancelled, name + ": skip not Cancelled");
                ctx.log.push_back(name + " skipped");
            } else if (result) {
                ctx.check(!op.throws && *result == op.value * 3, name + ": wrong child value");
                checksum += *result;
            } else if (result.error().code() == ErrorCode::Internal) {
                ctx.check(op.throws, name + ": Internal error without a throw");
            } else {
                ctx.check(result.error().code() == ErrorCode::Cancelled && op.stop != StopMode::None,
                          name + ": unexpected error " + result.error().describe());
            }
            break;
        }
        case OpKind::Latest: {
            const LatestWins::Ticket ticket = ctx.latest.next();
            ctx.sim->spawn(Lane::Interactive, latestRequest(ctx, ticket, op.sleepMs)).detach();
            break;
        }
        }
    }
    co_return checksum;
}

// What root(plan) must return regardless of interleaving, except for children that may be cancelled.
inline int expectedChecksumLowerBound(const std::vector<Op>& plan) {
    int sum = 0;
    for (const Op& op : plan) {
        if (op.kind == OpKind::Compute) {
            sum += expectedChain(op.value % 4, op.value);
        } else if (op.kind == OpKind::SpawnJoin && !op.throws && op.stop == StopMode::None) {
            sum += op.value * 3;
        }
    }
    return sum;
}

inline std::vector<std::vector<Op>> generatePlans(std::uint64_t workloadSeed) {
    detail::SplitMix64 rng(workloadSeed ^ 0xA5A5'5A5A'1234'5678ULL);
    const std::size_t roots = 4 + static_cast<std::size_t>(rng.below(9));
    std::vector<std::vector<Op>> plans(roots);
    for (std::vector<Op>& plan : plans) {
        const std::size_t ops = 6 + static_cast<std::size_t>(rng.below(15));
        for (std::size_t i = 0; i < ops; ++i) {
            Op op;
            op.kind = static_cast<OpKind>(rng.below(5));
            op.lane = kLanes[rng.below(kLaneCount)];
            op.value = static_cast<int>(rng.below(100));
            op.sleepMs = static_cast<int>(rng.below(4)) * 5; // 0, 5, 10 or 15 ms: equal deadlines are common.
            op.stop = static_cast<StopMode>(rng.below(3));
            op.throws = rng.below(8) == 0;
            if (op.kind == OpKind::Latest && op.sleepMs == 0) {
                op.sleepMs = 5;
            }
            plan.push_back(op);
        }
    }
    return plans;
}

// Re-derives every dispatch decision from the trace with an independent
// implementation of the documented policy, and measures waits against the bound.
inline void checkLanePolicy(const std::vector<SimTraceEvent>& trace, std::vector<std::string>& violations) {
    std::array<std::uint32_t, kLaneCount> passes{};
    std::array<std::uint32_t, kLaneCount> waiting{};
    for (const SimTraceEvent& event : trace) {
        // Highest-priority aged lane with work, else highest-priority lane with work.
        int expected = -1;
        for (std::size_t lane = 1; lane < kLaneCount && expected < 0; ++lane) {
            if ((event.ready >> lane & 1U) != 0 && passes[lane] >= LaneSelector::kAgingPasses[lane]) {
                expected = static_cast<int>(lane);
            }
        }
        for (std::size_t lane = 0; lane < kLaneCount && expected < 0; ++lane) {
            if ((event.ready >> lane & 1U) != 0) {
                expected = static_cast<int>(lane);
            }
        }
        const std::size_t chosen = laneIndex(event.lane);
        if (static_cast<int>(chosen) != expected) {
            violations.push_back("dispatch " + std::to_string(event.dispatch) + " chose lane " +
                                 std::to_string(chosen) + ", policy says " + std::to_string(expected));
        }
        passes[chosen] = 0;
        for (std::size_t lane = 0; lane < kLaneCount; ++lane) {
            const bool ready = (event.ready >> lane & 1U) != 0;
            if (lane > chosen && ready) {
                passes[lane] = std::min(passes[lane] + 1, LaneSelector::kAgingPasses[lane]);
            }
            waiting[lane] = (lane == chosen || !ready) ? 0 : waiting[lane] + 1;
            if (waiting[lane] > LaneSelector::maxWait(kAllLanes[lane])) {
                violations.push_back("lane " + std::to_string(lane) + " waited " + std::to_string(waiting[lane]) +
                                     " dispatches at dispatch " + std::to_string(event.dispatch));
            }
        }
    }
}

} // namespace scenario

// Runs the workload of `workloadSeed` on a SimExecutor seeded with `simSeed`.
inline ScenarioResult runScenario(std::uint64_t workloadSeed, std::uint64_t simSeed) {
    using namespace scenario;
    ScenarioResult out;
    const std::vector<std::vector<Op>> plans = generatePlans(workloadSeed);
    {
        Context ctx;
        SimExecutor sim(SimOptions{.seed = simSeed, .recordTrace = true});
        ctx.sim = &sim;
        std::vector<JoinHandle<int>> roots;
        for (std::size_t r = 0; r < plans.size(); ++r) {
            roots.push_back(sim.spawn(kLanes[r % kLaneCount], root(ctx, static_cast<int>(r), plans[r])));
        }
        sim.runUntilIdle();
        for (std::size_t r = 0; r < plans.size(); ++r) {
            if (!roots[r].ready()) {
                ctx.violations.push_back("root " + std::to_string(r) + " did not finish");
                continue;
            }
            const int checksum = roots[r].join();
            ctx.check(checksum >= expectedChecksumLowerBound(plans[r]), "root " + std::to_string(r) + " checksum");
        }
        ctx.check(ctx.spawned == ctx.bodiesRun + ctx.skipped, "spawned != ran + skipped");
        ctx.check(ctx.lastCommitted == ctx.latest.generation(), "the latest request did not win");
        ctx.check(sim.readyCount() == 0 && sim.timerCount() == 0, "work left after runUntilIdle");
        checkLanePolicy(sim.trace(), ctx.violations);
        out.traceHash = sim.traceHash();
        out.dispatches = sim.dispatches();
        out.trace = sim.trace();
        out.log = std::move(ctx.log);
        out.violations = std::move(ctx.violations);
    }
    return out;
}

} // namespace frost::sched::test
