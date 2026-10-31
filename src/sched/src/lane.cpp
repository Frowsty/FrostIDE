// SPDX-License-Identifier: Apache-2.0
//
// Lane names and the LaneSelector aging policy (see frost/sched/lane.hpp and lane_policy.hpp).

#include "frost/sched/lane.hpp"

#include "frost/core/assert.hpp"
#include "frost/sched/lane_policy.hpp"

#include <algorithm>

namespace frost::sched {

std::string_view toString(Lane lane) noexcept {
    switch (lane) {
    case Lane::Interactive: return "Interactive";
    case Lane::UserVisible: return "UserVisible";
    case Lane::Background: return "Background";
    case Lane::Idle: return "Idle";
    }
    return "Lane(?)";
}

LaneSelector::Order LaneSelector::order(LaneMask eligible) const noexcept {
    Order result;
    if (eligible == 0) {
        return result;
    }
    const Lane preferred = choose(eligible);
    result.lanes[result.count++] = preferred;
    for (const Lane lane : kAllLanes) {
        if (lane != preferred && (eligible & laneBit(lane)) != 0) {
            result.lanes[result.count++] = lane;
        }
    }
    return result;
}

Lane LaneSelector::choose(LaneMask eligible) const noexcept {
    FROST_PRE(eligible != 0);
    // The highest-priority aged lane wins...
    for (const Lane lane : kAllLanes) {
        if ((eligible & laneBit(lane)) != 0 && aged(lane)) {
            return lane;
        }
    }
    // ...otherwise the highest-priority eligible lane.
    for (const Lane lane : kAllLanes) {
        if ((eligible & laneBit(lane)) != 0) {
            return lane;
        }
    }
    return Lane::Idle; // Unreachable: eligible != 0.
}

void LaneSelector::dispatched(Lane lane, LaneMask eligible) noexcept {
    m_passes[laneIndex(lane)] = 0;
    for (std::size_t i = laneIndex(lane) + 1; i < kLaneCount; ++i) {
        if ((eligible & laneBit(kAllLanes[i])) != 0) {
            m_passes[i] = std::min(m_passes[i] + 1, kAgingPasses[i]);
        }
    }
}

} // namespace frost::sched
