// SPDX-License-Identifier: Apache-2.0
//
// Linear-space Myers diff (myers.hpp), written from E. W. Myers, "An O(ND) Difference Algorithm and
// Its Variations", Algorithmica 1(2), 1986, section 4b.
/*
 * The middle snake. With N = |A|, M = |B| and delta = N - M, a forward search from (0, 0) and a
 * reverse search from (N, M) advance one edit at a time. V[k] holds the furthest x reached on
 * diagonal k = x - y (the reverse search works on the reversed sequences, whose diagonal k' maps
 * to the forward diagonal delta - k'). When delta is odd, the paths can first overlap during a
 * forward step D (total 2D - 1 edits); when it is even, during a reverse step D (total 2D). The
 * snake found there lies on a minimal path, and the two rectangles before and after it are solved
 * the same way.
 *
 * Off-grid moves: the paper's recurrence takes x from the better of the two neighbouring
 * diagonals. A move from a frontier point on the grid's edge (a down move from y == M, a right move
 * from x == N) leaves the grid; such a candidate is dropped, and a diagonal with no candidate left
 * is marked unreachable (-1). This loses nothing: a path that would need the dropped move is
 * strictly longer than finishing along the edge from the frontier point, so no minimal path uses
 * it (ADR 0019, D4). Tests compare the edit count with an O(NM) LCS on 20k random inputs.
 *
 * Tie-break: for equal-length scripts the result is decided by the order of the diagonal scan
 * (k = -D upwards) and by "furthest x" (deletions before insertions). git's Myers variant differs,
 * so dense fallback regions may differ from git in hunks (not in size); see ADR 0019, D4.
 */

#include "myers.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <vector>

namespace frost::diff::detail {
namespace {

struct Snake {
    std::int64_t x0 = 0; // Start of the snake (after the last edit), local coordinates.
    std::int64_t y0 = 0;
    std::int64_t x1 = 0; // End of the snake.
    std::int64_t y1 = 0;
};

enum class SnakeResult : std::uint8_t { Found, Exhausted, NotFound };

/*
 * Finds the middle snake of the region (both sides non-empty). forward and backward have at least
 * 2 * ((N + M + 1) / 2) + 3 entries; their contents on entry do not matter (every entry read is
 * written first in this call).
 */
SnakeResult middleSnake(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b, const Region& r,
                        std::vector<std::int64_t>& forward, std::vector<std::int64_t>& backward, WorkBudget& budget,
                        Snake& out) noexcept {
    const std::int64_t n = r.a1 - r.a0;
    const std::int64_t m = r.b1 - r.b0;
    const std::int64_t delta = n - m;
    const bool odd = (delta & 1) != 0;
    const std::int64_t maxD = (n + m + 1) / 2;
    const std::int64_t offset = maxD + 1;
    const auto fv = [&](std::int64_t k) -> std::int64_t& { return forward[static_cast<std::size_t>(k + offset)]; };
    const auto bv = [&](std::int64_t k) -> std::int64_t& { return backward[static_cast<std::size_t>(k + offset)]; };
    const std::uint32_t* const pa = a.data() + r.a0;
    const std::uint32_t* const pb = b.data() + r.b0;
    // Reverse coordinates: a'[x] = A[n - 1 - x], b'[y] = B[m - 1 - y].
    const std::uint32_t* const ra = a.data() + r.a1 - 1;
    const std::uint32_t* const rb = b.data() + r.b1 - 1;

    for (std::int64_t d = 0; d <= maxD; ++d) {
        // Forward step d. The neighbours +-(d + 1) were not computed at step d - 1.
        fv(-d - 1) = -1;
        fv(d + 1) = -1;
        for (std::int64_t k = -d; k <= d; k += 2) {
            if (!budget.charge()) {
                return SnakeResult::Exhausted;
            }
            if (k < -m || k > n) {
                fv(k) = -1;
                continue;
            }
            std::int64_t x = -1;
            if (d == 0) {
                x = 0;
            } else {
                const std::int64_t down = fv(k + 1);  // From diagonal k + 1: y grows by one.
                const std::int64_t right = fv(k - 1); // From diagonal k - 1: x grows by one.
                if (down >= 0 && down - k <= m) {
                    x = down;
                }
                if (right >= 0 && right + 1 <= n) {
                    x = std::max(x, right + 1);
                }
            }
            if (x < 0) {
                fv(k) = -1;
                continue;
            }
            std::int64_t y = x - k;
            const std::int64_t x0 = x;
            const std::int64_t y0 = y;
            while (x < n && y < m && pa[x] == pb[y]) {
                if (!budget.charge()) {
                    return SnakeResult::Exhausted;
                }
                ++x;
                ++y;
            }
            fv(k) = x;
            if (odd && d >= 1) {
                const std::int64_t kr = delta - k;
                if (kr >= -(d - 1) && kr <= d - 1) {
                    const std::int64_t xr = bv(kr);
                    if (xr >= 0 && x + xr >= n) {
                        out = Snake{x0, y0, x, y};
                        return SnakeResult::Found;
                    }
                }
            }
        }
        // Reverse step d.
        bv(-d - 1) = -1;
        bv(d + 1) = -1;
        for (std::int64_t k = -d; k <= d; k += 2) {
            if (!budget.charge()) {
                return SnakeResult::Exhausted;
            }
            if (k < -m || k > n) {
                bv(k) = -1;
                continue;
            }
            std::int64_t x = -1;
            if (d == 0) {
                x = 0;
            } else {
                const std::int64_t down = bv(k + 1);
                const std::int64_t right = bv(k - 1);
                if (down >= 0 && down - k <= m) {
                    x = down;
                }
                if (right >= 0 && right + 1 <= n) {
                    x = std::max(x, right + 1);
                }
            }
            if (x < 0) {
                bv(k) = -1;
                continue;
            }
            std::int64_t y = x - k;
            const std::int64_t x0 = x;
            const std::int64_t y0 = y;
            while (x < n && y < m && *(ra - x) == *(rb - y)) {
                if (!budget.charge()) {
                    return SnakeResult::Exhausted;
                }
                ++x;
                ++y;
            }
            bv(k) = x;
            if (!odd) {
                const std::int64_t kf = delta - k;
                if (kf >= -d && kf <= d) {
                    const std::int64_t xf = fv(kf);
                    if (xf >= 0 && xf + x >= n) {
                        // The reverse snake in forward coordinates.
                        out = Snake{n - x, m - y, n - x0, m - y0};
                        return SnakeResult::Found;
                    }
                }
            }
        }
    }
    return SnakeResult::NotFound;
}

} // namespace

void markReplace(const Region& region, std::span<std::uint8_t> changedA, std::span<std::uint8_t> changedB) noexcept {
    std::fill(changedA.begin() + region.a0, changedA.begin() + region.a1, std::uint8_t{1});
    std::fill(changedB.begin() + region.b0, changedB.begin() + region.b1, std::uint8_t{1});
}

bool myersDiff(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b, Region region,
               std::span<std::uint8_t> changedA, std::span<std::uint8_t> changedB, WorkBudget& budget) noexcept {
    FROST_PRE(region.a0 <= region.a1 && region.a1 <= a.size() && region.b0 <= region.b1 && region.b1 <= b.size());
    std::vector<Region> stack;
    stack.push_back(region);
    std::vector<std::int64_t> forward;
    std::vector<std::int64_t> backward;
    const auto failAll = [&](const Region& current) {
        markReplace(current, changedA, changedB);
        for (const Region& queued : stack) {
            markReplace(queued, changedA, changedB);
        }
        stack.clear();
    };
    while (!stack.empty()) {
        Region r = stack.back();
        stack.pop_back();
        // Common prefix and suffix match (they stay unchanged).
        while (r.a0 < r.a1 && r.b0 < r.b1 && a[r.a0] == b[r.b0]) {
            if (!budget.charge()) {
                failAll(r);
                return false;
            }
            ++r.a0;
            ++r.b0;
        }
        while (r.a0 < r.a1 && r.b0 < r.b1 && a[r.a1 - 1] == b[r.b1 - 1]) {
            if (!budget.charge()) {
                failAll(r);
                return false;
            }
            --r.a1;
            --r.b1;
        }
        if (r.a0 == r.a1 || r.b0 == r.b1) {
            markReplace(r, changedA, changedB);
            continue;
        }
        const std::size_t need = 2 * ((std::size_t{r.a1 - r.a0} + (r.b1 - r.b0) + 1) / 2) + 3;
        if (forward.size() < need) {
            forward.resize(need);
            backward.resize(need);
        }
        Snake snake;
        const SnakeResult found = middleSnake(a, b, r, forward, backward, budget, snake);
        if (found == SnakeResult::Exhausted) {
            failAll(r);
            return false;
        }
        FROST_ASSERT(found == SnakeResult::Found);
        if (found != SnakeResult::Found) {
            markReplace(r, changedA, changedB); // Unreachable; keeps the script valid if it were not.
            continue;
        }
        const auto x0 = static_cast<std::uint32_t>(snake.x0);
        const auto y0 = static_cast<std::uint32_t>(snake.y0);
        const auto x1 = static_cast<std::uint32_t>(snake.x1);
        const auto y1 = static_cast<std::uint32_t>(snake.y1);
        FROST_ASSERT(x0 <= x1 && y0 <= y1 && x1 <= r.a1 - r.a0 && y1 <= r.b1 - r.b0);
        stack.push_back(Region{r.a0 + x1, r.a1, r.b0 + y1, r.b1});
        stack.push_back(Region{r.a0, r.a0 + x0, r.b0, r.b0 + y0});
    }
    return true;
}

} // namespace frost::diff::detail
