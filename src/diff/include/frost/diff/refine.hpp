// SPDX-License-Identifier: Apache-2.0
//
// frost::diff::refineHunk: intra-line refinement of one line hunk by words or characters
// (ADR 0019, D8), for the compare view's highlights and for edits finer than whole lines.
/*
 * Tokens (bytes are never decoded beyond what the rules below need):
 *   Word: a maximal run of [A-Za-z0-9_] or bytes >= 0x80 (non-ASCII, invalid bytes included, counts
 *         as a word character); a maximal run of [ \t]; each line break ("\r\n", "\r", "\n") as
 *         one token; every other byte as one token.
 *   Char: one token per well-formed UTF-8 code point (Unicode table 3-7); each byte of an
 *         ill-formed sequence alone; "\r\n" as one token.
 * Grapheme clusters wait for utf8proc (m1/editor-model). TODO(M5): Granularity::Grapheme.
 *
 * Algorithm: tokenise both sides of the hunk, strip the common token prefix and suffix, run a
 * minimal token-level Myers diff on the middle, and slide each pure deletion or insertion run
 * down while the token after it equals its first token (and it does not touch the next change).
 * Guards keep the cost bounded: a side larger than maxScanBytes is not tokenised; a middle larger
 * than maxBytes, or a token diff that runs out of maxWork, is reported coarse.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/text/positions.hpp"

#include <cstdint>
#include <stop_token>
#include <vector>

namespace frost::diff {

enum class Granularity : std::uint8_t { Word, Char };

// One change inside a hunk: a's bytes [a) are replaced by b's bytes [b). Absolute ranges in
// a.bytes() and b.bytes(); one of them may be empty.
struct RangePair {
    text::ByteRange a;
    text::ByteRange b;

    friend constexpr bool operator==(const RangePair&, const RangePair&) noexcept = default;
};

// Limits of one refinement; maxBytes and maxScanBytes apply to each side separately.
struct RefineLimits {
    std::uint32_t maxBytes = 64 * 1024;             // Bytes of the middle left after prefix/suffix trimming.
    std::uint64_t maxScanBytes = 16 * 1024 * 1024;  // Bytes of a hunk side that are tokenised at all.
    std::uint64_t maxWork = std::uint64_t{1} << 20; // Interning probes, Myers steps and slide steps.
};

struct Refinement {
    std::vector<RangePair> changes; // Sorted and disjoint on both sides.
    bool coarse = false;            // A guard fired: `changes` is one pair covering the hunk or its middle.
};

/*
 * Refines `hunk` (a hunk of a diff of `a` and `b`; precondition: within bounds and a.split() ==
 * b.split()). A hunk with one empty side gives one pair covering it (not coarse). Otherwise:
 *   - if either side exceeds limits.maxScanBytes: coarse, one pair covering the whole hunk;
 *   - else the sides are tokenised and the common token prefix and suffix are stripped;
 *   - if what remains exceeds limits.maxBytes on either side, or the token diff runs out of
 *     limits.maxWork: coarse, one pair covering the remaining middle;
 *   - else the minimal token diff of the middle, with the slide rule of the header comment.
 * Applying the changes to a's hunk bytes yields b's hunk bytes in every case; equal hunk sides give
 * no changes. Errors: Cancelled (polled at entry and every 4096 work units).
 * O(T + T' * D), with T the hunk's bytes and T' the middle's tokens; the bytes are compared with
 * memcmp, and only those around the first and last difference are tokenised, so a middle that is
 * certain to exceed maxBytes costs O(T) memcmp plus the tokens at its two edges (ADR 0019, D8).
 * Pure and reentrant. Budget: B18. Budget: B21 (RefinePerf.EdgesOnly).
 */
[[nodiscard]] Result<Refinement> refineHunk(const LineSequence& a, const LineSequence& b, const Hunk& hunk,
                                            Granularity granularity, const RefineLimits& limits = {},
                                            std::stop_token stop = {}) noexcept;

} // namespace frost::diff
