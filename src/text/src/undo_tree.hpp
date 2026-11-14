// SPDX-License-Identifier: Apache-2.0
//
// The undo tree behind frost::text::Document (private header; ADR 0017, decisions 5-11): nodes,
// their transactions as canonical regions with piece slices, region composition, the history cap
// and the group and composition state. Document is the only writer; white-box tests read it
// through Document::undoTreeForTesting().
/*
 * Regions (section 3.3 of the slice spec). A transaction from X (the parent's text) to Y (the
 * node's text) is a sorted list of regions {before range in X, after range in Y, removed = X's
 * bytes of before, inserted = Y's bytes of after}. They are the gaps of the X-to-Y alignment of
 * kept bytes (bytes of X that no edit of the transaction replaced, even with identical content):
 * between consecutive kept bytes, and before the first and after the last, a region exists exactly
 * when the X gap or the Y gap is non-empty. So regions are canonical: r_i.before.end <
 * r_{i+1}.before.begin (at least one kept byte between them, likewise in Y),
 * r_i.after.begin - r_i.before.begin is the sum of the earlier regions' length deltas, and no
 * region has both ranges empty. The slices are PieceTrees of existing pieces, so a transaction
 * retains no Snapshot and copies no text. A removed slice is always cut from X's own tree in one
 * piece (the compose base, below), so it holds X's pieces over `before`, at most two of them cut:
 * a run of k coalesced backspaces keeps a slice of about k / maxPieceBytes + 2 pieces, not k
 * one-byte pieces, and undo puts those pieces back into the live tree.
 *
 * The compose base. While the current node accepts edits (its transaction is open, or it is the
 * open group's node), UndoTree keeps X's piece tree beside it; it retains only the nodes X no
 * longer shares with the live text and is dropped when the node is sealed.
 *
 * Nodes live in an unordered_map keyed by id. The map is never iterated (traversals walk from the
 * root through children lists), so runs stay deterministic.
 *
 * Thread safety: the writer's (Document's) thread only.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/anchor_set.hpp"
#include "frost/text/detail/piece.hpp"
#include "frost/text/document.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/selection.hpp"
#include "frost/text/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace frost::text::detail {

inline constexpr std::uint64_t kNoNode = ~std::uint64_t{0};

// One region of a transaction (header comment).
struct Region {
    ByteRange before;   // In X, the text before the transaction.
    ByteRange after;    // In Y, the text after it.
    PieceTree removed;  // X's bytes of `before`.
    PieceTree inserted; // Y's bytes of `after`.
};

/*
 * Composes the raw batch `batch` (coordinates of Y, ascending, no no-ops) into `regions` (X -> Y),
 * giving the regions X -> Z, where Z is the text after the batch. xTree is X's piece tree (Y's when
 * `regions` is empty, since then X == Y) and zTree Z's. Regions (their after ranges) and edits are
 * merged in begin order as closed intervals; each maximal run of touching items becomes one region,
 * whose removed slice is cut from X over its before range and whose inserted slice is cut from Z
 * over its after range; a run that nets to no change (both ranges empty) is dropped, and a run that
 * is a single region is kept with its after range shifted. O(p + m) to merge plus O(B log n + P)
 * per slice cut (two per merged run).
 */
[[nodiscard]] std::vector<Region> composeRegions(std::span<const Region> regions, std::span<const RawEdit> batch,
                                                 const PieceTree& xTree, const PieceTree& zTree) noexcept;

// U4 for one node: canonical regions within texts of xSize and ySize bytes, slice sizes equal to
// their ranges. ErrorCode::Internal naming the violation. O(regions).
[[nodiscard]] Result<void> checkRegions(std::span<const Region> regions, std::uint64_t xSize,
                                        std::uint64_t ySize) noexcept;

// One text state and the transaction into it.
struct UndoNode {
    TransactionId externalTransaction; // Global boundary/tombstone, never locally replayed.
    std::uint64_t id = 0;
    std::uint64_t parent = kNoNode;
    std::vector<std::uint64_t> children; // In creation order.
    std::uint64_t redoChild = kNoNode;   // None exactly when there are no children.
    std::uint64_t sequence = 0;          // Creation order.
    std::uint64_t stateId = 0;           // Decision 14.
    // The transaction (none for the root):
    EditKind kind = EditKind::Other;
    std::vector<Region> regions;
    SelectionSet selectionsBefore;
    SelectionSet selectionsAfter;
    AnchorSet anchorsBefore;      // The registry at the first edit (a commit's: the base set); the
                                  // pre-redo set after each redo.
    AnchorSet anchorsAfter;       // The pre-undo set at each undo.
    bool hasAnchorsAfter = false; // Set by the first undo.
    IClock::SteadyTime lastEdit{};
    bool open = false; // Accepts coalesced edits.
};

// An IME composition in progress.
struct Composition {
    ByteOffset start;
    std::uint64_t length = 0;
    Snapshot base;         // The text at beginComposition.
    AnchorSet baseAnchors; // The registry at beginComposition.
};

// The tree plus the state Document keeps beside it. Plain data with helpers; Document maintains
// the invariants (Document::checkInvariants checks them).
struct UndoTree {
    using NodeMap = std::unordered_map<std::uint64_t, UndoNode>;

    // Nodes of dropped subtrees freed per later operation (releaseDropped). Freeing one node costs
    // about 1.3 us on REF-W1 with 10k-100k anchors (its slices and anchor-set path copies).
    static constexpr std::size_t kReleasedPerOp = 4;

    UndoTree() noexcept;

    [[nodiscard]] UndoNode& node(std::uint64_t id) noexcept;
    [[nodiscard]] const UndoNode& node(std::uint64_t id) const noexcept;
    [[nodiscard]] bool contains(std::uint64_t id) const noexcept { return nodes.contains(id); }
    [[nodiscard]] UndoNode& currentNode() noexcept { return node(current); }
    [[nodiscard]] const UndoNode& currentNode() const noexcept { return node(current); }

    // A new child of `parent` (its last child and its redo child) with a fresh id and sequence.
    UndoNode& createChild(std::uint64_t parent) noexcept;
    // Applies the history cap after a node was created (decision 11, section 3.10): while the
    // nodes excluding the root exceed `maxTransactions`, drop the root's oldest child subtree that
    // does not hold the current node (repairing the root's redo child), or re-root when the root
    // has one child. Never removes the current node or its ancestors. A dropped subtree leaves the
    // tree at once (O(1) per node) but is freed later by releaseDropped, so the keystroke that
    // drops an old redo branch of up to maxTransactions nodes does not pay for freeing it.
    void prune(std::uint32_t maxTransactions) noexcept;
    // Frees up to `count` nodes that prune dropped (any order). Document calls it once per
    // recorded change, undo and redo with kReleasedPerOp. After a drop of s nodes the cap is
    // reached again only after s more new nodes, each freeing kReleasedPerOp >= 1 of them, so
    // `dropped` holds at most one dropped subtree: at most maxTransactions nodes. O(count) node
    // destructions.
    void releaseDropped(std::size_t count) noexcept;
    // Drops the compose base (header comment): the current node stopped accepting edits.
    void dropComposeBase() noexcept {
        composeBase = {};
        composeBaseNode = kNoNode;
    }
    // Removes leaf `id` (rollback): the parent's redo child becomes `previousRedoChild` if it still
    // exists, else the parent's child with the largest sequence, else none.
    void removeLeaf(std::uint64_t id, std::uint64_t previousRedoChild) noexcept;
    // The number of edges from the root to the current node.
    [[nodiscard]] std::uint32_t currentDepth() const noexcept;
    // U3 (Document::checkInvariants). O(nodes).
    [[nodiscard]] Result<void> checkShape(std::uint32_t maxTransactions) const noexcept;

    NodeMap nodes;
    // Subtrees the history cap removed, waiting for releaseDropped; not part of the tree (nodeCount
    // excludes them). Destroyed with the UndoTree, before the TextBuffer.
    std::vector<NodeMap::node_type> dropped;
    // X of the current node while it accepts edits; composeBaseNode is that node, or kNoNode when
    // no base is held (Document::checkInvariants, U8).
    PieceTree composeBase;
    std::uint64_t composeBaseNode = kNoNode;
    std::uint64_t root = 0;
    std::uint64_t current = 0;
    std::uint64_t nextId = 1;
    std::uint64_t nextSequence = 1;
    std::uint64_t nodeCount = 1;    // Including the root.
    std::uint64_t stateCounter = 0; // The last stateId handed out.
    // Groups.
    std::uint32_t groupDepth = 0;
    std::uint64_t groupNode = kNoNode; // Created lazily at the group's first change.
    EditKind groupKind = EditKind::Other;
    SelectionSet groupSelectionsBefore;
    std::uint64_t groupPreviousRedoChild = kNoNode; // The parent's redo child before the group node.
    // IME.
    std::optional<Composition> composition;

private:
    void eraseSubtree(std::uint64_t id) noexcept;
};

} // namespace frost::text::detail
