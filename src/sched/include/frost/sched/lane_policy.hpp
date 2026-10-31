// SPDX-License-Identifier: Apache-2.0
//
// LaneSelector: the lane-priority and aging policy shared by ThreadPool (one
// selector per worker) and SimExecutor (one selector), so the deterministic
// simulator enforces exactly the guarantees the real pool gives.
#pragma once

#include "frost/sched/lane.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace frost::sched {

/*
 * Policy. A dispatcher (a ThreadPool worker, or the SimExecutor loop) asks the
 * selector which lane to take its next unit of work from, passing the set of
 * lanes that currently have eligible work. Normally the highest-priority
 * eligible lane wins. To keep lower lanes from starving, each lane L below
 * Interactive has a pass counter: every dispatch from a lane of higher priority
 * than L while L had eligible work "passes over" L and increments it. Once the
 * counter reaches kAgingPasses[L] the lane is aged, and the highest-priority aged
 * lane is dispatched next, ahead of non-aged higher lanes. Dispatching a lane
 * resets its counter.
 *
 * Guarantee (checked in lane_policy_test.cpp for every set of saturated lanes and
 * under random eligibility, and on every SimExecutor stress trace by an
 * independent oracle): while lane L has eligible
 * work, a dispatcher passes over it at most maxWait(L) = kAgingPasses[L] +
 * kLaneCount - 1 consecutive times. Even Interactive can be passed over 3 times
 * in a row, when the three lower lanes come due together. Pass counts are per
 * dispatcher; in the pool each worker ages independently.
 *
 * Within a lane the selector says nothing about order; the pool's queues and the
 * simulator's seed decide that.
 */
class LaneSelector {
public:
    // Passes, in dispatches, after which a lane with waiting work is served ahead
    // of higher lanes. Interactive is never passed over, so its entry is unused (0).
    // UserVisible: 4 dispatches; Background: 16; Idle: 64. Lower lanes wait longer.
    // With every lane saturated a dispatcher's turns split about 74% / 19% / 6% / 1.5%;
    // with only Interactive and UserVisible, 80% / 20%.
    static constexpr std::array<std::uint32_t, kLaneCount> kAgingPasses = {0, 4, 16, 64};

    // The bound from the guarantee above: consecutive dispatches that may pass over `lane`.
    [[nodiscard]] static constexpr std::uint32_t maxWait(Lane lane) noexcept {
        return kAgingPasses[laneIndex(lane)] + static_cast<std::uint32_t>(kLaneCount) - 1;
    }

    // Lanes to try, in order: the preferred lane first, then every other eligible
    // lane by priority (fallbacks for when the preferred queue turns out empty).
    struct Order {
        std::array<Lane, kLaneCount> lanes{};
        std::size_t count = 0;
    };

    // The dispatch order for `eligible` (empty Order if eligible == 0). O(kLaneCount); no side effects.
    [[nodiscard]] Order order(LaneMask eligible) const noexcept;

    // The preferred lane; precondition: eligible != 0.
    [[nodiscard]] Lane choose(LaneMask eligible) const noexcept;

    // Records that `lane` was dispatched while `eligible` lanes had work: resets
    // the lane's counter and passes over every lower eligible lane. O(kLaneCount).
    void dispatched(Lane lane, LaneMask eligible) noexcept;

    // The current pass count of `lane` (for tests and statistics).
    [[nodiscard]] std::uint32_t passes(Lane lane) const noexcept { return m_passes[laneIndex(lane)]; }

    // True once `lane` has been passed over kAgingPasses times.
    [[nodiscard]] bool aged(Lane lane) const noexcept {
        return lane != Lane::Interactive && m_passes[laneIndex(lane)] >= kAgingPasses[laneIndex(lane)];
    }

private:
    // Saturates at the lane's threshold, so it never overflows. Not thread-safe:
    // a selector belongs to one dispatcher.
    std::array<std::uint32_t, kLaneCount> m_passes{};
};

} // namespace frost::sched
