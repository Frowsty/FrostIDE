// SPDX-License-Identifier: Apache-2.0
//
// frost::text::Selection and SelectionSet: multiple carets and selections as an immutable,
// normalised value whose endpoints are anchors with biases derived from the geometry (ADR 0017,
// decision 4). The editor model's commands are pure functions over these values.
/*
 * Normalisation (section 3.2 of the slice spec). Selections are sorted by (start, end, input
 * index); one sweep merges the current selection s with the next one t when t.start < s.end, or
 * when t.start == s.end and s or t is empty. The merged range is [s.start, max(s.end, t.end)); it
 * takes the direction of its first non-empty member in sweep order (a caret when every member is
 * empty), it is primary when any member was, and its origin is its first member in sweep order.
 * So a caret inside or at the edge of a selection is absorbed, touching non-empty selections stay
 * separate and identical carets merge.
 *
 * Endpoint biases: a caret's endpoints are both Right (typing at a caret pushes it along); a
 * non-empty selection's lower endpoint is Left and its upper endpoint Right (it grows when text is
 * inserted at its edges). transformed() maps the endpoints with those biases, keeps each
 * direction (a non-empty selection may become a caret) and normalises again.
 *
 * SelectionSets do not use AnchorSet registry anchors: carets change on every keystroke and the
 * commands are pure.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/detail/ref.hpp"
#include "frost/text/positions.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace frost::text {

// One selection: `anchor` is the fixed end, `head` the caret end. Empty (anchor == head) is a
// caret. A plain value.
struct Selection {
    ByteOffset anchor;
    ByteOffset head;

    // A caret at `at`.
    [[nodiscard]] static constexpr Selection caret(ByteOffset at) noexcept { return {at, at}; }
    // [min(anchor, head), max(anchor, head)).
    [[nodiscard]] constexpr ByteRange range() const noexcept {
        return ByteRange{std::min(anchor, head), std::max(anchor, head)};
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return anchor == head; }
    // True when the caret end is before the fixed end.
    [[nodiscard]] constexpr bool reversed() const noexcept { return head < anchor; }

    friend constexpr bool operator==(const Selection&, const Selection&) noexcept = default;
};

/*
 * An immutable, normalised, non-empty-or-empty set of selections with one primary selection.
 *
 * Lifetime: a reference-counted array (null when empty), so copies are O(1) (one relaxed increment)
 * and moves touch no counter; the special members are out of line (selection.cpp) because the
 * array type is private. Thread safety: an immutable value, safe on any thread like Snapshot
 * (ADR 0013, D7); never assign one object while another thread reads it. Allocation failure
 * terminates.
 */
class SelectionSet {
public:
    // No selections.
    SelectionSet() noexcept;
    SelectionSet(const SelectionSet& other) noexcept;
    SelectionSet(SelectionSet&& other) noexcept;
    SelectionSet& operator=(const SelectionSet& other) noexcept;
    SelectionSet& operator=(SelectionSet&& other) noexcept;
    ~SelectionSet();

    // One caret at `at`, primary.
    [[nodiscard]] static SelectionSet caret(ByteOffset at) noexcept;

    /*
     * Sorts and merges `selections` (header comment). The primary selection is the one that
     * absorbed selections[primary]. When `origins` is given it is cleared and receives, for each
     * result selection, the input index of its first member in sweep order: callers carry
     * per-selection data (goal columns, per-caret clipboard text) through merges this way.
     * Precondition: primary < selections.size() when it is non-empty. O(n log n). Budget: B5.
     */
    [[nodiscard]] static SelectionSet fromSelections(std::span<const Selection> selections, std::size_t primary,
                                                     std::vector<std::uint32_t>* origins = nullptr) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    // The selections, sorted by (start, end) and pairwise unmergeable. Valid while this value (or
    // a copy) lives.
    [[nodiscard]] std::span<const Selection> all() const noexcept;
    // The index of the primary selection in all(); 0 when empty.
    [[nodiscard]] std::size_t primaryIndex() const noexcept;
    // The largest endpoint (the last selection's end), 0 when empty. O(1).
    [[nodiscard]] ByteOffset maxOffset() const noexcept;

    /*
     * Maps every endpoint through `batch` (sorted like applyEdits) with its derived bias, keeps
     * each selection's direction, and normalises again; the primary follows its selection. With
     * `origins`, as for fromSelections, indexing all() of *this. O(n + m) to map; the re-sort is
     * O(n log n) in the worst case (a caret and a selection start inside one replaced range swap
     * order), typically a linear check. Budget: B5.
     */
    [[nodiscard]] SelectionSet transformed(std::span<const RawEdit> batch,
                                           std::vector<std::uint32_t>* origins = nullptr) const noexcept;

    // Equal selections and equal primary index. O(1) when both share one array, else O(n).
    friend bool operator==(const SelectionSet& a, const SelectionSet& b) noexcept;

    /*
     * Checks, returning ErrorCode::Internal naming the first violation:
     *   L1 sorted by (start, end);  L2 no adjacent pair is mergeable by the rule above;
     *   L3 primaryIndex() < size(), or 0 when empty.
     * O(n).
     */
    [[nodiscard]] Result<void> checkInvariants() const noexcept;

private:
    struct Data;
    explicit SelectionSet(detail::Ref<Data> data) noexcept;

    detail::Ref<Data> m_data;
};

} // namespace frost::text
