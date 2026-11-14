// SPDX-License-Identifier: Apache-2.0
//
// frost::text::Document: the single writer of a TextBuffer, its transactional undo tree and its
// anchor registry (ADR 0017). Edits, undo, redo, groups and IME composition all report a ChangeSet
// with D8 TextChanges for tree-sitter and LSP and the raw edits for anchors and selections.
/*
 * Undo tree (decisions 5-8 and 11). Nodes are text states; the edge into a node is its
 * transaction, stored as canonical regions {before range of the parent text X, after range of the
 * node text Y, removed slice, inserted slice}, whose slices are pieces of the buffer's append-only
 * storage (nothing is copied). Undo and redo replay those slices through TextBuffer::replaySlices,
 * so their TextChanges are precise and LSP-replayable, at a cost independent of the edited size. A
 * new transaction becomes a new child of the current node, so older children survive as redo
 * branches; redo() follows the most recently created or undone-from child. Removed slices are cut
 * from the parent text's own tree, so a run of coalesced deletes keeps whole pieces. When the
 * history cap drops an old redo branch, its nodes are freed a few per later operation, not all in
 * the keystroke that dropped them.
 *
 * TODO(M11): chronological undo traversal (Vim-style g-/g+), jumping to a state, an undo-tree
 * enumeration API and the undo-tree / Local History view (feature row 218).
 *
 * Coalescing (decision 7). An edit joins the current node's open transaction iff its kind is
 * Typing, DeleteBackward or DeleteForward and equals the transaction's, the transaction is open,
 * now - lastEdit <= coalesceWindow (lastEdit is the transaction's latest edit, so the window
 * slides), and context.selectionsBefore == the transaction's selectionsAfter (the carets did not
 * move; empty sets compare equal). The decision is taken after validation, so a rejected edit seals
 * nothing. Undo, redo, rollback, beginGroup, endGroup, breakCoalescing() and any recorded edit
 * that does not coalesce seal the transaction; a node that the history cap makes the root has its
 * transaction data dropped (it is never the current node, whose transaction stays open).
 *
 * Groups (decision 8). beginGroup/endGroup nest; the outermost pair forms one transaction, created
 * lazily at the first text change, with the outermost beginGroup's kind and selectionsBefore and
 * the outermost endGroup's selectionsAfter. Groups never coalesce; each edit inside stays
 * all-or-nothing. rollbackGroup() reverts and discards the whole group and leaves no redo branch.
 *
 * IME (decision 9). Preedit is applied to the buffer (every consumer sees it, origin Composition)
 * and never recorded. A commit records one Typing step relative to the text at composition start
 * (the base); it may coalesce with earlier typing. A cancel records nothing. While composing,
 * edit, undo, redo and groups return Busy. committedText() is the text without the preedit; save,
 * autosave, the hot-exit journal and local history persist it, never text().
 *
 * Anchors (decisions 1, 2 and 10). Every text change maps the registry through its raw batch, so
 * anchors().version() == version() and every anchor stays on a char boundary. Each transaction
 * keeps the anchor sets before and after it; undo puts every live anchor that existed before the
 * transaction back at its offset there (redo likewise with the set after it), so undo then redo is
 * the identity on text and anchors, and a bookmark inside deleted text comes back on undo.
 *
 * Errors are checked in a fixed order shared with the test oracle (the slice spec's section 3.8,
 * repeated on each member). Every rejection leaves the text, version, anchors, undo tree (with the
 * coalescing state and stateId), group and composition state unchanged.
 *
 * Thread safety: not thread-safe; one writer thread (like TextBuffer). DocumentSnapshot,
 * AnchorSet and SelectionSet are immutable values safe on any thread. Allocation failure
 * terminates (noexcept). The IClock must outlive the Document.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/core/transaction_id.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/anchor_set.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/selection.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text {

namespace detail {
struct UndoTree; // src/text/src/undo_tree.hpp: the undo tree, group and composition state.
} // namespace detail

// What an edit was, for coalescing and for history labels.
enum class EditKind : std::uint8_t { Other, Typing, DeleteBackward, DeleteForward, Paste, External };

// Where a ChangeSet came from. Consumers that defer Composition changes (the M3 LSP client) flush
// or drop them at the next non-Composition ChangeSet.
enum class ChangeOrigin : std::uint8_t { Edit, Undo, Redo, Rollback, Composition };

// The caller's view of an edit. Selections are stored as given and never resolved or
// boundary-checked: selectionsBefore refers to the text before the edit (the composition base for
// a commit) and selectionsAfter to the text after it. Empty sets are allowed.
struct EditContext {
    EditKind kind = EditKind::Other;
    SelectionSet selectionsBefore;
    SelectionSet selectionsAfter;
};

/*
 * What consumers receive. `changes` (D8 TextChanges, in application order, each in its own step's
 * coordinates) are for tree-sitter and LSP; `edits` (one raw batch, ascending, no no-ops) are for
 * anchors and other editors' SelectionSets; `selections` are the selections to restore (undo,
 * redo, rollback) or the ones echoed (edit). Both apply to the snapshot at baseVersion and produce
 * version; the two versions are equal when nothing changed.
 *
 * Per operation: edit/replace: origin Edit, the requested edits, context.selectionsAfter. undo:
 * Undo, [{r.after, |r.before|}] over the node's regions, the node's selectionsBefore. redo: Redo,
 * [{r.before, |r.after|}], the node's selectionsAfter. rollbackGroup: Rollback, as the group's undo
 * (empty if it changed nothing), the group's selectionsBefore. updateComposition: Composition,
 * [{[start, start + len), |preedit|}], empty selections. commitComposition: Edit,
 * [{[rb, re + len), |text|}], context.selectionsAfter. cancelComposition: Composition,
 * [{[start, start + len), 0}], empty selections.
 *
 * New-text extraction (undo, redo, rollback): change i's new bytes are the post-op snapshot's bytes
 * [start_i + s_i, newEnd_i + s_i), where s_i is the sum of (newEnd - oldEnd) over the changes
 * applied after change i (which all lie before it): regions are separated by kept bytes, which D8's
 * one-byte widening never alters. Edit batches whose widening reaches an adjacent requested edit
 * may not satisfy this rule (an M3 interop note in ADR 0017).
 */
struct ChangeSet {
    ChangeOrigin origin = ChangeOrigin::Edit;
    std::uint64_t baseVersion = 0;
    std::uint64_t version = 0;
    std::vector<RawEdit> edits;
    std::vector<TextChange> changes;
    SelectionSet selections;
};

// Document options; at namespace scope because a nested struct with member initializers cannot be
// a default argument inside its own class. FROST_PRE: coalesceWindow >= 0, maxTransactions >= 1.
struct DocumentOptions {
    // Typing and deletes within this time of the previous edit of their run coalesce.
    std::chrono::milliseconds coalesceWindow{1000};
    // The history cap, in undo-tree nodes excluding the root (decision 11).
    std::uint32_t maxTransactions = 1000;
};

// A text snapshot and the anchor registry at the same version; any thread. O(1) to copy.
struct DocumentSnapshot {
    Snapshot text;
    AnchorSet anchors;
};

// The history state, for menus and the save manager.
struct UndoState {
    bool canUndo = false;           // The current node is not the root (undo may still be Busy).
    bool canRedo = false;           // The current node has a child to redo.
    std::uint32_t redoBranches = 0; // The current node's children.
    std::uint32_t transactions = 0; // Nodes excluding the root.
    bool groupOpen = false;
    bool composing = false;
    // Identifies the committed text state (decision 14): a per-Document counter stamped on the
    // current node at every recorded change; undo, redo and rollback make the target node's value
    // current; preedit, no-ops and rejections leave it. Equal values mean equal committed text (the
    // converse does not hold). SaveManager stores it at save for the dirty flag.
    std::uint64_t stateId = 0;
    TransactionId externalUndo; // Assigned: route Undo through the owning global transaction.
    TransactionId externalRedo; // Assigned: local redo cannot replay just this document.
    bool externallyLocked = false;
};

class Document {
public:
    // A document over `buffer` with an empty history and registry. The anchor registry starts at
    // the buffer's version and size. Precondition: `options` within the bounds above; `clock`
    // outlives the Document.
    explicit Document(TextBuffer buffer, DocumentOptions options = {}, const IClock& clock = systemClock()) noexcept;
    // TextBuffer::fromUtf8 (its errors) wrapped in a Document. Budget: B15.
    [[nodiscard]] static Result<Document> fromUtf8(std::string text, TextBufferOptions bufferOptions = {},
                                                   DocumentOptions options = {},
                                                   const IClock& clock = systemClock()) noexcept;

    // Movable; a moved-from Document may only be destroyed or assigned to.
    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;
    ~Document();
    Document(const Document&) = delete ("a Document is the only writer of its buffer and history");
    Document& operator=(const Document&) = delete ("a Document is the only writer of its buffer and history");

    // The current text including any preedit, borrowed: invalidated by the next change. O(1).
    [[nodiscard]] const Snapshot& text() const noexcept;
    // The text without the preedit (the composition base while composing, else text()). O(1).
    [[nodiscard]] Snapshot committedText() const noexcept;
    // The text and anchor registry. O(1). Budget: B5.
    [[nodiscard]] DocumentSnapshot snapshot() const noexcept;
    // The anchor registry, borrowed: invalidated by any change, createAnchor or releaseAnchor.
    [[nodiscard]] const AnchorSet& anchors() const noexcept { return m_anchors; }
    // The buffer version (TextBuffer::version()). O(1).
    [[nodiscard]] std::uint64_t version() const noexcept;
    // O(1).
    [[nodiscard]] UndoState undoState() const noexcept;

    // Owner-thread coordinator lease. Refuses composition/groups/another writer. Normal edits,
    // composition and local undo/redo return Busy while held. IDs identify a coordinator, not IO authority.
    [[nodiscard]] Result<void> acquireExternalWrite(TransactionId id) noexcept;
    void releaseExternalWrite(TransactionId id) noexcept;
    [[nodiscard]] bool ownsExternalWrite(TransactionId id) const noexcept { return id.assigned() && m_externalWriter == id; }
    [[nodiscard]] Result<ChangeSet> replaceExternal(TransactionId id, ByteRange range, std::string_view utf8,
                                                    const EditContext& context) noexcept;
    // Marks the current sealed nonroot edge; tombstone survives coordinator destruction. Local
    // Undo/Redo refuses it until the owning coordinator admits the entire inverse/forward plan.
    [[nodiscard]] Result<void> markExternalTransaction(TransactionId id) noexcept;
    // A real zero-region edge for rename/persistence-only effects; never marks an earlier user edit.
    // Empty selections deliberately preserve each view's carets (Editor::follow/finishHistory map no edits).
    [[nodiscard]] Result<void> recordExternalBarrier(TransactionId id) noexcept;
    [[nodiscard]] Result<ChangeSet> undoExternal(TransactionId id) noexcept;
    [[nodiscard]] Result<ChangeSet> redoExternal(TransactionId id) noexcept;

    // A new registry anchor. Errors, in order: OutOfRange (offset > size); InvalidArgument (inside
    // a UTF-8 sequence; a CRLF-interior offset is allowed); OutOfRange (AnchorSet::kMaxAnchors
    // anchors). Allowed while composing and inside groups. O(B log n) amortised.
    [[nodiscard]] Result<AnchorId> createAnchor(ByteOffset offset, Bias bias) noexcept;
    // Errors: NotFound. O(B log n).
    [[nodiscard]] Result<void> releaseAnchor(AnchorId id) noexcept;

    /*
     * Applies `edits` atomically (TextBuffer::applyEdits) and records them (coalescing, groups).
     * Errors, in order: Busy ("an IME composition is active"); then TextBuffer's checks 1-5 in
     * applyEdits' order. An empty span, or only no-op edits, changes nothing: an empty ChangeSet
     * (origin Edit, baseVersion == version, selections = context.selectionsAfter) and nothing is
     * recorded. Precondition (FROST_PRE, after validation): context.selectionsBefore.maxOffset()
     * <= the size before and context.selectionsAfter.maxOffset() <= the size after.
     * O(TextBuffer::applyEdits + m B log n (anchors) + regions + m (B log n + P)). Budget: B5.
     */
    [[nodiscard]] Result<ChangeSet> edit(std::span<const Edit> edits, const EditContext& context) noexcept;
    // One edit through TextBuffer::replace (no per-edit message label); otherwise as edit(). A
    // no-op reports no change. Budget: B5.
    [[nodiscard]] Result<ChangeSet> replace(ByteRange range, std::string_view utf8,
                                            const EditContext& context) noexcept;

    // Seals the open transaction, so the next edit starts a new one. O(1).
    void breakCoalescing() noexcept;

    // Opens a group (nested groups form one transaction). Errors: Busy (composing). Seals the open
    // transaction at the outermost level. O(1).
    [[nodiscard]] Result<void> beginGroup(const EditContext& context) noexcept;
    // Closes one level; the outermost stores `selectionsAfter` and seals. Errors: InvalidArgument
    // (no group open). An empty group records nothing. O(1).
    [[nodiscard]] Result<void> endGroup(const SelectionSet& selectionsAfter) noexcept;
    // Reverts and discards the outermost group at any depth (origin Rollback, the group's
    // selectionsBefore); the parent's redo child is restored if it still exists. Errors:
    // InvalidArgument (no group open). Cost as undo().
    [[nodiscard]] Result<ChangeSet> rollbackGroup() noexcept;

    /*
     * Undoes the current node's transaction. Errors, in order: Busy (composing); Busy ("a group is
     * open"); NotFound ("nothing to undo"). A transaction whose regions all cancelled out changes no
     * text and keeps the version, but still moves the current node, stateId and selections.
     * Cost: O(regions × (B log n + P) + r × B log n) for the r anchors the restore moves, plus
     * O(B log n + k log k) per region for the k anchors in its closed range. Budget: B5.
     */
    [[nodiscard]] Result<ChangeSet> undo() noexcept;
    // Redoes the current node's redo child, or child `branch` (in creation order). Errors, in
    // order: Busy (composing); Busy (group open); with a branch, OutOfRange if branch >= children;
    // without, NotFound if there is no child. Cost as undo(). Budget: B5.
    [[nodiscard]] Result<ChangeSet> redo(std::optional<std::uint32_t> branch = std::nullopt) noexcept;

    // Starts an IME composition at `at` with an empty preedit; does not seal. Errors, in order:
    // Busy (composing); Busy (group open); OutOfRange (at > size); InvalidArgument (inside a UTF-8
    // sequence). O(1).
    [[nodiscard]] Result<void> beginComposition(ByteOffset at) noexcept;
    // Replaces the preedit with `preedit` (origin Composition; nothing recorded). Errors, in order:
    // InvalidArgument (not composing); TextBuffer checks 4-5 on the replacement. Budget: B5.
    [[nodiscard]] Result<ChangeSet> updateComposition(std::string_view preedit) noexcept;
    /*
     * Replaces the preedit, and `replaceInBase` (a range [rb, re) of the base text with
     * rb <= start <= re; default [start, start)), with `text`, ends the composition and records the
     * net batch [{[rb, re), |text|}] against the base as one Typing step (which may coalesce;
     * context.kind is ignored; context.selectionsBefore refers to the base text). Empty text with
     * an empty replacement range is a cancel (origin Composition, nothing recorded). Errors, in
     * order: InvalidArgument (not composing); with replaceInBase: InvalidArgument (begin > end),
     * OutOfRange (end > base size), InvalidArgument (does not contain the start), InvalidArgument (a
     * bound inside a UTF-8 sequence of the base); then TextBuffer checks 4-5 on the replacement. On
     * error the composition stays active. Budget: B5.
     */
    [[nodiscard]] Result<ChangeSet> commitComposition(std::string_view text, const EditContext& context,
                                                      std::optional<ByteRange> replaceInBase = std::nullopt) noexcept;
    // Removes the preedit and ends the composition; records nothing. Errors: InvalidArgument (not
    // composing).
    [[nodiscard]] Result<ChangeSet> cancelComposition() noexcept;
    // The preedit's range in text() while composing.
    [[nodiscard]] std::optional<ByteRange> compositionRange() const noexcept;

    /*
     * Checks U1-U8 (ADR 0017): U1 the TextBuffer invariants; U2 the registry's invariants, its
     * version and size match the text, every anchor is on a char boundary; U3 the tree's shape
     * (parent and children agree, redoChild is a child and none exactly when there are none, the
     * current node is reachable, nodes - 1 <= maxTransactions, at most maxTransactions dropped
     * nodes wait to be freed, sequences increase along children, unique ids and stateIds); U4 every
     * node's regions are canonical with slice sizes equal to their ranges and anchor sets sized
     * like X and Y; U5 the current node's inserted slices equal the committed text and, while it
     * accepts edits, its removed slices equal its parent text (the compose base); U6 while
     * composing, the text is base[0, start) + preedit + base[start, ..) over shared storage; U7 a
     * group and a composition are never active together and the group node is current; U8 an open
     * transaction is the current non-root node with a coalescable kind and no group open, and the
     * compose base is held exactly while the current node accepts edits (open, or the open
     * group's node). Returns ErrorCode::Internal naming the first violation. Writer thread;
     * O(bytes + anchors B log n + regions).
     */
    [[nodiscard]] Result<void> checkInvariants() const noexcept;

    // The undo tree, for white-box tests. Not public API.
    [[nodiscard]] const detail::UndoTree& undoTreeForTesting() const noexcept;

private:
    // Records the change from `before` (the tree the raw batch `edits` applies to) to the current
    // text: into the group node, by coalescing, or as a new child (decisions 7 and 8).
    void recordChange(const detail::PieceTree& before, std::span<const RawEdit> edits, const EditContext& context,
                      EditKind kind, const AnchorSet& anchorsBefore) noexcept;
    // Maps the registry through `edits`, records them and builds the edit's ChangeSet.
    [[nodiscard]] ChangeSet finishEdit(std::uint64_t baseVersion, const detail::PieceTree& before,
                                       std::vector<RawEdit> edits, std::vector<TextChange> changes,
                                       const EditContext& context) noexcept;
    // Replays `slices`, maps the registry through `raw` and restores the anchors of `restoreFrom`
    // that lie in the closed `ranges` of the text after the replay. Returns the D8 changes.
    std::vector<TextChange> replay(std::span<const detail::SliceEdit> slices, std::span<const RawEdit> raw,
                                   const AnchorSet& restoreFrom, std::span<const ByteRange> ranges) noexcept;
    [[nodiscard]] ChangeSet undoNode(std::uint64_t node, ChangeOrigin origin) noexcept;
    // Replaces `range` (the preedit) with `text` through TextBuffer::replace and maps the
    // registry; records nothing. On success the preedit length becomes |text|.
    [[nodiscard]] Result<ChangeSet> compositionChange(ByteRange range, std::string_view text) noexcept;

    // Declaration order matters for destruction: m_undo (whose slices point into the buffer's
    // storage) is destroyed first, then the registry, then the buffer. Destroying a slice never
    // reads piece bytes, but this order keeps the storage alive throughout anyway.
    TextBuffer m_buffer;
    AnchorSet m_anchors;
    std::unique_ptr<detail::UndoTree> m_undo; // Pimpl: tree, group and composition state.
    DocumentOptions m_options;
    const IClock* m_clock = nullptr;
    TransactionId m_externalWriter;
    TransactionId m_externalPermit;
};

} // namespace frost::text
