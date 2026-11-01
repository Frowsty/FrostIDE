// SPDX-License-Identifier: Apache-2.0
//
// Sliders::GitClassic: the compaction of changed-line groups that makes histogram output match git
// before 2.54 (ADR 0019, D5). Our own behavioural formulation, verified against git as a black box;
// see compaction.cpp for the rule.
#pragma once

#include "work_budget.hpp"

#include <cstdint>
#include <span>

namespace frost::diff::detail {

/*
 * One pass over side X: every group of changed lines in `changedX` slides to the lowest placement
 * whose end aligns with a change on the other side (`changedOther`, which the pass does not modify),
 * else to the lowest placement; groups absorb neighbours they touch. Each slide step and each
 * absorbed line costs one unit; returns false when the budget runs out, leaving a valid script.
 * O(|X| + |other| + slide steps).
 */
[[nodiscard]] bool slidePass(std::span<const std::uint32_t> x, std::span<std::uint8_t> changedX,
                             std::span<const std::uint8_t> changedOther, WorkBudget& budget) noexcept;

/*
 * GitClassic compaction: slidePass over A (against B's flags), then ONCE over B (against A's final
 * flags). Running the passes again is not a no-op and would break git parity (A = 011, B = 001).
 * Does nothing when the budget is already exhausted. Returns false when the budget ran out.
 */
[[nodiscard]] bool compactGitClassic(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b,
                                     std::span<std::uint8_t> changedA, std::span<std::uint8_t> changedB,
                                     WorkBudget& budget) noexcept;

} // namespace frost::diff::detail
