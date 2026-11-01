// SPDX-License-Identifier: Apache-2.0
//
// GitClassic slider compaction (compaction.hpp; ADR 0019, D5).
/*
 * Our own formulation of the behaviour git < 2.54 shows for histogram output with
 * --no-indent-heuristic, established by comparing hunks with git as a black box (ADR 0019,
 * "Measurements"); no git source was read.
 *
 * A diff is held as change flags per side. The unchanged lines of the two sides pair up in order,
 * so the k-th unchanged line of X pairs with the k-th unchanged line of the other side O.
 * gap[k] (k = 0..U, U the number of unchanged lines) is the number of changed lines of O just
 * before its k-th unchanged line (gap[U] counts the trailing ones). A group of changed X lines
 * [s, e) whose end e is X's k-th unchanged line is "aligned" when gap[k] > 0: a change of O sits
 * at the same place. One pass over X, with k the number of unchanged X lines in [0, e):
 *
 *   for each group [s, e):
 *     repeat:
 *       size = e - s
 *       slide up while X[s-1] == X[e-1] (the line above equals the group's last line): the group
 *         moves up by one, k -= 1, and absorbs every changed line it now touches above (after every
 *         step);
 *       aligned = e if gap[k] > 0, else none;
 *       slide down while X[s] == X[e]: the group moves down by one, k += 1, absorbs every changed
 *         line it now touches below, and aligned = e if gap[k] > 0 (after every step);
 *     until the group absorbed nothing (e - s == size);
 *     if aligned is set, move back up to it (the last round absorbed nothing, so this only retraces);
 *     continue after the group.
 *
 * Net effect: the lowest placement that aligns with a change on the other side, else the lowest
 * placement. Every step moves one changed line to an equal line, so the flags always describe a
 * valid script. The passes run once each, A then B: pass A runs against B's raw flags, which pass B
 * then changes, so running pass A again can move groups (A = 011, B = 001: git prints
 * [(1,1,1,2), (2,3,3,3)], the single-pass result; a second round gives [(1,2,1,2)]). Re-running
 * pass B alone on the final flags is a no-op, which checkDiff's H4 asserts.
 *
 * TODO(M5): git >= 2.54's final clean-up phase for histogram output, and the indent heuristic.
 */

#include "compaction.hpp"

#include "frost/core/assert.hpp"

#include <vector>

namespace frost::diff::detail {
namespace {

constexpr std::size_t kNoAlignment = static_cast<std::size_t>(-1);

} // namespace

bool slidePass(std::span<const std::uint32_t> x, std::span<std::uint8_t> changedX,
               std::span<const std::uint8_t> changedOther, WorkBudget& budget) noexcept {
    FROST_PRE(x.size() == changedX.size());
    std::vector<std::uint32_t> gap;
    gap.reserve(changedOther.size() + 1);
    std::uint32_t run = 0;
    for (const std::uint8_t changed : changedOther) {
        if (changed != 0) {
            ++run;
        } else {
            gap.push_back(run);
            run = 0;
        }
    }
    gap.push_back(run);

    const std::size_t n = x.size();
    std::span<std::uint8_t> cx = changedX;
    std::size_t i = 0;
    std::size_t k = 0; // Unchanged X lines in [0, i), later in [0, e).
    while (i < n) {
        if (cx[i] == 0) {
            ++i;
            ++k;
            continue;
        }
        std::size_t s = i;
        std::size_t e = i;
        while (e < n && cx[e] != 0) {
            ++e;
        }
        std::size_t aligned = kNoAlignment;
        for (;;) {
            const std::size_t size = e - s;
            // 1. Slide up, absorbing after every step.
            while (s > 0 && x[s - 1] == x[e - 1]) {
                if (!budget.charge()) {
                    return false;
                }
                --s;
                --e;
                cx[s] = 1;
                cx[e] = 0;
                --k;
                while (s > 0 && cx[s - 1] != 0) {
                    if (!budget.charge()) {
                        return false;
                    }
                    --s;
                }
            }
            // 2. Alignment at the top placement.
            FROST_ASSERT(k < gap.size());
            aligned = gap[k] > 0 ? e : kNoAlignment;
            // 3. Slide down, absorbing and re-checking the alignment after every step.
            while (e < n && x[s] == x[e]) {
                if (!budget.charge()) {
                    return false;
                }
                cx[s] = 0;
                cx[e] = 1;
                ++s;
                ++e;
                ++k;
                while (e < n && cx[e] != 0) {
                    if (!budget.charge()) {
                        return false;
                    }
                    ++e;
                }
                FROST_ASSERT(k < gap.size());
                if (gap[k] > 0) {
                    aligned = e;
                }
            }
            if (e - s == size) {
                break;
            }
        }
        // The last round absorbed nothing, so moving back up only retraces it.
        if (aligned != kNoAlignment) {
            while (e > aligned) {
                if (!budget.charge()) {
                    return false;
                }
                --s;
                --e;
                cx[s] = 1;
                cx[e] = 0;
                --k;
            }
        }
        i = e;
    }
    return true;
}

bool compactGitClassic(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b,
                       std::span<std::uint8_t> changedA, std::span<std::uint8_t> changedB,
                       WorkBudget& budget) noexcept {
    if (budget.exhausted()) {
        return false;
    }
    if (!slidePass(a, changedA, changedB, budget)) {
        return false;
    }
    return slidePass(b, changedB, changedA, budget);
}

} // namespace frost::diff::detail
