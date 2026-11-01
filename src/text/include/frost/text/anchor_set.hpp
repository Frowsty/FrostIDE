// SPDX-License-Identifier: Apache-2.0
//
// frost::text::AnchorSet: an immutable, persistent set of anchors with stable ids, mapped through
// edits by value (ADR 0017, decisions 2 and 3). Document keeps its registry in one and publishes it
// with every DocumentSnapshot, so an anchor created at version v resolves in every later snapshot,
// from any thread, in O(log n).
/*
 * Representation (detail/anchor_tree.hpp): a Left-bias tree and a Right-bias tree of
 * {gap, label, id} entries in offset order, plus an id index of {id, label, bias}. resolve(id) is
 * an index seek and a label seek. transformed() splices each class tree per edit step; the index is
 * never touched by a transform, because within a class the mapping is monotone and labels stay in
 * order. Only creation and moves need new labels; when two neighbours leave no label free, a block
 * of labels is spread out by the simplified order-maintenance algorithm of Bender, Cole, Demaine,
 * Farach-Colton and Zito, "Two Simplified Algorithms for Maintaining Order in a List" (ESA 2002,
 * LNCS 2461), with T = 1.5 (anchor_set.cpp).
 *
 * Character boundaries need the text, so Document checks them, not AnchorSet. A consumer that
 * keeps its own AnchorSet (for example the display map) checks boundaries itself and transforms
 * the set with every ChangeSet::edits, after checking ChangeSet::baseVersion == set.version().
 *
 * Thread safety: an immutable value. Every const member is safe on any thread and lock-free; the
 * nodes are shared by reference counting as in ADR 0013, D7 (relaxed increments, acq_rel
 * decrements). Never assign one AnchorSet object while another thread reads that same object.
 * Allocation failure terminates (noexcept), as in the rest of frost::text.
 *
 * Complexity: n anchors, B = 8 (fan-out), m edits in a batch.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/detail/anchor_tree.hpp"
#include "frost/text/positions.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace frost::text {

// Stable identity of an anchor; 1, 2, ... in creation order, never reused within one lineage
// (values derived from one another by withAnchor). Two values derived from one older value can
// hand out the same id: Document derives only from its current set. Ids from different AnchorSets
// (the Document's registry, a consumer's own set) must never be mixed: they would silently resolve
// a different anchor.
struct AnchorId {
    std::uint64_t value = 0;

    friend constexpr auto operator<=>(AnchorId, AnchorId) noexcept = default;
};

// An anchor with its id, as resolveRange() and all() return them.
struct ResolvedAnchor {
    AnchorId id;
    ByteOffset offset;
    Bias bias = Bias::Left;

    friend constexpr bool operator==(const ResolvedAnchor&, const ResolvedAnchor&) noexcept = default;
};

class AnchorSet {
public:
    // At most this many anchors per set: (4/3)^64 exceeds it, so a relabel always succeeds.
    static constexpr std::size_t kMaxAnchors = std::size_t{1} << 24;

    // Relabel counters for perf reports and tests: a value field copied with the set, no shared
    // state.
    struct Stats {
        std::uint64_t relabels = 0;          // Label blocks spread out.
        std::uint64_t relabelledEntries = 0; // Entries that received a new label in those spreads.

        friend constexpr bool operator==(const Stats&, const Stats&) noexcept = default;
    };

    // The empty set at version 0 for a text of size 0.
    AnchorSet() noexcept = default;
    // The empty set for a text of `textSize` bytes at `version`.
    AnchorSet(std::uint64_t version, ByteOffset textSize) noexcept;

    // Copy is O(1): three relaxed increments (null trees cost nothing). Move is noexcept.
    AnchorSet(const AnchorSet&) noexcept = default;
    AnchorSet(AnchorSet&&) noexcept = default;
    AnchorSet& operator=(const AnchorSet&) noexcept = default;
    AnchorSet& operator=(AnchorSet&&) noexcept = default;
    ~AnchorSet() = default;

    // The text version the offsets refer to. O(1).
    [[nodiscard]] std::uint64_t version() const noexcept { return m_version; }
    // The size of the text the offsets refer to. O(1).
    [[nodiscard]] ByteOffset textSize() const noexcept { return m_textSize; }
    // The number of anchors. O(1).
    [[nodiscard]] std::size_t size() const noexcept { return m_index.summary().count; }
    [[nodiscard]] bool empty() const noexcept { return m_index.empty(); }
    // The relabel counters accumulated along this value's history. Not public API (perf reports
    // and tests).
    [[nodiscard]] const Stats& statsForTesting() const noexcept { return m_stats; }

    // The offset of anchor `id`. Errors: NotFound when the set has no such anchor. O(B log n).
    // Budget: B5.
    [[nodiscard]] Result<ByteOffset> resolve(AnchorId id) const noexcept;
    // The offset and bias of anchor `id`. Errors: NotFound. O(B log n).
    [[nodiscard]] Result<Anchor> anchor(AnchorId id) const noexcept;

    // The anchors with range.begin <= offset <= range.end (a closed range), sorted by (offset, Left
    // before Right, id). Precondition: begin <= end <= textSize(). O(B log n + k log k) for k
    // results. Budget: B5.
    [[nodiscard]] std::vector<ResolvedAnchor> resolveRange(ByteRange range) const noexcept;
    // Every anchor, in the same order. O(n log n) in the worst case (ties sorted by id).
    [[nodiscard]] std::vector<ResolvedAnchor> all() const noexcept;

    // Value updates: each returns a new set and leaves *this unchanged.

    // The set with a new anchor at `offset` and its id. It goes after every anchor of its class at
    // that offset. Precondition: offset <= textSize() and size() < kMaxAnchors (Document checks
    // both and returns errors). Amortised O(B log^2 n) with relabelling, typically O(B log n).
    [[nodiscard]] std::pair<AnchorSet, AnchorId> withAnchor(ByteOffset offset, Bias bias) const noexcept;
    // The set without anchor `id`. Errors: NotFound. O(B log n).
    [[nodiscard]] Result<AnchorSet> without(AnchorId id) const noexcept;
    /*
     * Moves existing anchors to new offsets, keeping their ids and biases (ResolvedAnchor::bias is
     * ignored); a move to the anchor's current offset is skipped. A moved anchor goes after every
     * anchor of its class at the new offset. Precondition: every id exists, every offset <=
     * textSize(), each id at most once. O(r B log n) amortised for the r real moves, or a bulk
     * rebuild of all three trees when r * ceil(log2(n + 2)) > n: O(n log n) (its index is sorted
     * by id once; see ADR 0017).
     */
    [[nodiscard]] AnchorSet withMoved(std::span<const ResolvedAnchor> moves) const noexcept;
    /*
     * Maps every anchor through `batch` (anchor.hpp's rule), sets version() to `newVersion` and
     * textSize() to the size after the batch. Precondition (FROST_PRE): the batch is valid for a
     * text of textSize() bytes and sorted like applyEdits. Per step O(B log n + k) by splices, k
     * the anchors collapsed into a replaced range (O(B log n) when they already share one offset),
     * or O(n + m) by one sweep when m * ceil(log2(n + 2)) > n. The two paths give identical sets.
     * Budget: B5.
     */
    [[nodiscard]] AnchorSet transformed(std::span<const RawEdit> batch, std::uint64_t newVersion) const noexcept;
    // The same with the sweep path forced on (forceSweep) or off. Not public API: the white-box
    // test that both paths agree.
    [[nodiscard]] AnchorSet transformedForTesting(std::span<const RawEdit> batch, std::uint64_t newVersion,
                                                  bool forceSweep) const noexcept;

    /*
     * Checks, returning ErrorCode::Internal naming the first violation:
     *   A1 the SumTree invariants S1-S4 on all three trees;
     *   A2 labels strictly increase along each class tree, within [1, 2^64 - 2];
     *   A3 ids strictly increase in the index and are all below the next id;
     *   A4 the index count equals the sum of the two class counts;
     *   A5 each index entry's (bias, label) finds the class entry with the same id;
     *   A6 every offset is <= textSize().
     * O(n B log n); safe on any thread. For tests.
     */
    [[nodiscard]] Result<void> checkInvariants() const noexcept;

    // Structural equality of the trees (same entries, labels and gaps), version and size. O(n).
    // For the white-box splice-versus-sweep test.
    [[nodiscard]] bool sameStructureForTesting(const AnchorSet& other) const noexcept;
    // The class trees and the id index, for white-box memory reports that count the nodes undo
    // history retains (DocumentPerf). Not public API. O(1).
    [[nodiscard]] const detail::AnchorTree& classTreeForTesting(Bias bias) const noexcept { return tree(bias); }
    [[nodiscard]] const detail::AnchorIndex& indexForTesting() const noexcept { return m_index; }

private:
    [[nodiscard]] const detail::AnchorTree& tree(Bias bias) const noexcept {
        return bias == Bias::Left ? m_left : m_right;
    }
    [[nodiscard]] detail::AnchorTree& tree(Bias bias) noexcept { return bias == Bias::Left ? m_left : m_right; }

    // Inserts entry `id` into the class tree of `bias` at `offset` (after every entry of that class
    // at the offset), relabelling when no label is free, and returns its label. The index is
    // updated for relabelled entries only.
    std::uint64_t insertEntry(Bias bias, std::uint64_t offset, std::uint64_t id) noexcept;
    // Spreads the labels of the smallest block around `lo` that is sparse enough, leaving a free
    // label right after `lo` (Bender et al.), and updates the index for the relabelled entries.
    void relabel(Bias bias, std::uint64_t lo) noexcept;
    // Sets the index labels of `relabelled` ((id, new label) pairs, any order).
    void updateIndexLabels(std::vector<std::pair<std::uint64_t, std::uint64_t>>& relabelled) noexcept;
    [[nodiscard]] AnchorSet transformImpl(std::span<const RawEdit> batch, std::uint64_t newVersion,
                                          int path) const noexcept;
    [[nodiscard]] AnchorSet movedBulk(std::span<const ResolvedAnchor> moves) const noexcept;

    detail::AnchorTree m_left;
    detail::AnchorTree m_right;
    detail::AnchorIndex m_index;
    std::uint64_t m_nextId = 1;
    std::uint64_t m_version = 0;
    ByteOffset m_textSize;
    Stats m_stats;
};

} // namespace frost::text
