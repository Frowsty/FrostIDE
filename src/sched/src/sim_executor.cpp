// SPDX-License-Identifier: Apache-2.0
//
// SimExecutor: seeded dispatch and virtual time (see frost/sched/sim_executor.hpp).

#include "frost/sched/sim_executor.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>

namespace frost::sched {
namespace {

// FNV-1a, 64-bit (Fowler, Noll, Vo; public domain): offset basis and prime.
constexpr std::uint64_t kFnvOffset = 0xCBF2'9CE4'8422'2325ULL;
constexpr std::uint64_t kFnvPrime = 0x0000'0100'0000'01B3ULL;

constexpr std::uint64_t fnvMix(std::uint64_t hash, std::uint64_t value) noexcept {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xFFU;
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace

SimExecutor::SimExecutor(SimOptions options)
    : m_owner(std::this_thread::get_id()), m_seed(options.seed), m_rng(options.seed),
      m_steadyStart(options.steadyStart), m_systemStart(options.systemStart), m_now(options.steadyStart),
      m_traceHash(kFnvOffset), m_recordTrace(options.recordTrace) {}

SimExecutor::~SimExecutor() { shutdown(); }

std::size_t SimExecutor::readyCount() const noexcept {
    std::size_t count = 0;
    for (const std::vector<ReadyElement>& lane : m_ready) {
        count += lane.size();
    }
    return count;
}

LaneMask SimExecutor::readyMask() const noexcept {
    LaneMask mask = 0;
    for (const Lane lane : kAllLanes) {
        if (!m_ready[laneIndex(lane)].empty()) {
            mask = static_cast<LaneMask>(mask | laneBit(lane));
        }
    }
    return mask;
}

bool SimExecutor::step() {
    FROST_PRE(onOwnerThread());
    FROST_PRE(!m_currentLane.has_value()); // Not from inside a task.
    const LaneMask ready = readyMask();
    if (ready != 0) {
        dispatch(ready);
        return true;
    }
    if (m_timers.empty()) {
        return false;
    }
    // Nothing runnable: jump to the earliest deadline and release everything due then.
    const IClock::Duration::rep next = m_timers.begin()->first.first;
    m_now = SteadyTime(IClock::Duration(next));
    releaseTimersUpTo(next, true);
    return true;
}

std::uint64_t SimExecutor::runUntilIdle() {
    const std::uint64_t before = m_dispatches;
    while (step()) {
    }
    return m_dispatches - before;
}

std::uint64_t SimExecutor::runUntil(IClock::SteadyTime deadline) {
    const std::uint64_t before = m_dispatches;
    for (;;) {
        if (readyMask() != 0 ||
            (!m_timers.empty() && m_timers.begin()->first.first <= deadline.time_since_epoch().count())) {
            step();
        } else {
            break;
        }
    }
    m_now = std::max(m_now, deadline);
    return m_dispatches - before;
}

std::uint64_t SimExecutor::runFor(IClock::Duration duration) { return runUntil(m_now + duration); }

void SimExecutor::shutdown() {
    FROST_PRE(onOwnerThread());
    FROST_PRE(!m_currentLane.has_value());
    if (!m_stopping) {
        m_stopping = true;
        m_stopSource.request_stop();
        // Pending sleeps end now, cut short; time does not move.
        if (!m_timers.empty()) {
            releaseTimersUpTo(m_timers.rbegin()->first.first, false);
        }
    }
    runUntilIdle();
}

void SimExecutor::dispatch(LaneMask ready) {
    const Lane lane = m_selector.choose(ready);
    std::vector<ReadyElement>& queue = m_ready[laneIndex(lane)];
    const std::size_t pick = static_cast<std::size_t>(m_rng.below(queue.size()));
    const ReadyElement element = queue[pick];
    queue[pick] = queue.back();
    queue.pop_back();
    m_selector.dispatched(lane, ready);

    const IClock::Duration time = m_now.time_since_epoch();
    m_traceHash = fnvMix(fnvMix(fnvMix(m_traceHash, element.sequence), laneIndex(lane)),
                         static_cast<std::uint64_t>(time.count()));
    if (m_recordTrace) {
        m_trace.push_back(SimTraceEvent{m_dispatches, time, element.sequence, lane, ready});
    }
    ++m_dispatches;

    m_currentLane = lane;
    std::coroutine_handle<>::from_address(element.address).resume();
    m_currentLane.reset();
}

void SimExecutor::enqueue(std::coroutine_handle<> handle, Lane lane) noexcept {
    FROST_PRE(onOwnerThread());
    FROST_PRE(static_cast<bool>(handle));
    m_ready[laneIndex(lane)].push_back(ReadyElement{m_nextSequence++, handle.address()});
}

void SimExecutor::addTimer(TimerRequest& request, const std::stop_token& token) noexcept {
    FROST_PRE(onOwnerThread());
    if (m_stopping || token.stop_requested()) {
        request.expired = false;
        enqueue(request.handle, request.lane);
        return;
    }
    request.id = ++m_nextTimerId;
    m_timers.emplace(TimerKey{request.deadline.time_since_epoch().count(), request.id}, &request);
}

void SimExecutor::cancelTimer(TimerRequest& request) noexcept {
    FROST_PRE(onOwnerThread());
    const auto it = m_timers.find(TimerKey{request.deadline.time_since_epoch().count(), request.id});
    if (it == m_timers.end() || it->second != &request) {
        return; // Not registered yet (addTimer() sees the stop) or already released.
    }
    m_timers.erase(it);
    request.expired = false;
    enqueue(request.handle, request.lane);
}

void SimExecutor::releaseTimersUpTo(IClock::Duration::rep ticks, bool expired) {
    // In (deadline, registration) order, so the release order itself is deterministic.
    while (!m_timers.empty() && m_timers.begin()->first.first <= ticks) {
        TimerRequest& request = *m_timers.begin()->second;
        m_timers.erase(m_timers.begin());
        request.expired = expired;
        enqueue(request.handle, request.lane);
    }
}

} // namespace frost::sched
