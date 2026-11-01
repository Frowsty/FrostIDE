// SPDX-License-Identifier: Apache-2.0
//
// Myers' O(ND) difference algorithm in linear space (E. W. Myers, "An O(ND) Difference Algorithm and
// Its Variations", Algorithmica 1(2), 1986, section 4b: the middle snake, divide and conquer) over
// id sequences (ADR 0019, D4). The histogram's fallback, Algorithm::Myers, and the token diff of
// refinement.
#pragma once

#include "work_budget.hpp"

#include <cstdint>
#include <span>

namespace frost::diff::detail {

// A rectangle of the edit graph: A[a0, a1) against B[b0, b1).
struct Region {
    std::uint32_t a0 = 0;
    std::uint32_t a1 = 0;
    std::uint32_t b0 = 0;
    std::uint32_t b1 = 0;
};

// Marks A[a0, a1) and B[b0, b1) as changed (a whole replace).
void markReplace(const Region& region, std::span<std::uint8_t> changedA, std::span<std::uint8_t> changedB) noexcept;

/*
 * Marks a minimal edit script of A[region] against B[region] in the change flags: a flag set to 1
 * is a deleted line of A or an inserted line of B; the flags inside the region must be 0 on entry.
 * Each subproblem first strips its common prefix and suffix (which the paper's recursion allows),
 * then splits at the middle snake; subproblems sit on an explicit stack (no recursion).
 *
 * Work: one unit per diagonal visited and per matching snake step. Returns false when the budget
 * ran out: the subproblem being solved and every queued one are then marked as whole replaces, a
 * valid but coarser script. O((N + M) * D) time, O(N + M) space.
 */
[[nodiscard]] bool myersDiff(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b, Region region,
                             std::span<std::uint8_t> changedA, std::span<std::uint8_t> changedB,
                             WorkBudget& budget) noexcept;

} // namespace frost::diff::detail
