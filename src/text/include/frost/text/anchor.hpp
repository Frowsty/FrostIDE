// SPDX-License-Identifier: Apache-2.0
//
// Anchors as values (ADR 0017, decision 1): a byte offset with a bias, the raw edits that move
// anchors, and the mapping rule through one edit or a batch. AnchorSet (anchor_set.hpp) keeps
// anchors by identity across snapshots; SelectionSet (selection.hpp) maps its endpoints with the
// same rule.
/*
 * The rule ("attachment" semantics). An edit replaces [a, b) of the text before it by n bytes. An
 * offset x maps to
 *   x < a:              x (unchanged);
 *   x > b:              x - (b - a) + n (shifted);
 *   a <= x <= b:        a for Bias::Left, a + n for Bias::Right (the closed range collapses).
 * A Left anchor stays after the character before it and a Right anchor before the character after
 * it; when that character is deleted, the anchor goes to the deletion point. Text inserted at an
 * anchor therefore goes after a Left anchor and before a Right one.
 *
 * Anchors are mapped through the requested edits (RawEdit), never through the D8-normalised
 * TextChanges (ADR 0013): D8 widens a change by one byte at a CR, which would move an anchor that
 * sits before the CR past the inserted text.
 *
 * A batch (sorted as TextBuffer::applyEdits requires: edits[i-1].range.end <= edits[i].range.begin)
 * means sequential application from the last edit to the first, each edit in the coordinates of
 * the text before the batch. No-op edits (an empty range and no bytes) map every offset to itself.
 */
#pragma once

#include "frost/core/strong_type.hpp"
#include "frost/text/positions.hpp"

#include <cstdint>
#include <span>

namespace frost::text {

// Which neighbour an anchor sticks to when text is inserted at it or deleted around it.
enum class Bias : std::uint8_t { Left, Right };

// The value form of an anchor: an offset and its bias (SelectionSet endpoints, AnchorSet entries).
// A plain value; trivially copyable and safe to share between threads.
struct Anchor {
    ByteOffset offset;
    Bias bias = Bias::Left;

    friend constexpr bool operator==(const Anchor&, const Anchor&) noexcept = default;
};

// One requested replacement, before D8 widening: `range` of the text before its batch became
// `newBytes` bytes. Document reports these in ChangeSet::edits; consumers that keep their own
// anchors (the display map, DiagnosticsStore) map them with mapOffset or AnchorSet::transformed.
struct RawEdit {
    ByteRange range;
    std::uint64_t newBytes = 0;

    // True for an empty range replaced by no bytes, which maps every offset to itself.
    [[nodiscard]] constexpr bool isNoOp() const noexcept { return range.empty() && newBytes == 0; }

    friend constexpr bool operator==(const RawEdit&, const RawEdit&) noexcept = default;
};

// The rule above for one edit. Precondition: edit.range.begin <= edit.range.end. O(1).
[[nodiscard]] constexpr ByteOffset mapOffset(ByteOffset x, Bias bias, const RawEdit& edit) noexcept {
    const ByteOffset a = edit.range.begin;
    const ByteOffset b = edit.range.end;
    if (x < a) {
        return x;
    }
    if (x > b) {
        return x - (b - a) + edit.newBytes;
    }
    return bias == Bias::Left ? a : a + edit.newBytes;
}

/*
 * The rule through a batch (sorted like applyEdits; no-ops allowed and ignored), equal to applying
 * the edits one by one from the last to the first. Closed form, with D_i the sum of
 * (newBytes - |range|) over the edits before edit i:
 *   - when no edit's closed range [a_i, b_i] contains x: x + D_i for the first edit i with
 *     b_i >= x (x + D_m beyond the last edit);
 *   - otherwise let F and L be the first and last edits whose closed ranges contain x (they are
 *     contiguous: the ones after F are inserts at x and at most one range starting at x).
 *     Right maps to a_L + n_L + D_L, the end of L's replacement. Left maps to a_K + D_K, where K
 *     is the first edit of the run of touching edits ending at F (b_{i-1} == a_i for K < i <= F):
 *     sequential application collapses a Left anchor through every touching edit before F.
 * Precondition: the batch is sorted and every range has begin <= end (FROST_PRE). O(m), no
 * allocation. Budget: B5.
 */
[[nodiscard]] ByteOffset mapOffset(ByteOffset x, Bias bias, std::span<const RawEdit> batch) noexcept;

} // namespace frost::text
