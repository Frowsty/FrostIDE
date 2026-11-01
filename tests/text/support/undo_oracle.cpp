// SPDX-License-Identifier: Apache-2.0
//
// UndoOracle (undo_oracle.hpp): the naive std::string model of Document, reimplemented from the
// slice spec's section 3 without any of the library's undo, anchor or selection code.

#include "text/support/undo_oracle.hpp"

#include "frost/core/assert.hpp"
#include "frost/text/anchor_set.hpp"

#include <algorithm>
#include <numeric>
#include <tuple>
#include <utility>

namespace frost::text::test {
namespace {

bool coalescable(EditKind kind) {
    return kind == EditKind::Typing || kind == EditKind::DeleteBackward || kind == EditKind::DeleteForward;
}

// Applies one raw step to an alignment vector: [a, b) goes, n unaligned bytes come in at a.
void applyOrigin(std::vector<std::int32_t>& origin, std::uint64_t a, std::uint64_t b, std::uint64_t n) {
    const auto first = origin.begin() + static_cast<std::ptrdiff_t>(a);
    origin.erase(first, origin.begin() + static_cast<std::ptrdiff_t>(b));
    origin.insert(origin.begin() + static_cast<std::ptrdiff_t>(a), static_cast<std::size_t>(n), -1);
}

std::vector<std::int32_t> identityOrigin(std::uint64_t size) {
    std::vector<std::int32_t> origin(static_cast<std::size_t>(size));
    std::iota(origin.begin(), origin.end(), 0);
    return origin;
}

} // namespace

OracleSelections oracleNormalise(std::span<const OracleSelection> input, std::size_t primary,
                                 std::vector<std::uint32_t>* origins) {
    if (origins != nullptr) {
        origins->clear();
    }
    OracleSelections out;
    if (input.empty()) {
        return out;
    }
    FROST_ASSERT(primary < input.size());
    // Sweep ranks: the position of each input in (lo, hi, index) order.
    std::vector<std::uint32_t> order(input.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](std::uint32_t x, std::uint32_t y) {
        return std::tuple(input[x].lo(), input[x].hi(), x) < std::tuple(input[y].lo(), input[y].hi(), y);
    });
    std::vector<std::uint32_t> rank(input.size());
    for (std::uint32_t k = 0; k < order.size(); ++k) {
        rank[order[k]] = k;
    }
    struct Group {
        std::uint64_t start;
        std::uint64_t end;
        std::uint32_t firstRank;     // The smallest member rank.
        std::uint32_t origin;        // The input index of that member.
        std::uint32_t directionRank; // The smallest rank of a non-empty member (or ~0u).
        bool reversed;               // That member's direction.
        bool primary;
    };
    std::vector<Group> groups;
    for (std::uint32_t i = 0; i < input.size(); ++i) {
        const OracleSelection& s = input[i];
        groups.push_back(Group{s.lo(), s.hi(), rank[i], i, s.empty() ? ~0u : rank[i], s.reversed(), i == primary});
    }
    const auto mergeable = [](const Group& g, const Group& h) {
        return h.start < g.end || (h.start == g.end && (g.start == g.end || h.start == h.end));
    };
    for (;;) {
        std::sort(groups.begin(), groups.end(), [](const Group& x, const Group& y) {
            return std::tuple(x.start, x.end, x.firstRank) < std::tuple(y.start, y.end, y.firstRank);
        });
        std::size_t k = 0;
        while (k + 1 < groups.size() && !mergeable(groups[k], groups[k + 1])) {
            ++k;
        }
        if (k + 1 >= groups.size()) {
            break;
        }
        Group& g = groups[k];
        const Group h = groups[k + 1];
        g.start = std::min(g.start, h.start);
        g.end = std::max(g.end, h.end);
        if (h.firstRank < g.firstRank) {
            g.firstRank = h.firstRank;
            g.origin = h.origin;
        }
        if (h.directionRank < g.directionRank) {
            g.directionRank = h.directionRank;
            g.reversed = h.reversed;
        }
        g.primary = g.primary || h.primary;
        groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(k) + 1);
    }
    for (const Group& g : groups) {
        if (g.primary) {
            out.primary = out.items.size();
        }
        if (g.start == g.end) {
            out.items.push_back(OracleSelection{g.start, g.start});
        } else {
            out.items.push_back(g.reversed ? OracleSelection{g.end, g.start} : OracleSelection{g.start, g.end});
        }
        if (origins != nullptr) {
            origins->push_back(g.origin);
        }
    }
    return out;
}

std::uint64_t oracleMapOffset(std::uint64_t x, Bias bias, std::uint64_t a, std::uint64_t b, std::uint64_t n) noexcept {
    if (x < a) {
        return x;
    }
    if (x > b) {
        return x - (b - a) + n;
    }
    return bias == Bias::Left ? a : a + n;
}

std::uint64_t oracleMapBatch(std::uint64_t x, Bias bias, std::span<const RawEdit> batch) noexcept {
    for (std::size_t i = batch.size(); i-- > 0;) {
        x = oracleMapOffset(x, bias, batch[i].range.begin.value(), batch[i].range.end.value(), batch[i].newBytes);
    }
    return x;
}

OracleSelections oracleTransform(const OracleSelections& selections, std::span<const RawEdit> batch,
                                 std::vector<std::uint32_t>* origins) {
    std::vector<OracleSelection> mapped;
    for (const OracleSelection& s : selections.items) {
        if (s.empty()) {
            const std::uint64_t x = oracleMapBatch(s.head, Bias::Right, batch);
            mapped.push_back(OracleSelection{x, x});
            continue;
        }
        const std::uint64_t lo = oracleMapBatch(s.lo(), Bias::Left, batch);
        const std::uint64_t hi = oracleMapBatch(s.hi(), Bias::Right, batch);
        mapped.push_back(s.reversed() ? OracleSelection{hi, lo} : OracleSelection{lo, hi});
    }
    if (mapped.empty()) {
        if (origins != nullptr) {
            origins->clear();
        }
        return {};
    }
    return oracleNormalise(mapped, selections.primary, origins);
}

std::vector<OracleRegion> oracleRegions(std::span<const std::int32_t> origin, std::uint64_t xSize) {
    std::vector<OracleRegion> out;
    // The previous kept byte, as (x + 1, y + 1): the gaps start there.
    std::uint64_t gapX = 0;
    std::uint64_t gapY = 0;
    const std::int32_t* data = origin.data();
    const std::uint64_t ySize = origin.size();
    for (std::uint64_t y = 0; y <= ySize; ++y) {
        std::uint64_t x = xSize; // The sentinel kept byte after the end.
        if (y < ySize) {
            if (data[y] < 0) {
                continue;
            }
            x = static_cast<std::uint64_t>(data[y]);
        }
        if (x > gapX || y > gapY) {
            out.push_back(OracleRegion{gapX, x, gapY, y});
        }
        gapX = x + 1;
        gapY = y + 1;
    }
    return out;
}

UndoOracle::UndoOracle(std::string text, std::uint64_t maxBufferBytes, std::chrono::nanoseconds coalesceWindow,
                       std::uint32_t maxTransactions)
    : m_maxBufferBytes(maxBufferBytes), m_window(coalesceWindow), m_maxTransactions(maxTransactions),
      m_textOracle(text) {
    Node root;
    root.alive = true;
    root.text = std::move(text);
    m_nodes.push_back(std::move(root));
}

const std::string& UndoOracle::committedText() const noexcept {
    return m_composition ? m_composition->base : m_textOracle.text();
}

UndoOracle::Node& UndoOracle::node(std::uint64_t id) {
    FROST_ASSERT(id < m_nodes.size() && m_nodes[id].alive);
    return m_nodes[static_cast<std::size_t>(id)];
}

const UndoOracle::Node& UndoOracle::node(std::uint64_t id) const {
    FROST_ASSERT(id < m_nodes.size() && m_nodes[id].alive);
    return m_nodes[static_cast<std::size_t>(id)];
}

OracleUndoState UndoOracle::undoState() const {
    const Node& current = node(m_current);
    OracleUndoState state;
    state.canUndo = m_current != m_root;
    state.canRedo = current.redoChild.has_value();
    state.redoBranches = static_cast<std::uint32_t>(current.children.size());
    state.transactions = static_cast<std::uint32_t>(m_nodeCount - 1);
    state.groupOpen = m_groupDepth > 0;
    state.composing = m_composition.has_value();
    state.stateId = current.stateId;
    return state;
}

std::optional<OracleAnchor> UndoOracle::anchor(std::uint64_t id) const {
    for (const OracleAnchor& a : m_anchors) {
        if (a.id == id) {
            return a;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> UndoOracle::compositionRange() const {
    if (!m_composition) {
        return std::nullopt;
    }
    return std::pair(m_composition->start, m_composition->start + m_composition->length);
}

std::uint32_t UndoOracle::currentDepth() const {
    std::uint32_t depth = 0;
    for (std::uint64_t at = m_current; at != m_root; at = node(at).parent) {
        ++depth;
    }
    return depth;
}

std::vector<std::uint64_t> UndoOracle::redoChain() const {
    std::vector<std::uint64_t> chain;
    for (std::optional<std::uint64_t> at = node(m_current).redoChild; at; at = node(*at).redoChild) {
        chain.push_back(*at);
    }
    return chain;
}

std::vector<OracleRegion> UndoOracle::regionsOf(const Node& n) const {
    return n.extendable ? oracleRegions(n.origin, node(n.parent).text.size()) : n.regions;
}

std::vector<OracleRegion> UndoOracle::currentRegions() const {
    return m_current == m_root ? std::vector<OracleRegion>() : regionsOf(node(m_current));
}

void UndoOracle::seal(Node& n) {
    if (n.extendable) {
        n.regions = oracleRegions(n.origin, node(n.parent).text.size());
        n.origin.clear();
        n.origin.shrink_to_fit();
        n.extendable = false;
    }
    n.open = false;
}

void UndoOracle::sealCurrent() { seal(node(m_current)); }

OracleResult<std::uint64_t> UndoOracle::createAnchor(std::uint64_t offset, Bias bias) {
    if (offset > text().size()) {
        return std::unexpected(ErrorCode::OutOfRange);
    }
    if (!m_textOracle.isCharBoundary(offset)) {
        return std::unexpected(ErrorCode::InvalidArgument);
    }
    if (m_anchors.size() >= AnchorSet::kMaxAnchors) {
        return std::unexpected(ErrorCode::OutOfRange);
    }
    const std::uint64_t id = m_nextAnchorId++;
    m_anchors.push_back(OracleAnchor{id, offset, bias});
    return id;
}

std::optional<ErrorCode> UndoOracle::releaseAnchor(std::uint64_t id) {
    const auto found =
        std::find_if(m_anchors.begin(), m_anchors.end(), [id](const OracleAnchor& a) { return a.id == id; });
    if (found == m_anchors.end()) {
        return ErrorCode::NotFound;
    }
    m_anchors.erase(found);
    return std::nullopt;
}

std::vector<ByteChange> UndoOracle::apply(std::span<const RawEdit> raw, std::span<const std::string> inserted) {
    std::vector<ByteChange> changes;
    for (std::size_t i = raw.size(); i-- > 0;) {
        const std::uint64_t a = raw[i].range.begin.value();
        const std::uint64_t b = raw[i].range.end.value();
        if (a == b && inserted[i].empty()) {
            continue;
        }
        changes.push_back(expectedChange(m_textOracle.text(), a, b, inserted[i]));
        m_textOracle.replace(a, b, inserted[i]);
        for (OracleAnchor& anchor : m_anchors) {
            anchor.offset = oracleMapOffset(anchor.offset, anchor.bias, a, b, inserted[i].size());
        }
    }
    if (!changes.empty()) {
        ++m_version;
    }
    return changes;
}

std::uint64_t UndoOracle::createChild(std::uint64_t parent) {
    const std::uint64_t id = m_nodes.size();
    Node child;
    child.alive = true;
    child.parent = parent;
    child.sequence = m_nextSequence++;
    m_nodes.push_back(std::move(child));
    Node& p = node(parent);
    p.children.push_back(id);
    p.redoChild = id;
    ++m_nodeCount;
    return id;
}

void UndoOracle::eraseSubtree(std::uint64_t id) {
    std::vector<std::uint64_t> pending{id};
    while (!pending.empty()) {
        const std::uint64_t at = pending.back();
        pending.pop_back();
        Node& n = node(at);
        pending.insert(pending.end(), n.children.begin(), n.children.end());
        n = Node{};
        --m_nodeCount;
    }
}

void UndoOracle::prune() {
    while (m_nodeCount - 1 > m_maxTransactions) {
        Node& root = node(m_root);
        if (root.children.size() >= 2) {
            std::uint64_t onPath = m_current;
            while (node(onPath).parent != m_root) {
                onPath = node(onPath).parent;
            }
            std::optional<std::uint64_t> victim;
            for (const std::uint64_t c : root.children) {
                if (c != onPath && (!victim || node(c).sequence < node(*victim).sequence)) {
                    victim = c;
                }
            }
            std::erase(root.children, *victim);
            eraseSubtree(*victim);
            if (root.redoChild == victim) {
                std::optional<std::uint64_t> newest;
                for (const std::uint64_t c : root.children) {
                    if (!newest || node(c).sequence > node(*newest).sequence) {
                        newest = c;
                    }
                }
                root.redoChild = newest;
            }
            continue;
        }
        const std::uint64_t child = root.children.front();
        FROST_ASSERT(child != m_current);
        root = Node{};
        --m_nodeCount;
        Node& newRoot = node(child);
        newRoot.parent = 0;
        newRoot.kind = EditKind::Other;
        newRoot.selectionsBefore = {};
        newRoot.selectionsAfter = {};
        newRoot.anchorsBefore.clear();
        newRoot.anchorsAfter.clear();
        newRoot.lastEdit = std::chrono::nanoseconds{0};
        newRoot.open = false;
        newRoot.extendable = false;
        newRoot.origin.clear();
        newRoot.regions.clear();
        m_root = child;
    }
}

void UndoOracle::record(const std::string& beforeText, std::span<const RawEdit> raw, EditKind kind,
                        const OracleSelections& before, const OracleSelections& after,
                        const std::vector<OracleAnchor>& anchorsBefore, std::chrono::nanoseconds now) {
    const auto extend = [&](Node& n) {
        for (std::size_t i = raw.size(); i-- > 0;) {
            applyOrigin(n.origin, raw[i].range.begin.value(), raw[i].range.end.value(), raw[i].newBytes);
        }
        n.text = committedText();
        n.lastEdit = now;
        n.stateId = ++m_stateCounter;
    };
    if (m_groupDepth > 0) {
        if (!m_groupNode) {
            m_groupPreviousRedo = node(m_current).redoChild;
            const std::uint64_t id = createChild(m_current);
            Node& created = node(id);
            created.kind = m_groupKind;
            created.selectionsBefore = m_groupBefore;
            created.anchorsBefore = anchorsBefore;
            created.extendable = true;
            created.origin = identityOrigin(beforeText.size());
            m_current = id;
            m_groupNode = id;
            prune();
        }
        extend(node(m_current));
        return;
    }
    Node& current = node(m_current);
    if (current.open && coalescable(kind) && kind == current.kind && now - current.lastEdit <= m_window &&
        before == current.selectionsAfter) {
        extend(current);
        current.selectionsAfter = after;
        return;
    }
    sealCurrent();
    const std::uint64_t id = createChild(m_current);
    Node& created = node(id);
    created.kind = kind;
    created.selectionsBefore = before;
    created.selectionsAfter = after;
    created.anchorsBefore = anchorsBefore;
    created.extendable = true;
    created.origin = identityOrigin(beforeText.size());
    extend(created);
    created.open = coalescable(kind);
    m_current = id;
    if (!created.open) {
        seal(created);
    }
    prune();
}

OracleResult<OracleChangeSet> UndoOracle::edit(std::span<const OracleEdit> edits, EditKind kind,
                                               const OracleSelections& before, const OracleSelections& after,
                                               std::chrono::nanoseconds now) {
    if (m_composition) {
        return std::unexpected(ErrorCode::Busy);
    }
    std::uint64_t total = text().size();
    for (std::size_t i = 0; i < edits.size(); ++i) {
        if (const std::optional<ErrorCode> error = m_textOracle.checkEdit(edits[i].a, edits[i].b, edits[i].text)) {
            return std::unexpected(*error);
        }
        if (i > 0 && edits[i - 1].b > edits[i].a) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        total = total - (edits[i].b - edits[i].a) + edits[i].text.size();
    }
    if (total > m_maxBufferBytes) {
        return std::unexpected(ErrorCode::OutOfRange);
    }
    OracleChangeSet out;
    out.origin = ChangeOrigin::Edit;
    out.baseVersion = m_version;
    out.selections = after;
    for (const OracleEdit& e : edits) {
        if (e.a != e.b || !e.text.empty()) {
            out.edits.push_back(RawEdit{ByteRange{ByteOffset{e.a}, ByteOffset{e.b}}, e.text.size()});
            out.inserted.push_back(e.text);
        }
    }
    if (out.edits.empty()) {
        out.version = m_version;
        return out;
    }
    const std::string beforeText = text();
    const std::vector<OracleAnchor> anchorsBefore = m_anchors;
    out.changes = apply(out.edits, out.inserted);
    out.version = m_version;
    record(beforeText, out.edits, kind, before, after, anchorsBefore, now);
    return out;
}

void UndoOracle::breakCoalescing() {
    // A group node is never open, and it keeps receiving the group's edits.
    if (m_groupDepth == 0) {
        sealCurrent();
    }
}

std::optional<ErrorCode> UndoOracle::beginGroup(EditKind kind, const OracleSelections& before) {
    if (m_composition) {
        return ErrorCode::Busy;
    }
    if (m_groupDepth == 0) {
        sealCurrent();
        m_groupKind = kind;
        m_groupBefore = before;
        m_groupNode.reset();
        m_groupPreviousRedo.reset();
    }
    ++m_groupDepth;
    return std::nullopt;
}

std::optional<ErrorCode> UndoOracle::endGroup(const OracleSelections& after) {
    if (m_groupDepth == 0) {
        return ErrorCode::InvalidArgument;
    }
    if (--m_groupDepth > 0) {
        return std::nullopt;
    }
    if (m_groupNode) {
        node(*m_groupNode).selectionsAfter = after;
    }
    sealCurrent();
    m_groupNode.reset();
    m_groupBefore = {};
    m_groupPreviousRedo.reset();
    return std::nullopt;
}

void UndoOracle::restoreAnchors(const std::vector<OracleAnchor>& from) {
    for (OracleAnchor& anchor : m_anchors) {
        for (const OracleAnchor& old : from) {
            if (old.id == anchor.id) {
                anchor.offset = old.offset;
                break;
            }
        }
    }
}

OracleChangeSet UndoOracle::undoNode(std::uint64_t id, ChangeOrigin origin) {
    seal(node(id));
    Node& n = node(id);
    n.anchorsAfter = m_anchors;
    const std::string x = node(n.parent).text;
    OracleChangeSet out;
    out.origin = origin;
    out.baseVersion = m_version;
    out.selections = n.selectionsBefore;
    for (const OracleRegion& r : n.regions) {
        out.edits.push_back(
            RawEdit{ByteRange{ByteOffset{r.afterBegin}, ByteOffset{r.afterEnd}}, r.beforeEnd - r.beforeBegin});
        out.inserted.push_back(
            x.substr(static_cast<std::size_t>(r.beforeBegin), static_cast<std::size_t>(r.beforeEnd - r.beforeBegin)));
    }
    out.changes = apply(out.edits, out.inserted);
    out.version = m_version;
    FROST_ASSERT(text() == x);
    restoreAnchors(n.anchorsBefore);
    m_current = n.parent;
    node(n.parent).redoChild = id;
    return out;
}

OracleResult<OracleChangeSet> UndoOracle::undo() {
    if (m_composition || m_groupDepth > 0) {
        return std::unexpected(ErrorCode::Busy);
    }
    if (m_current == m_root) {
        return std::unexpected(ErrorCode::NotFound);
    }
    return undoNode(m_current, ChangeOrigin::Undo);
}

OracleResult<OracleChangeSet> UndoOracle::redo(std::optional<std::uint32_t> branch) {
    if (m_composition || m_groupDepth > 0) {
        return std::unexpected(ErrorCode::Busy);
    }
    Node& p = node(m_current);
    std::uint64_t id = 0;
    if (branch) {
        if (*branch >= p.children.size()) {
            return std::unexpected(ErrorCode::OutOfRange);
        }
        id = p.children[*branch];
    } else {
        if (!p.redoChild) {
            return std::unexpected(ErrorCode::NotFound);
        }
        id = *p.redoChild;
    }
    sealCurrent();
    Node& c = node(id);
    c.anchorsBefore = m_anchors;
    OracleChangeSet out;
    out.origin = ChangeOrigin::Redo;
    out.baseVersion = m_version;
    out.selections = c.selectionsAfter;
    for (const OracleRegion& r : c.regions) {
        out.edits.push_back(
            RawEdit{ByteRange{ByteOffset{r.beforeBegin}, ByteOffset{r.beforeEnd}}, r.afterEnd - r.afterBegin});
        out.inserted.push_back(
            c.text.substr(static_cast<std::size_t>(r.afterBegin), static_cast<std::size_t>(r.afterEnd - r.afterBegin)));
    }
    out.changes = apply(out.edits, out.inserted);
    out.version = m_version;
    FROST_ASSERT(text() == c.text);
    restoreAnchors(c.anchorsAfter);
    node(m_current).redoChild = id;
    m_current = id;
    return out;
}

OracleResult<OracleChangeSet> UndoOracle::rollbackGroup() {
    if (m_groupDepth == 0) {
        return std::unexpected(ErrorCode::InvalidArgument);
    }
    OracleChangeSet out;
    if (!m_groupNode) {
        out.origin = ChangeOrigin::Rollback;
        out.baseVersion = m_version;
        out.version = m_version;
    } else {
        const std::uint64_t id = *m_groupNode;
        out = undoNode(id, ChangeOrigin::Rollback);
        const std::uint64_t parent = node(id).parent;
        node(id) = Node{};
        --m_nodeCount;
        Node& p = node(parent);
        std::erase(p.children, id);
        if (m_groupPreviousRedo && *m_groupPreviousRedo < m_nodes.size() && m_nodes[*m_groupPreviousRedo].alive &&
            m_nodes[*m_groupPreviousRedo].parent == parent) {
            p.redoChild = m_groupPreviousRedo;
        } else {
            p.redoChild.reset();
            for (const std::uint64_t c : p.children) {
                if (!p.redoChild || node(c).sequence > node(*p.redoChild).sequence) {
                    p.redoChild = c;
                }
            }
        }
    }
    out.selections = m_groupBefore;
    m_groupDepth = 0;
    m_groupNode.reset();
    m_groupBefore = {};
    m_groupPreviousRedo.reset();
    return out;
}

std::optional<ErrorCode> UndoOracle::beginComposition(std::uint64_t at) {
    if (m_composition || m_groupDepth > 0) {
        return ErrorCode::Busy;
    }
    if (at > text().size()) {
        return ErrorCode::OutOfRange;
    }
    if (!m_textOracle.isCharBoundary(at)) {
        return ErrorCode::InvalidArgument;
    }
    m_composition = Composition{at, 0, text(), m_anchors};
    return std::nullopt;
}

OracleResult<OracleChangeSet> UndoOracle::compositionChange(std::string_view preedit) {
    Composition& c = *m_composition;
    if (!oracleValid(preedit)) {
        return std::unexpected(ErrorCode::InvalidArgument);
    }
    if (text().size() - c.length + preedit.size() > m_maxBufferBytes) {
        return std::unexpected(ErrorCode::OutOfRange);
    }
    OracleChangeSet out;
    out.origin = ChangeOrigin::Composition;
    out.baseVersion = m_version;
    if (c.length != 0 || !preedit.empty()) {
        out.edits.push_back(RawEdit{ByteRange{ByteOffset{c.start}, ByteOffset{c.start + c.length}}, preedit.size()});
        out.inserted.emplace_back(preedit);
        out.changes = apply(out.edits, out.inserted);
    }
    out.version = m_version;
    c.length = preedit.size();
    return out;
}

OracleResult<OracleChangeSet> UndoOracle::updateComposition(std::string_view preedit) {
    if (!m_composition) {
        return std::unexpected(ErrorCode::InvalidArgument);
    }
    return compositionChange(preedit);
}

OracleResult<OracleChangeSet> UndoOracle::cancelComposition() {
    if (!m_composition) {
        return std::unexpected(ErrorCode::InvalidArgument);
    }
    OracleResult<OracleChangeSet> out = compositionChange({});
    m_composition.reset();
    return out;
}

OracleResult<OracleChangeSet>
UndoOracle::commitComposition(std::string_view committed, const OracleSelections& before, const OracleSelections& after,
                              std::optional<std::pair<std::uint64_t, std::uint64_t>> replaceInBase,
                              std::chrono::nanoseconds now) {
    if (!m_composition) {
        return std::unexpected(ErrorCode::InvalidArgument);
    }
    const std::uint64_t start = m_composition->start;
    const std::uint64_t length = m_composition->length;
    const auto [rb, re] = replaceInBase.value_or(std::pair(start, start));
    if (replaceInBase) {
        if (rb > re) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        if (re > m_composition->base.size()) {
            return std::unexpected(ErrorCode::OutOfRange);
        }
        if (rb > start || start > re) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        // A base offset x is x in the current text before the preedit and x + length after it.
        const auto baseBoundary = [&](std::uint64_t x) {
            return m_textOracle.isCharBoundary(x <= start ? x : x + length);
        };
        if (!baseBoundary(rb) || !baseBoundary(re)) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
    }
    if (committed.empty() && rb == re) {
        return cancelComposition();
    }
    if (!oracleValid(committed)) {
        return std::unexpected(ErrorCode::InvalidArgument);
    }
    if (text().size() - (re + length - rb) + committed.size() > m_maxBufferBytes) {
        return std::unexpected(ErrorCode::OutOfRange);
    }
    OracleChangeSet out;
    out.origin = ChangeOrigin::Edit;
    out.baseVersion = m_version;
    out.selections = after;
    out.edits.push_back(RawEdit{ByteRange{ByteOffset{rb}, ByteOffset{re + length}}, committed.size()});
    out.inserted.emplace_back(committed);
    out.changes = apply(out.edits, out.inserted);
    out.version = m_version;
    Composition done = std::move(*m_composition);
    m_composition.reset();
    const RawEdit net{ByteRange{ByteOffset{rb}, ByteOffset{re}}, committed.size()};
    record(done.base, std::span<const RawEdit>(&net, 1), EditKind::Typing, before, after, done.baseAnchors, now);
    return out;
}

} // namespace frost::text::test
