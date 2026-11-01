// SPDX-License-Identifier: Apache-2.0
//
// AnchorSet's path choices (ADR 0017, decision 2 and "Path thresholds" under its measurements;
// private header, included by anchor_set.cpp and the white-box tests): when a transform sweeps
// every anchor instead of splicing each edit step, and when moves rebuild the whole set instead of
// reinserting each anchor.
/*
 * Cost model. A sweep or a bulk rebuild visits every one of the n anchors once (about 25 ns per
 * entry on REF-W1). A splice step or a per-move reinsertion costs O(B log n), but its constant is
 * large: a few splits and concats of 8..16-wide nodes, measured at about 16 (splice step) and 8
 * (reinsertion) sweep entries per tree level. So the per-step path wins exactly while
 *   steps * factor * ceil(log2(n + 2)) <= n,
 * with factor kSweepFactor for transforms and kBulkMoveFactor for moves. The factors are fitted to
 * the crossovers measured with forced paths at n = 1k .. 1M anchors (ADR 0017); the first version
 * of this slice used factor 1, which kept the slow per-step paths for up to 8x (moves) and 15x
 * (transforms) more steps than pays off. Both paths always give equal results (the forced-path
 * tests), so the factors affect time only.
 */
#pragma once

#include <bit>
#include <cstdint>

namespace frost::text::detail {

inline constexpr std::uint64_t kSweepFactor = 16;
inline constexpr std::uint64_t kBulkMoveFactor = 8;

// ceil(log2(x)) for x >= 1.
[[nodiscard]] constexpr std::uint64_t ceilLog2(std::uint64_t x) noexcept {
    return static_cast<std::uint64_t>(std::bit_width(x - 1));
}

// True when a batch of `steps` non-no-op edit steps over `anchors` anchors should be swept.
[[nodiscard]] constexpr bool preferSweep(std::uint64_t steps, std::uint64_t anchors) noexcept {
    return steps * kSweepFactor * ceilLog2(anchors + 2) > anchors;
}

// True when `moves` moves among `anchors` anchors should rebuild the set in bulk.
[[nodiscard]] constexpr bool preferBulkMove(std::uint64_t moves, std::uint64_t anchors) noexcept {
    return moves * kBulkMoveFactor * ceilLog2(anchors + 2) > anchors;
}

} // namespace frost::text::detail
