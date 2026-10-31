// SPDX-License-Identifier: Apache-2.0
//
// SimExecutor: the deterministic IScheduler double (PLAN.md section 6.2,
// "IScheduler -> seeded SimExecutor"). Single-threaded, seeded, virtual time.
//
/*
 * Model. The simulator is one dispatcher with no threads of its own: the test
 * drives it (step(), runUntilIdle(), runUntil(), or JoinHandle::join(), which
 * steps until the task is done). Each dispatch:
 *   1. asks a LaneSelector, the same policy object the ThreadPool workers use,
 *      for the lane (priority plus aging), given the lanes with ready work;
 *   2. picks one ready element of that lane at random from the seed;
 *   3. resumes it until it suspends.
 * So the seed explores every order the real pool may produce within a lane,
 * while lane priority and the aging bound hold exactly as in the pool. (The cap
 * on concurrent Background+Idle work is trivially met with one dispatcher.)
 *
 * Virtual time. The simulator is also the IClock its tasks see (clock()). Time
 * stands still while anything is ready; when nothing is, it jumps to the
 * earliest timer and releases every timer due at that instant. A sleeper
 * resumed by its deadline therefore observes steadyNow() == its deadline.
 *
 * Determinism. Given the same seed and the same sequence of calls from the
 * test, every run produces the same dispatch sequence on every platform (own
 * PRNG, no addresses or wall-clock time in decisions). traceHash() fingerprints
 * it; with SimOptions::recordTrace the full trace is kept.
 *
 * Shutdown matches ThreadPool: stopToken() is requested, pending sleeps end
 * with `false`, and everything left runs to completion. The destructor shuts
 * down, so declare a SimExecutor after (destroy it before) the state its tasks use.
 *
 * Thread safety: none. Every member, and every enqueue/timer call made by tasks,
 * must happen on the thread that constructed it (checked).
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/lane_policy.hpp"
#include "frost/sched/scheduler.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace frost::sched {

struct SimOptions {
    std::uint64_t seed = 0;
    IClock::SteadyTime steadyStart{}; // Virtual steady time at construction.
    IClock::SystemTime systemStart{}; // Virtual wall time at construction; advances with steady time.
    bool recordTrace = false;         // Keep every SimTraceEvent (traceHash() is always maintained).
};

// One dispatch, as recorded in the trace.
struct SimTraceEvent {
    std::uint64_t dispatch; // Dispatch number, from 0.
    IClock::Duration time;  // Virtual steady time (since the clock's epoch) at the dispatch.
    std::uint64_t sequence; // Enqueue sequence number of the resumed element (from 0, deterministic per seed).
    Lane lane;              // The lane it was dispatched from.
    LaneMask ready;         // Lanes that had ready work when the choice was made.

    friend bool operator==(const SimTraceEvent&, const SimTraceEvent&) = default;
};

class SimExecutor final : public IScheduler, public IClock {
public:
    explicit SimExecutor(SimOptions options = {});
    // Shuts down (see the top of the file). Precondition: owner thread, not inside a dispatch.
    ~SimExecutor();
    SimExecutor(const SimExecutor&) = delete ("tasks hold references to their scheduler");
    SimExecutor& operator=(const SimExecutor&) = delete ("tasks hold references to their scheduler");

    // --- Driving (owner thread, never from inside a task) ---

    // Dispatches one ready element; if none is ready, advances virtual time to the
    // earliest timer and releases every timer due then. Returns false only when
    // nothing is ready and no timer is pending. O(log timers) plus the task's own work.
    bool step();
    // Steps until nothing is ready and no timer is pending; returns the dispatches made.
    std::uint64_t runUntilIdle();
    // Steps while work is ready or a timer is due at or before `deadline`, then sets
    // the clock to max(now, deadline). Returns the dispatches made.
    std::uint64_t runUntil(IClock::SteadyTime deadline);
    // runUntil(steadyNow() + duration).
    std::uint64_t runFor(IClock::Duration duration);
    // Drains as described at the top of the file. Idempotent.
    void shutdown();

    // --- Introspection ---

    [[nodiscard]] std::uint64_t seed() const noexcept { return m_seed; }
    [[nodiscard]] std::uint64_t dispatches() const noexcept { return m_dispatches; }
    // FNV-1a over (sequence, lane, time) of every dispatch so far.
    [[nodiscard]] std::uint64_t traceHash() const noexcept { return m_traceHash; }
    // Empty unless SimOptions::recordTrace.
    [[nodiscard]] const std::vector<SimTraceEvent>& trace() const noexcept { return m_trace; }
    [[nodiscard]] std::size_t readyCount() const noexcept;
    [[nodiscard]] std::size_t timerCount() const noexcept { return m_timers.size(); }

    // --- IClock: virtual time ---
    [[nodiscard]] SteadyTime steadyNow() const noexcept override { return m_now; }
    [[nodiscard]] SystemTime systemNow() const noexcept override { return m_systemStart + (m_now - m_steadyStart); }

    // --- IScheduler (owner thread only) ---
    void enqueue(std::coroutine_handle<> handle, Lane lane) noexcept override;
    [[nodiscard]] const IClock& clock() const noexcept override { return *this; }
    [[nodiscard]] std::optional<Lane> currentLane() const noexcept override { return m_currentLane; }
    [[nodiscard]] std::stop_token stopToken() const noexcept override { return m_stopSource.get_token(); }
    void addTimer(TimerRequest& request, const std::stop_token& token) noexcept override;
    void cancelTimer(TimerRequest& request) noexcept override;
    [[nodiscard]] bool callerDriven() const noexcept override { return true; }
    bool driveOne() override { return step(); }

private:
    struct ReadyElement {
        std::uint64_t sequence;
        void* address;
    };
    using TimerKey = std::pair<IClock::Duration::rep, std::uint64_t>; // (deadline ticks, registration id)

    [[nodiscard]] bool onOwnerThread() const noexcept { return std::this_thread::get_id() == m_owner; }
    [[nodiscard]] LaneMask readyMask() const noexcept;
    void dispatch(LaneMask ready);
    void releaseTimersUpTo(IClock::Duration::rep ticks, bool expired);

    std::thread::id m_owner;
    std::uint64_t m_seed;
    detail::SplitMix64 m_rng;
    LaneSelector m_selector;
    SteadyTime m_steadyStart;
    SystemTime m_systemStart;
    SteadyTime m_now;
    std::array<std::vector<ReadyElement>, kLaneCount> m_ready;
    std::map<TimerKey, TimerRequest*> m_timers;
    std::uint64_t m_nextSequence = 0;
    std::uint64_t m_nextTimerId = 0;
    std::uint64_t m_dispatches = 0;
    std::uint64_t m_traceHash;
    bool m_recordTrace;
    std::vector<SimTraceEvent> m_trace;
    std::optional<Lane> m_currentLane; // Set while a dispatch is running.
    bool m_stopping = false;
    std::stop_source m_stopSource;
};

} // namespace frost::sched
