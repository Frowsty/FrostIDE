// SPDX-License-Identifier: Apache-2.0
//
// Document (frost/text/document.hpp): edits with recording and coalescing, undo and redo by slice
// replay with anchor restoration, groups and rollback, IME composition, the history state and the
// invariants U1-U8 (ADR 0017).
/*
 * Why the restore scan is enough (decision 10, section 4.6 of the slice spec). After an undo the
 * registry is the pre-undo set mapped through the undo's raw batch; the scan then moves the anchors
 * in the closed X ranges of the regions that anchorsBefore holds to their anchorsBefore offsets.
 *   - An anchor that was alive before the transaction at a kept X position (outside every closed
 *     before-range) has a kept byte on each side. Attachment semantics keep it between those two
 *     bytes through every edit of the transaction, so it sits at the corresponding kept Y
 *     position, and the inverse batch maps it back exactly.
 *   - Mapping is monotone and never moves an anchor past a surviving byte, so an anchor in a closed
 *     before-range ends in that region's closed after-range, and the inverse batch sends it to the
 *     endpoints of the before-range, where the scan finds it.
 *   - A cancelled preedit leaves pre-existing anchors where they were, and a commit moves them as
 *     its net batch does (section 3.6), so compositions do not break the two points above.
 * So after the scan every anchor that anchorsBefore holds is at its offset there. Redo is the
 * mirror image with anchorsAfter and the closed Y ranges. The differential test's oracle applies
 * decision 10 literally (no scan), which checks this argument on every undo and redo.
 */

#include "frost/text/document.hpp"

#include "frost/core/assert.hpp"

#include "piece_tree.hpp"
#include "undo_tree.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace frost::text {
namespace {

using detail::kNoNode;
using detail::UndoNode;
using detail::UndoTree;

constexpr bool coalescable(EditKind kind) noexcept {
    return kind == EditKind::Typing || kind == EditKind::DeleteBackward || kind == EditKind::DeleteForward;
}

bool byId(const ResolvedAnchor& x, const ResolvedAnchor& y) noexcept { return x.id < y.id; }

// The bytes of a slice. O(bytes).
std::string sliceText(const detail::PieceTree& slice) {
    std::string out;
    out.reserve(static_cast<std::size_t>(slice.summary().bytes));
    for (detail::PieceTree::Cursor c = slice.first(); c.item() != nullptr; c.next()) {
        out += c.item()->view();
    }
    return out;
}

// The bytes [a, b) of `tree`. O(B log n + P + (b - a)).
std::string treeText(const detail::PieceTree& tree, std::uint64_t a, std::uint64_t b) {
    const detail::PieceTree right = detail::splitAtByte(tree, a).second;
    return sliceText(detail::splitAtByte(right, b - a).first);
}

std::int64_t sizeDelta(const UndoNode& node) noexcept {
    std::int64_t delta = 0;
    for (const detail::Region& r : node.regions) {
        delta += static_cast<std::int64_t>(r.after.size()) - static_cast<std::int64_t>(r.before.size());
    }
    return delta;
}

} // namespace

Document::Document(TextBuffer buffer, DocumentOptions options, const IClock& clock) noexcept
    : m_buffer(std::move(buffer)), m_anchors(m_buffer.version(), m_buffer.current().size()),
      m_undo(std::make_unique<detail::UndoTree>()), m_options(options), m_clock(&clock) {
    FROST_PRE(options.coalesceWindow >= std::chrono::milliseconds{0} && options.maxTransactions >= 1);
}

Result<Document> Document::fromUtf8(std::string text, TextBufferOptions bufferOptions, DocumentOptions options,
                                    const IClock& clock) noexcept {
    Result<TextBuffer> buffer = TextBuffer::fromUtf8(std::move(text), bufferOptions);
    if (!buffer) {
        return std::unexpected(std::move(buffer).error());
    }
    return Document(std::move(*buffer), options, clock);
}

Document::Document(Document&& other) noexcept = default;

Document& Document::operator=(Document&& other) noexcept {
    if (this != &other) {
        // The old undo tree goes first, while the old buffer (whose storage its slices point
        // into) is still alive.
        m_undo = std::move(other.m_undo);
        m_anchors = std::move(other.m_anchors);
        m_buffer = std::move(other.m_buffer);
        m_options = other.m_options;
        m_clock = other.m_clock;
    }
    return *this;
}

Document::~Document() = default;

const Snapshot& Document::text() const noexcept { return m_buffer.current(); }

Snapshot Document::committedText() const noexcept {
    return m_undo->composition ? m_undo->composition->base : m_buffer.snapshot();
}

DocumentSnapshot Document::snapshot() const noexcept { return DocumentSnapshot{m_buffer.snapshot(), m_anchors}; }

std::uint64_t Document::version() const noexcept { return m_buffer.version(); }

UndoState Document::undoState() const noexcept {
    const UndoTree& u = *m_undo;
    const UndoNode& current = u.currentNode();
    UndoState state;
    state.canUndo = u.current != u.root;
    state.canRedo = current.redoChild != kNoNode;
    state.redoBranches = static_cast<std::uint32_t>(current.children.size());
    state.transactions = static_cast<std::uint32_t>(u.nodeCount - 1);
    state.groupOpen = u.groupDepth > 0;
    state.composing = u.composition.has_value();
    state.stateId = current.stateId;
    return state;
}

const detail::UndoTree& Document::undoTreeForTesting() const noexcept { return *m_undo; }

Result<AnchorId> Document::createAnchor(ByteOffset offset, Bias bias) noexcept {
    const Snapshot& text = m_buffer.current();
    if (offset > text.size()) {
        return fail(ErrorCode::OutOfRange, std::format("offset {} is beyond the text's {} bytes", offset, text.size()));
    }
    if (!text.isCharBoundary(offset)) {
        return fail(ErrorCode::InvalidArgument, std::format("byte {} is inside a UTF-8 sequence", offset));
    }
    if (m_anchors.size() >= AnchorSet::kMaxAnchors) {
        return fail(ErrorCode::OutOfRange, std::format("the registry holds {} anchors already", m_anchors.size()));
    }
    auto [anchors, id] = m_anchors.withAnchor(offset, bias);
    m_anchors = std::move(anchors);
    return id;
}

Result<void> Document::releaseAnchor(AnchorId id) noexcept {
    Result<AnchorSet> anchors = m_anchors.without(id);
    if (!anchors) {
        return std::unexpected(std::move(anchors).error());
    }
    m_anchors = std::move(*anchors);
    return {};
}

Result<ChangeSet> Document::edit(std::span<const Edit> edits, const EditContext& context) noexcept {
    if (m_undo->composition) {
        return fail(ErrorCode::Busy, "an IME composition is active");
    }
    const std::uint64_t baseVersion = m_buffer.version();
    const detail::PieceTree before = m_buffer.current().pieceTree();
    Result<std::vector<TextChange>> changes = m_buffer.applyEdits(edits);
    if (!changes) {
        return std::unexpected(std::move(changes).error());
    }
    std::vector<RawEdit> raw;
    raw.reserve(edits.size());
    for (const Edit& e : edits) {
        const RawEdit step{e.range, e.text.size()};
        if (!step.isNoOp()) {
            raw.push_back(step);
        }
    }
    return finishEdit(baseVersion, before, std::move(raw), std::move(*changes), context);
}

Result<ChangeSet> Document::replace(ByteRange range, std::string_view utf8, const EditContext& context) noexcept {
    if (m_undo->composition) {
        return fail(ErrorCode::Busy, "an IME composition is active");
    }
    const std::uint64_t baseVersion = m_buffer.version();
    const detail::PieceTree before = m_buffer.current().pieceTree();
    Result<TextChange> change = m_buffer.replace(range, utf8);
    if (!change) {
        return std::unexpected(std::move(change).error());
    }
    std::vector<RawEdit> raw;
    std::vector<TextChange> changes;
    const RawEdit step{range, utf8.size()};
    if (!step.isNoOp()) { // A no-op's empty change is dropped, as applyEdits does.
        raw.push_back(step);
        changes.push_back(*change);
    }
    return finishEdit(baseVersion, before, std::move(raw), std::move(changes), context);
}

ChangeSet Document::finishEdit(std::uint64_t baseVersion, const detail::PieceTree& before, std::vector<RawEdit> edits,
                               std::vector<TextChange> changes, const EditContext& context) noexcept {
    ChangeSet out;
    out.origin = ChangeOrigin::Edit;
    out.baseVersion = baseVersion;
    out.version = m_buffer.version();
    out.selections = context.selectionsAfter;
    if (edits.empty()) {
        FROST_ASSERT(out.version == baseVersion && changes.empty());
        return out; // Nothing changed: nothing recorded, the coalescing state is kept.
    }
    FROST_ASSERT(out.version == baseVersion + 1);
    FROST_PRE(context.selectionsBefore.maxOffset().value() <= before.summary().bytes &&
              context.selectionsAfter.maxOffset() <= m_buffer.current().size());
    const AnchorSet anchorsBefore = m_anchors;
    m_anchors = m_anchors.transformed(edits, out.version);
    recordChange(before, edits, context, context.kind, anchorsBefore);
    out.edits = std::move(edits);
    out.changes = std::move(changes);
    return out;
}

void Document::recordChange(const detail::PieceTree& before, std::span<const RawEdit> edits, const EditContext& context,
                            EditKind kind, const AnchorSet& anchorsBefore) noexcept {
    UndoTree& u = *m_undo;
    const IClock::SteadyTime now = m_clock->steadyNow();
    const detail::PieceTree& after = m_buffer.current().pieceTree();
    if (u.groupDepth > 0) {
        // Decision 8: every change of the group goes into one node, created at the first one.
        if (u.groupNode == kNoNode) {
            u.groupPreviousRedoChild = u.currentNode().redoChild;
            UndoNode& created = u.createChild(u.current);
            created.kind = u.groupKind;
            created.selectionsBefore = u.groupSelectionsBefore;
            created.anchorsBefore = anchorsBefore;
            u.current = created.id;
            u.groupNode = created.id;
            u.composeBase = before; // X of the group (undo_tree.hpp, the compose base).
            u.composeBaseNode = created.id;
            u.prune(m_options.maxTransactions);
        }
        UndoNode& group = u.currentNode();
        FROST_ASSERT(u.composeBaseNode == group.id);
        group.regions = detail::composeRegions(group.regions, edits, u.composeBase, after);
        group.lastEdit = now;
        group.stateId = ++u.stateCounter;
        u.releaseDropped(UndoTree::kReleasedPerOp);
        return;
    }
    UndoNode& current = u.currentNode();
    // Decision 7. The current node is never the root when open (U8).
    if (current.open && coalescable(kind) && kind == current.kind &&
        now - current.lastEdit <= m_options.coalesceWindow && context.selectionsBefore == current.selectionsAfter) {
        FROST_ASSERT(u.composeBaseNode == current.id);
        current.regions = detail::composeRegions(current.regions, edits, u.composeBase, after);
        current.selectionsAfter = context.selectionsAfter;
        current.lastEdit = now;
        current.stateId = ++u.stateCounter;
        u.releaseDropped(UndoTree::kReleasedPerOp);
        return;
    }
    current.open = false;
    u.dropComposeBase();
    UndoNode& created = u.createChild(u.current);
    created.kind = kind;
    created.regions = detail::composeRegions({}, edits, before, after);
    created.selectionsBefore = context.selectionsBefore;
    created.selectionsAfter = context.selectionsAfter;
    created.anchorsBefore = anchorsBefore;
    created.lastEdit = now;
    created.open = coalescable(kind);
    created.stateId = ++u.stateCounter;
    if (created.open) {
        u.composeBase = before;
        u.composeBaseNode = created.id;
    }
    u.current = created.id;
    u.prune(m_options.maxTransactions);
    u.releaseDropped(UndoTree::kReleasedPerOp);
}

void Document::breakCoalescing() noexcept {
    UndoTree& u = *m_undo;
    u.currentNode().open = false;
    if (u.groupNode == kNoNode) {
        u.dropComposeBase(); // An open group's node keeps its base until endGroup.
    }
}

std::vector<TextChange> Document::replay(std::span<const detail::SliceEdit> slices, std::span<const RawEdit> raw,
                                         const AnchorSet& restoreFrom, std::span<const ByteRange> ranges) noexcept {
    std::vector<TextChange> changes = m_buffer.replaySlices(slices);
    AnchorSet mapped = m_anchors.transformed(raw, m_buffer.version());
    /*
     * The restore scan (header comment): the anchors in each closed range, matched by id with the
     * anchors of `restoreFrom` in the same range. Any other anchor is looked up with find(), which
     * rejects an id newer than all of restoreFrom's in O(1) and never allocates. Those are the
     * anchors created after restoreFrom was taken (for example inside a paste whose undo collapses
     * them), the only kind the scan can meet there: an older live anchor is in restoreFrom (ids
     * are never reused) and, by the argument above, in the same closed range.
     */
    std::vector<ResolvedAnchor> moves;
    for (const ByteRange& range : ranges) {
        std::vector<ResolvedAnchor> here = mapped.resolveRange(range);
        if (here.empty()) {
            continue;
        }
        std::vector<ResolvedAnchor> there = restoreFrom.resolveRange(range);
        std::sort(here.begin(), here.end(), byId);
        std::sort(there.begin(), there.end(), byId);
        std::size_t k = 0;
        for (const ResolvedAnchor& anchor : here) {
            while (k < there.size() && there[k].id < anchor.id) {
                ++k;
            }
            ByteOffset target = anchor.offset;
            if (k < there.size() && there[k].id == anchor.id) {
                target = there[k].offset;
            } else if (const std::optional<ByteOffset> elsewhere = restoreFrom.find(anchor.id)) {
                target = *elsewhere;
            }
            if (target != anchor.offset) {
                moves.push_back(ResolvedAnchor{anchor.id, target, anchor.bias});
            }
        }
    }
    m_anchors = mapped.withMoved(moves);
    return changes;
}

ChangeSet Document::undoNode(std::uint64_t id, ChangeOrigin origin) noexcept {
    UndoTree& u = *m_undo;
    UndoNode& node = u.node(id);
    const std::uint64_t baseVersion = m_buffer.version();
    node.anchorsAfter = m_anchors;
    node.hasAnchorsAfter = true;
    std::vector<detail::SliceEdit> slices;
    std::vector<RawEdit> raw;
    std::vector<ByteRange> ranges;
    slices.reserve(node.regions.size());
    raw.reserve(node.regions.size());
    ranges.reserve(node.regions.size());
    for (const detail::Region& r : node.regions) {
        slices.push_back(detail::SliceEdit{r.after, r.removed});
        raw.push_back(RawEdit{r.after, r.before.size()});
        ranges.push_back(r.before);
    }
    std::vector<TextChange> changes = replay(slices, raw, node.anchorsBefore, ranges);
    node.open = false;
    u.dropComposeBase(); // `id` is the current node (undo, or the open group's node at rollback).
    u.current = node.parent;
    u.node(node.parent).redoChild = id;
    u.releaseDropped(UndoTree::kReleasedPerOp);
    ChangeSet out;
    out.origin = origin;
    out.baseVersion = baseVersion;
    out.version = m_buffer.version();
    out.edits = std::move(raw);
    out.changes = std::move(changes);
    out.selections = node.selectionsBefore;
    return out;
}

Result<ChangeSet> Document::undo() noexcept {
    UndoTree& u = *m_undo;
    if (u.composition) {
        return fail(ErrorCode::Busy, "an IME composition is active");
    }
    if (u.groupDepth > 0) {
        return fail(ErrorCode::Busy, "a group is open");
    }
    if (u.current == u.root) {
        return fail(ErrorCode::NotFound, "nothing to undo");
    }
    return undoNode(u.current, ChangeOrigin::Undo);
}

Result<ChangeSet> Document::redo(std::optional<std::uint32_t> branch) noexcept {
    UndoTree& u = *m_undo;
    if (u.composition) {
        return fail(ErrorCode::Busy, "an IME composition is active");
    }
    if (u.groupDepth > 0) {
        return fail(ErrorCode::Busy, "a group is open");
    }
    UndoNode& current = u.currentNode();
    std::uint64_t id = kNoNode;
    if (branch) {
        if (*branch >= current.children.size()) {
            return fail(ErrorCode::OutOfRange,
                        std::format("redo branch {} of {} branches", *branch, current.children.size()));
        }
        id = current.children[*branch];
    } else {
        if (current.redoChild == kNoNode) {
            return fail(ErrorCode::NotFound, "nothing to redo");
        }
        id = current.redoChild;
    }
    UndoNode& child = u.node(id);
    FROST_ASSERT(child.hasAnchorsAfter); // Reaching the parent took an undo of the child.
    const std::uint64_t baseVersion = m_buffer.version();
    child.anchorsBefore = m_anchors;
    std::vector<detail::SliceEdit> slices;
    std::vector<RawEdit> raw;
    std::vector<ByteRange> ranges;
    slices.reserve(child.regions.size());
    raw.reserve(child.regions.size());
    ranges.reserve(child.regions.size());
    for (const detail::Region& r : child.regions) {
        slices.push_back(detail::SliceEdit{r.before, r.inserted});
        raw.push_back(RawEdit{r.before, r.after.size()});
        ranges.push_back(r.after);
    }
    std::vector<TextChange> changes = replay(slices, raw, child.anchorsAfter, ranges);
    current.open = false;
    u.dropComposeBase();
    current.redoChild = id;
    u.current = id;
    u.releaseDropped(UndoTree::kReleasedPerOp);
    ChangeSet out;
    out.origin = ChangeOrigin::Redo;
    out.baseVersion = baseVersion;
    out.version = m_buffer.version();
    out.edits = std::move(raw);
    out.changes = std::move(changes);
    out.selections = child.selectionsAfter;
    return out;
}

Result<void> Document::beginGroup(const EditContext& context) noexcept {
    UndoTree& u = *m_undo;
    if (u.composition) {
        return fail(ErrorCode::Busy, "an IME composition is active");
    }
    if (u.groupDepth == 0) {
        u.currentNode().open = false;
        u.dropComposeBase();
        u.groupKind = context.kind;
        u.groupSelectionsBefore = context.selectionsBefore;
        u.groupNode = kNoNode;
        u.groupPreviousRedoChild = kNoNode;
    }
    ++u.groupDepth;
    return {};
}

Result<void> Document::endGroup(const SelectionSet& selectionsAfter) noexcept {
    UndoTree& u = *m_undo;
    if (u.groupDepth == 0) {
        return fail(ErrorCode::InvalidArgument, "no group is open");
    }
    if (--u.groupDepth > 0) {
        return {};
    }
    if (u.groupNode != kNoNode) {
        u.node(u.groupNode).selectionsAfter = selectionsAfter;
    }
    u.currentNode().open = false;
    u.dropComposeBase();
    u.groupNode = kNoNode;
    u.groupSelectionsBefore = {};
    u.groupPreviousRedoChild = kNoNode;
    return {};
}

Result<ChangeSet> Document::rollbackGroup() noexcept {
    UndoTree& u = *m_undo;
    if (u.groupDepth == 0) {
        return fail(ErrorCode::InvalidArgument, "no group is open");
    }
    ChangeSet out;
    if (u.groupNode == kNoNode) {
        out.origin = ChangeOrigin::Rollback;
        out.baseVersion = m_buffer.version();
        out.version = m_buffer.version();
    } else {
        const std::uint64_t group = u.groupNode;
        out = undoNode(group, ChangeOrigin::Rollback);
        u.removeLeaf(group, u.groupPreviousRedoChild);
    }
    out.selections = u.groupSelectionsBefore;
    u.groupDepth = 0;
    u.groupNode = kNoNode;
    u.groupSelectionsBefore = {};
    u.groupPreviousRedoChild = kNoNode;
    return out;
}

Result<void> Document::beginComposition(ByteOffset at) noexcept {
    UndoTree& u = *m_undo;
    if (u.composition) {
        return fail(ErrorCode::Busy, "an IME composition is active");
    }
    if (u.groupDepth > 0) {
        return fail(ErrorCode::Busy, "a group is open");
    }
    const Snapshot& text = m_buffer.current();
    if (at > text.size()) {
        return fail(ErrorCode::OutOfRange, std::format("offset {} is beyond the text's {} bytes", at, text.size()));
    }
    if (!text.isCharBoundary(at)) {
        return fail(ErrorCode::InvalidArgument, std::format("byte {} is inside a UTF-8 sequence", at));
    }
    u.composition = detail::Composition{at, 0, m_buffer.snapshot(), m_anchors};
    return {};
}

Result<ChangeSet> Document::compositionChange(ByteRange range, std::string_view text) noexcept {
    const std::uint64_t baseVersion = m_buffer.version();
    Result<TextChange> change = m_buffer.replace(range, text);
    if (!change) {
        return std::unexpected(std::move(change).error());
    }
    ChangeSet out;
    out.origin = ChangeOrigin::Composition;
    out.baseVersion = baseVersion;
    const RawEdit step{range, text.size()};
    if (!step.isNoOp()) {
        out.edits.push_back(step);
        out.changes.push_back(*change);
        m_anchors = m_anchors.transformed(out.edits, m_buffer.version());
    }
    out.version = m_buffer.version();
    m_undo->composition->length = text.size();
    return out;
}

Result<ChangeSet> Document::updateComposition(std::string_view preedit) noexcept {
    const std::optional<detail::Composition>& composition = m_undo->composition;
    if (!composition) {
        return fail(ErrorCode::InvalidArgument, "no IME composition is active");
    }
    return compositionChange(ByteRange{composition->start, composition->start + composition->length}, preedit);
}

Result<ChangeSet> Document::cancelComposition() noexcept {
    const std::optional<detail::Composition>& composition = m_undo->composition;
    if (!composition) {
        return fail(ErrorCode::InvalidArgument, "no IME composition is active");
    }
    Result<ChangeSet> out =
        compositionChange(ByteRange{composition->start, composition->start + composition->length}, {});
    FROST_ASSERT(out.has_value()); // Removing text never fails checks 4 and 5.
    m_undo->composition.reset();
    return out;
}

Result<ChangeSet> Document::commitComposition(std::string_view text, const EditContext& context,
                                              std::optional<ByteRange> replaceInBase) noexcept {
    UndoTree& u = *m_undo;
    if (!u.composition) {
        return fail(ErrorCode::InvalidArgument, "no IME composition is active");
    }
    const detail::Composition& composition = *u.composition;
    const ByteRange net = replaceInBase.value_or(ByteRange{composition.start, composition.start});
    if (replaceInBase) {
        const Snapshot& base = composition.base;
        if (net.begin > net.end) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("the replaced range [{}, {}) begins after its end", net.begin, net.end));
        }
        if (net.end > base.size()) {
            return fail(
                ErrorCode::OutOfRange,
                std::format("the replaced range ends at {}, beyond the base text's {} bytes", net.end, base.size()));
        }
        if (net.begin > composition.start || composition.start > net.end) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("the replaced range [{}, {}) does not contain the composition start {}", net.begin,
                                    net.end, composition.start));
        }
        for (const ByteOffset bound : {net.begin, net.end}) {
            if (!base.isCharBoundary(bound)) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("byte {} is inside a UTF-8 sequence of the base text", bound));
            }
        }
    }
    if (text.empty() && net.empty()) {
        return cancelComposition(); // Section 3.6: an empty commit is a cancel.
    }
    const ByteRange shown{net.begin, net.end + composition.length};
    const std::uint64_t baseVersion = m_buffer.version();
    Result<TextChange> change = m_buffer.replace(shown, text);
    if (!change) {
        return std::unexpected(std::move(change).error()); // The composition stays active.
    }
    ChangeSet out;
    out.origin = ChangeOrigin::Edit;
    out.baseVersion = baseVersion;
    out.version = m_buffer.version();
    out.edits.push_back(RawEdit{shown, text.size()});
    out.changes.push_back(*change);
    out.selections = context.selectionsAfter;
    FROST_ASSERT(!out.edits.front().isNoOp() && out.version == baseVersion + 1);
    m_anchors = m_anchors.transformed(out.edits, out.version);
    // Record the net step against the base (decision 9); the composition ends first, so the
    // committed text is the current text again.
    detail::Composition done = std::move(*u.composition);
    u.composition.reset();
    FROST_PRE(context.selectionsBefore.maxOffset() <= done.base.size() &&
              context.selectionsAfter.maxOffset() <= m_buffer.current().size());
    const RawEdit netStep{net, text.size()};
    recordChange(done.base.pieceTree(), std::span<const RawEdit>(&netStep, 1), context, EditKind::Typing,
                 done.baseAnchors);
    return out;
}

std::optional<ByteRange> Document::compositionRange() const noexcept {
    const std::optional<detail::Composition>& composition = m_undo->composition;
    if (!composition) {
        return std::nullopt;
    }
    return ByteRange{composition->start, composition->start + composition->length};
}

Result<void> Document::checkInvariants() const noexcept {
    const UndoTree& u = *m_undo;
    // U1.
    if (Result<void> checked = m_buffer.checkInvariants(); !checked) {
        return fail(ErrorCode::Internal, "U1: " + checked.error().message());
    }
    // U2.
    const Snapshot& text = m_buffer.current();
    if (Result<void> checked = m_anchors.checkInvariants(); !checked) {
        return fail(ErrorCode::Internal, "U2: " + checked.error().message());
    }
    if (m_anchors.version() != m_buffer.version() || m_anchors.textSize() != text.size()) {
        return fail(ErrorCode::Internal,
                    std::format("U2: the registry is at version {} over {} bytes, the text at version {} with {}",
                                m_anchors.version(), m_anchors.textSize(), m_buffer.version(), text.size()));
    }
    for (const ResolvedAnchor& anchor : m_anchors.all()) {
        if (!text.isCharBoundary(anchor.offset)) {
            return fail(ErrorCode::Internal,
                        std::format("U2: anchor {} at {} is inside a UTF-8 sequence", anchor.id.value, anchor.offset));
        }
    }
    // U3.
    if (Result<void> checked = u.checkShape(m_options.maxTransactions); !checked) {
        return checked;
    }
    // U4: sizes of every node's text, derived from the committed size along the current path.
    const Snapshot committed = committedText();
    std::uint64_t rootSize = committed.size().value();
    for (std::uint64_t at = u.current; at != u.root; at = u.node(at).parent) {
        const std::int64_t delta = sizeDelta(u.node(at));
        rootSize =
            delta >= 0 ? rootSize - static_cast<std::uint64_t>(delta) : rootSize + static_cast<std::uint64_t>(-delta);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pending{{u.root, rootSize}};
    while (!pending.empty()) {
        const auto [at, size] = pending.back();
        pending.pop_back();
        for (const std::uint64_t childId : u.node(at).children) {
            const UndoNode& child = u.node(childId);
            const std::int64_t delta = sizeDelta(child);
            const std::uint64_t childSize =
                delta >= 0 ? size + static_cast<std::uint64_t>(delta) : size - static_cast<std::uint64_t>(-delta);
            if (Result<void> checked = detail::checkRegions(child.regions, size, childSize); !checked) {
                return fail(ErrorCode::Internal, std::format("node {}: {}", childId, checked.error().message()));
            }
            if (child.anchorsBefore.textSize().value() != size ||
                (child.hasAnchorsAfter && child.anchorsAfter.textSize().value() != childSize)) {
                return fail(ErrorCode::Internal,
                            std::format("U4: the anchor sets of node {} do not match its texts of {} and {} bytes",
                                        childId, size, childSize));
            }
            pending.emplace_back(childId, childSize);
        }
    }
    // U5. While the compose base is held (U8), the removed slices are X's bytes as well.
    if (u.current != u.root) {
        const UndoNode& current = u.currentNode();
        const bool baseHeld = u.composeBaseNode == u.current;
        const std::int64_t delta = sizeDelta(current);
        const std::uint64_t committedSize = committed.size().value();
        const std::uint64_t xSize = delta >= 0 ? committedSize - static_cast<std::uint64_t>(delta)
                                               : committedSize + static_cast<std::uint64_t>(-delta);
        if (baseHeld && u.composeBase.summary().bytes != xSize) {
            return fail(ErrorCode::Internal, std::format("U5: the compose base has {} bytes, the parent text {}",
                                                         u.composeBase.summary().bytes, xSize));
        }
        for (const detail::Region& r : current.regions) {
            if (sliceText(r.inserted) != committed.text(r.after)) {
                return fail(ErrorCode::Internal,
                            std::format("U5: the inserted slice of [{}, {}) differs from the committed text",
                                        r.after.begin, r.after.end));
            }
            if (baseHeld &&
                sliceText(r.removed) != treeText(u.composeBase, r.before.begin.value(), r.before.end.value())) {
                return fail(ErrorCode::Internal,
                            std::format("U5: the removed slice of [{}, {}) differs from the compose base",
                                        r.before.begin, r.before.end));
            }
        }
    }
    // U6.
    if (u.composition) {
        const detail::Composition& c = *u.composition;
        const Snapshot& base = c.base;
        const ByteOffset end = c.start + c.length;
        if (end > text.size() || !base.sharesStorageWith(text) || text.size() != base.size() + c.length ||
            text.text(ByteRange{ByteOffset{0}, c.start}) != base.text(ByteRange{ByteOffset{0}, c.start}) ||
            text.text(ByteRange{end, text.size()}) != base.text(ByteRange{c.start, base.size()})) {
            return fail(ErrorCode::Internal,
                        std::format("U6: the text is not the base with the preedit [{}, {}) inserted", c.start, end));
        }
    }
    // U7.
    if (u.groupDepth > 0 && u.composition) {
        return fail(ErrorCode::Internal, "U7: a group and a composition are both active");
    }
    if (u.groupNode != kNoNode && (u.groupDepth == 0 || u.groupNode != u.current)) {
        return fail(ErrorCode::Internal, std::format("U7: group node {} is not the current node {} of an open group",
                                                     u.groupNode, u.current));
    }
    // U8.
    std::vector<std::uint64_t> walk{u.root};
    while (!walk.empty()) {
        const std::uint64_t at = walk.back();
        walk.pop_back();
        const UndoNode& n = u.node(at);
        if (n.open && (at != u.current || at == u.root || !coalescable(n.kind) || u.groupDepth > 0)) {
            return fail(ErrorCode::Internal, std::format("U8: node {} is open", at));
        }
        walk.insert(walk.end(), n.children.begin(), n.children.end());
    }
    // U8 (the compose base): held, for the current node, exactly while that node accepts edits.
    const bool accepting =
        u.current != u.root && (u.currentNode().open || (u.groupNode != kNoNode && u.groupNode == u.current));
    if (accepting != (u.composeBaseNode != kNoNode) ||
        (u.composeBaseNode != kNoNode && u.composeBaseNode != u.current)) {
        return fail(ErrorCode::Internal,
                    std::format("U8: the compose base belongs to node {}, the current node {} {}", u.composeBaseNode,
                                u.current, accepting ? "accepts edits" : "accepts none"));
    }
    if (u.composeBaseNode == kNoNode && !u.composeBase.empty()) {
        return fail(ErrorCode::Internal, "U8: a dropped compose base still holds pieces");
    }
    return {};
}

} // namespace frost::text
