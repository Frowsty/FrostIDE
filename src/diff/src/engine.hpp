// SPDX-License-Identifier: Apache-2.0
//
// The diff engine over interned id sequences, shared by diffLines, merge3 and checkDiff
// (implemented in line_diff.cpp): the configured algorithm plus the slider pass into change flags,
// and hunks read off the flags; and refineHunk's scan switch (refine.cpp). Private to frost::diff
// (white-box tests include it).
#pragma once

#include "frost/diff/line_diff.hpp"
#include "frost/diff/refine.hpp"

#include "interner.hpp"
#include "work_budget.hpp"

#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace frost::diff::detail {

// A diff as change flags: changedA[i] != 0 when A's line i is deleted, changedB[j] != 0 when B's
// line j is inserted. The unchanged lines of both sides pair up in order.
struct FlagDiff {
    std::vector<std::uint8_t> changedA;
    std::vector<std::uint8_t> changedB;
    bool degraded = false;
    std::uint32_t fallbacks = 0;
};

/*
 * Diffs id sequence a against b (ids < idCount) with `algorithm` and `sliders` under `budget`.
 * Equal sequences give no changes without charging; an empty side gives a whole replace. When the
 * budget runs out the result is valid and degraded (see line_diff.hpp). O(as diffLines).
 */
[[nodiscard]] FlagDiff diffIds(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b,
                               std::uint32_t idCount, Algorithm algorithm, Sliders sliders,
                               WorkBudget& budget) noexcept;

// The maximal hunks of a flag diff: runs read off both flag arrays in parallel. Precondition: the
// two sides have the same number of unchanged lines. O(N + M).
[[nodiscard]] std::vector<Hunk> hunksFromFlags(std::span<const std::uint8_t> changedA,
                                               std::span<const std::uint8_t> changedB) noexcept;

// diffLines with a choice of line hashes; LineHashes::ConstantForTests models a flood of colliding
// lines (Limits.InternerFlooding). diffLines calls it with LineHashes::Fnv.
[[nodiscard]] Result<LineDiff> diffLinesWith(const LineSequence& a, const LineSequence& b, const DiffOptions& options,
                                             std::stop_token stop, LineHashes hashes) noexcept;

// How refineHunk finds the tokens to trim: Window tokenises only the bytes around the first and
// last difference (refineHunk's way); FullScan tokenises both hunk sides whole, the plain
// definition, kept as the oracle of Refine.WindowEqualsFullScan.
enum class RefineScan : std::uint8_t { Window, FullScan };

// refineHunk (refine.hpp) with a choice of scan; both give identical results. Implemented in
// refine.cpp.
[[nodiscard]] Result<Refinement> refineHunkWith(const LineSequence& a, const LineSequence& b, const Hunk& hunk,
                                                Granularity granularity, const RefineLimits& limits,
                                                std::stop_token stop, RefineScan scan) noexcept;

} // namespace frost::diff::detail
