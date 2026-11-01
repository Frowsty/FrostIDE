// SPDX-License-Identifier: Apache-2.0
//
// Scheduler lanes (PLAN.md section 2, "Scheduler"): every unit of work runs in
// one of four priority lanes, highest first. Lanes are a dispatch priority, not
// an OS thread priority: a worker always prefers the highest lane with eligible
// work, subject to aging (lane_policy.hpp) and the low-lane cap (thread_pool.hpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace frost::sched {

enum class Lane : std::uint8_t {
    Interactive = 0, // Input handling and anything the user is actively waiting on (typing, completion popups).
    UserVisible = 1, // Work whose result appears on screen soon (highlighting, diagnostics for the active file).
    Background = 2,  // Indexing, scanning, cache upkeep.
    Idle = 3,        // Work that may wait arbitrarily long (prefetching, statistics).
};

inline constexpr std::size_t kLaneCount = 4;

// Every lane in priority order, highest first.
inline constexpr std::array<Lane, kLaneCount> kAllLanes = {Lane::Interactive, Lane::UserVisible, Lane::Background,
                                                           Lane::Idle};

// The lane's position in kAllLanes (0 = highest priority).
[[nodiscard]] constexpr std::size_t laneIndex(Lane lane) noexcept { return static_cast<std::size_t>(lane); }

// Background and Idle are the "low" lanes whose concurrent execution is capped.
[[nodiscard]] constexpr bool isLowLane(Lane lane) noexcept { return lane >= Lane::Background; }

// A set of lanes, one bit per lane (bit i = kAllLanes[i]).
using LaneMask = std::uint8_t;

[[nodiscard]] constexpr LaneMask laneBit(Lane lane) noexcept { return static_cast<LaneMask>(1U << laneIndex(lane)); }

inline constexpr LaneMask kLowLanes = static_cast<LaneMask>(laneBit(Lane::Background) | laneBit(Lane::Idle));

// "Interactive", "UserVisible", "Background" or "Idle"; "Lane(?)" only for a value outside the enum (e.g. a bad cast).
[[nodiscard]] std::string_view toString(Lane lane) noexcept;

} // namespace frost::sched
