// SPDX-License-Identifier: Apache-2.0
//
// The side-by-side diff model (M5, ADR 0048) behind the diff viewer: the line blocks of two texts
// (frost::diff's histogram diff under git's line model, so they equal `git diff --histogram`'s
// hunks), the intra-line changes of each replaced block (refineHunk by words), the line mapping
// that keeps the two panes scrolled together, and block navigation (next/previous change).
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/diff/refine.hpp"
#include "frost/vcs/line_changes.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace frost::vcs {

enum class DiffSide : std::uint8_t { Left, Right };

// One changed block: the left's lines [hunk.a) became the right's lines [hunk.b).
struct DiffBlock {
    diff::Hunk hunk;
    // For a replaced block (both sides non-empty): the changed byte ranges inside it, absolute in the
    // left's and the right's bytes. Empty for pure insertions and deletions.
    std::vector<diff::RangePair> inner;
    bool coarse = false; // The refinement gave up (a huge block): `inner` covers the block's middle.

    // Added (no left lines), Deleted (no right lines) or Modified.
    [[nodiscard]] LineChangeKind kind() const noexcept;
    // The block's first line on `side`.
    [[nodiscard]] LineIndex begin(DiffSide side) const noexcept {
        return side == DiffSide::Left ? hunk.a.begin : hunk.b.begin;
    }
    [[nodiscard]] diff::LineRange lines(DiffSide side) const noexcept {
        return side == DiffSide::Left ? hunk.a : hunk.b;
    }
};

/*
 * The diff of two texts. Immutable after compute(); copies share the texts (LineSequence is O(1) to
 * copy). Thread-safe for reads.
 */
class SideBySideDiff {
public:
    SideBySideDiff() = default;

    /*
     * Diffs `left` (HEAD, the index) against `right` (the working copy) with LineSplit::Lf, git's
     * model, and refines every replaced block by words. Errors: those of LineSequence::fromBytes
     * and diff::diffLines (OutOfRange, Cancelled). O(diff + refinement).
     */
    [[nodiscard]] static Result<SideBySideDiff> compute(std::string left, std::string right,
                                                        const std::stop_token& stop = {});

    [[nodiscard]] const diff::LineSequence& left() const noexcept { return m_left; }
    [[nodiscard]] const diff::LineSequence& right() const noexcept { return m_right; }
    [[nodiscard]] const diff::LineSequence& text(DiffSide which) const noexcept {
        return which == DiffSide::Left ? m_left : m_right;
    }
    [[nodiscard]] std::span<const DiffBlock> blocks() const noexcept { return m_blocks; }
    // The hunks alone, as diff::Hunk values (what the tests compare with git's -U0 headers).
    [[nodiscard]] std::vector<diff::Hunk> hunks() const;

    /*
     * The line of the other side shown beside `line` of `from`: unchanged lines map to their twins,
     * lines inside a block to the other side's block (its first line plus the offset, clamped).
     * O(blocks).
     */
    [[nodiscard]] LineIndex mapLine(DiffSide from, LineIndex line) const noexcept;

    // The first block that starts after `line` on `side`, or nullopt. O(log blocks).
    [[nodiscard]] std::optional<std::size_t> blockAfter(DiffSide side, LineIndex line) const noexcept;
    // The last block that starts before `line` on `side`, or nullopt. O(log blocks).
    [[nodiscard]] std::optional<std::size_t> blockBefore(DiffSide side, LineIndex line) const noexcept;
    // The block that holds `line` on `side` (a pure insertion or deletion holds the line it sits
    // at on the empty side), or nullopt. O(blocks).
    [[nodiscard]] std::optional<std::size_t> blockAt(DiffSide side, LineIndex line) const noexcept;

private:
    diff::LineSequence m_left;
    diff::LineSequence m_right;
    std::vector<DiffBlock> m_blocks;
};

} // namespace frost::vcs
