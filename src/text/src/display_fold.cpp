// SPDX-License-Identifier: Apache-2.0
//
// The line patch of a ChangeSet, projected fold ranges and the identity inlay layer.

#include "batch_mapper.hpp"
#include "display_layers.hpp"

#include <cstddef>
#include <limits>
#include <map>
#include <ranges>
#include <set>

namespace frost::text::detail {

namespace {
struct EventPoint {
    std::uint64_t at;
    std::uint64_t other;
    std::uint64_t id;
    bool open;
};

[[nodiscard]] bool eventLess(const EventPoint& a, const EventPoint& b) noexcept {
    if (a.at != b.at)
        return a.at < b.at;
    if (a.open != b.open)
        return !a.open;
    if (a.other != b.other)
        return a.other > b.other;
    return a.open ? a.id < b.id : a.id > b.id;
}

// Split a projection at a buffer boundary; only identity runs may be split internally.
[[nodiscard]] std::pair<FoldRunTree, FoldRunTree> splitRunAt(const FoldRunTree& tree, std::uint64_t at,
                                                             const Snapshot& text) noexcept {
    auto [left, right] = tree.split([at](const FoldRunSummary& sum) { return sum.buffer.bytes > at; });
    const auto begin = left.summary().buffer.bytes;
    if (begin == at)
        return {std::move(left), std::move(right)};
    const FoldRun run = *right.front();
    FROST_ASSERT(run.foldId == 0 && at < begin + run.buffer.bytes);
    const auto first = text.summaryOf({ByteOffset{begin}, ByteOffset{at}});
    const auto second = text.summaryOf({ByteOffset{at}, ByteOffset{begin + run.buffer.bytes}});
    const FoldRun a{first, first, 0};
    const FoldRun b{second, second, 0};
    return {FoldRunTree::concat(left, FoldRunTree::fromItems(std::span(&a, 1))),
            FoldRunTree::concat(FoldRunTree::fromItems(std::span(&b, 1)), right.withoutFront())};
}

[[nodiscard]] FoldRunTree concatIdentityRuns(const FoldRunTree& left, const FoldRunTree& right) noexcept {
    if (left.empty() || right.empty())
        return FoldRunTree::concat(left, right);
    const FoldRun a = *left.back();
    const FoldRun b = *right.front();
    if (a.foldId != 0 || b.foldId != 0)
        return FoldRunTree::concat(left, right);
    const auto summary = text::combine(a.buffer, b.buffer);
    const FoldRun merged{summary, summary, 0};
    return FoldRunTree::concat(FoldRunTree::concat(left.withoutBack(), FoldRunTree::fromItems(std::span(&merged, 1))),
                               right.withoutFront());
}

// Walk a prefix backwards, skipping subtrees whose reverse depth cannot expose an open ancestor.
void collectOpenAncestors(const FoldEventTree::Node* node, std::int64_t& balance,
                          std::vector<std::uint64_t>& ids) noexcept {
    const auto& summary = node->summary;
    if (balance + std::min<std::int64_t>(0, summary.minDepth) - summary.depth >= 0) {
        balance -= summary.depth;
        return;
    }
    if (node->height == 0) {
        const auto* leaf = static_cast<const FoldEventTree::Leaf*>(node);
        for (std::size_t i = leaf->count; i > 0; --i) {
            const auto& event = leaf->items[i - 1];
            if (!event.open)
                ++balance;
            else if (balance != 0)
                --balance;
            else
                ids.push_back(event.id);
        }
        return;
    }
    const auto* branch = static_cast<const FoldEventTree::Internal*>(node);
    for (std::size_t i = branch->count; i > 0; --i)
        collectOpenAncestors(branch->children[i - 1].get(), balance, ids);
}
} // namespace

std::vector<SpanEdit> composeLinePatch(std::span<const TextChange> changes) noexcept {
    // The patch maps the text before the set (old) to the text after the steps so far (new).
    std::vector<SpanEdit> patch;
    for (const TextChange& change : changes) {
        // The change replaces current lines [s, e) by [s, newEnd).
        const std::uint32_t s = change.startPoint.line.value();
        const std::uint32_t e = change.oldEndPoint.line.value() + 1;
        const std::uint32_t newEnd = change.newEndPoint.line.value() + 1;
        const std::int64_t delta = std::int64_t{newEnd} - std::int64_t{e};

        // The entries [i, j) overlap [s, e) in current lines; the ones before i end at or before s.
        std::size_t i = 0;
        while (i < patch.size() && patch[i].newEnd <= s) {
            ++i;
        }
        std::size_t j = i;
        while (j < patch.size() && patch[j].newFirst < e) {
            ++j;
        }
        // A current line outside every entry maps back by the shift after the entries before it.
        const auto toOld = [&patch](std::uint32_t line, std::size_t before) {
            if (before == 0) {
                return line;
            }
            const SpanEdit& last = patch[before - 1];
            return static_cast<std::uint32_t>(std::int64_t{line} -
                                              (std::int64_t{last.newEnd} - std::int64_t{last.oldEnd}));
        };

        SpanEdit merged;
        if (i < j && patch[i].newFirst <= s) {
            merged.oldFirst = patch[i].oldFirst;
            merged.newFirst = patch[i].newFirst;
        } else {
            merged.oldFirst = toOld(s, i);
            merged.newFirst = s;
        }
        std::uint32_t currentEnd = e;
        if (i < j && patch[j - 1].newEnd >= e) {
            merged.oldEnd = patch[j - 1].oldEnd;
            currentEnd = patch[j - 1].newEnd;
        } else {
            merged.oldEnd = toOld(e, j);
        }
        merged.newEnd = static_cast<std::uint32_t>(std::int64_t{currentEnd} + delta);

        for (std::size_t k = j; k < patch.size(); ++k) {
            patch[k].newFirst = static_cast<std::uint32_t>(std::int64_t{patch[k].newFirst} + delta);
            patch[k].newEnd = static_cast<std::uint32_t>(std::int64_t{patch[k].newEnd} + delta);
        }
        patch.erase(patch.begin() + static_cast<std::ptrdiff_t>(i), patch.begin() + static_cast<std::ptrdiff_t>(j));
        patch.insert(patch.begin() + static_cast<std::ptrdiff_t>(i), merged);
    }
    return patch;
}

// --- FoldLayer ----------------------------------------------------------------------------------

FoldLayer::FoldLayer(Snapshot text, std::vector<ByteRange> folds, std::vector<std::uint64_t> ids,
                     std::uint64_t nextId) noexcept
    : m_text(std::move(text)), m_folds(std::make_shared<const std::vector<ByteRange>>(std::move(folds))),
      m_nextFoldId(nextId), m_foldAnchors(m_text.version(), m_text.size()) {
    if (ids.empty()) {
        ids.reserve(m_folds->size());
        for (std::size_t i = 0; i < m_folds->size(); ++i)
            ids.push_back(m_nextFoldId++);
    }
    FROST_ASSERT(ids.size() == m_folds->size());
    std::vector<FoldRecord> records;
    records.reserve(m_folds->size());
    std::vector<FoldRun> runs;
    std::uint64_t previous = 0;
    const auto addIdentity = [&](std::uint64_t begin, std::uint64_t end) {
        if (begin == end)
            return;
        const auto summary = m_text.summaryOf({ByteOffset{begin}, ByteOffset{end}});
        runs.push_back({summary, summary, 0});
    };
    for (std::size_t i = 0; i < m_folds->size(); ++i) {
        const ByteRange range = (*m_folds)[i];
        auto [starts, begin] = m_foldAnchors.withAnchor(range.begin, Bias::Right);
        auto [ends, end] = starts.withAnchor(range.end, Bias::Left);
        m_foldAnchors = std::move(ends);
        records.push_back({ids[i], begin, end});
        if (range.begin.value() < previous)
            continue;
        addIdentity(previous, range.begin.value());
        runs.push_back({m_text.summaryOf(range), summarize("..."), ids[i]});
        previous = range.end.value();
    }
    std::ranges::sort(records, {}, &FoldRecord::id);
    m_foldRecords = FoldRecordTree::fromItems(records);
    addIdentity(previous, m_text.size().value());
    m_runs = FoldRunTree::fromItems(runs);
    std::vector<EventPoint> points;
    points.reserve(m_folds->size() * 2);
    for (std::size_t i = 0; i < m_folds->size(); ++i) {
        const auto range = (*m_folds)[i];
        const auto id = ids[i];
        points.push_back({range.begin.value(), range.end.value(), id, true});
        points.push_back({range.end.value(), range.begin.value(), id, false});
    }
    std::ranges::sort(points, eventLess);
    std::vector<FoldEvent> events;
    events.reserve(points.size());
    std::uint64_t last = 0;
    for (const auto point : points) {
        events.push_back({point.at - last, point.id, point.open});
        last = point.at;
    }
    m_events = FoldEventTree::fromItems(events);
    m_size = m_runs.summary().projected.bytes;
    m_lineCount = m_runs.summary().projected.lineBreaks + 1;
}

const std::vector<ByteRange>& FoldLayer::folds() const noexcept {
    if (!m_folds) {
        ++m_stats.materializedFoldLists;
        auto ranges = std::make_shared<std::vector<ByteRange>>();
        ranges->reserve(static_cast<std::size_t>(m_foldRecords.summary().count));
        for (auto cursor = m_events.first(); const auto* event = cursor.item(); cursor.next()) {
            if (!event->open)
                continue;
            const FoldRecord& item = record(event->id);
            ranges->push_back({*m_foldAnchors.find(item.begin), *m_foldAnchors.find(item.end)});
        }
        m_folds = std::move(ranges);
    }
    return *m_folds;
}

const FoldRecord& FoldLayer::record(std::uint64_t id) const noexcept {
    const auto* found = findRecord(id);
    FROST_ASSERT(found != nullptr);
    return *found;
}

const FoldRecord* FoldLayer::findRecord(std::uint64_t id) const noexcept {
    if (id == 0 || id > m_foldRecords.summary().maxId)
        return nullptr;
    const auto cursor = m_foldRecords.seek([id](const FoldRecordSummary& sum) { return sum.maxId >= id; });
    return cursor.item() && cursor.item()->id == id ? cursor.item() : nullptr;
}

FoldEventTree FoldLayer::changeEventsAt(std::uint64_t at, std::optional<FoldEvent> add,
                                        std::optional<FoldEvent> remove) const noexcept {
    auto [before, tail] = m_events.split([at](const FoldEventSummary& sum) { return sum.bytes >= at; });
    const auto base = before.summary().bytes;
    std::vector<FoldEvent> group;
    std::optional<EventPoint> addedPoint;
    if (add) {
        const auto& item = record(add->id);
        const auto other = *m_foldAnchors.find(add->open ? item.end : item.begin);
        addedPoint = EventPoint{at, other.value(), add->id, add->open};
    }
    std::uint64_t count = 0;
    for (auto cursor = tail.first(); const auto* event = cursor.item(); cursor.next()) {
        if (base + cursor.prefix().bytes + event->gap != at)
            break;
        ++count;
        if (remove && event->id == remove->id && event->open == remove->open)
            continue;
        if (addedPoint) {
            const auto& item = record(event->id);
            const auto other = *m_foldAnchors.find(event->open ? item.end : item.begin);
            if (eventLess(*addedPoint, EventPoint{at, other.value(), event->id, event->open})) {
                group.push_back(*add);
                addedPoint.reset();
            }
        }
        group.push_back(*event);
    }
    if (addedPoint)
        group.push_back(*add);
    auto [discard, after] = tail.split([count](const FoldEventSummary& sum) { return sum.count > count; });
    (void)discard;
    if (!after.empty()) {
        FoldEvent first = *after.front();
        if (count == 0 && !group.empty())
            first.gap -= at - base;
        else if (count != 0 && group.empty())
            first.gap += at - base;
        after = FoldEventTree::concat(FoldEventTree::fromItems(std::span(&first, 1)), after.withoutFront());
    }
    for (auto& event : group)
        event.gap = 0;
    if (!group.empty())
        group.front().gap = at - base;
    return FoldEventTree::concat(FoldEventTree::concat(before, FoldEventTree::fromItems(group)), after);
}

FoldEventTree FoldLayer::removeEventsAt(std::uint64_t at, std::span<const std::uint64_t> ids,
                                        std::uint64_t& scanned) const noexcept {
    auto [before, tail] = m_events.split([at](const FoldEventSummary& sum) { return sum.bytes >= at; });
    const auto base = before.summary().bytes;
    std::vector<FoldEvent> kept;
    std::uint64_t count = 0;
    for (auto cursor = tail.first(); const auto* event = cursor.item(); cursor.next()) {
        if (base + cursor.prefix().bytes + event->gap != at)
            break;
        ++count;
        ++scanned;
        if (!std::ranges::binary_search(ids, event->id))
            kept.push_back(*event);
    }
    auto [discard, after] = tail.split([count](const FoldEventSummary& sum) { return sum.count > count; });
    (void)discard;
    if (!after.empty() && kept.empty() && count != 0) {
        FoldEvent first = *after.front();
        first.gap += at - base;
        after = FoldEventTree::concat(FoldEventTree::fromItems(std::span(&first, 1)), after.withoutFront());
    }
    for (auto& event : kept)
        event.gap = 0;
    if (!kept.empty())
        kept.front().gap = at - base;
    return FoldEventTree::concat(FoldEventTree::concat(before, FoldEventTree::fromItems(kept)), after);
}

std::vector<ByteRange> FoldLayer::containing(ByteOffset offset) const noexcept {
    std::vector<ByteRange> ranges;
    for (const FoldInfo& info : containingInfos(offset))
        ranges.push_back(info.range);
    return ranges;
}

std::vector<FoldInfo> FoldLayer::containingInfos(ByteOffset offset) const noexcept {
    const auto [before, after] = m_events.split([offset](const FoldEventSummary& sum) {
        return sum.bytes > offset.value() || (sum.bytes == offset.value() && sum.lastOpen);
    });
    (void)after;
    std::vector<std::uint64_t> ids;
    std::int64_t balance = 0;
    if (const auto* root = before.rootNode())
        collectOpenAncestors(root, balance, ids);
    std::vector<FoldInfo> ranges;
    ranges.reserve(ids.size());
    for (const auto id : ids) {
        const FoldRecord& item = record(id);
        ranges.push_back({FoldId{id}, {*m_foldAnchors.find(item.begin), *m_foldAnchors.find(item.end)}});
    }
    return ranges;
}

std::vector<FoldInfo> FoldLayer::intersectingInfos(ByteRange range) const noexcept {
    auto result = containingInfos(range.begin);
    if (range.empty())
        return result;
    auto cursor = m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= range.begin.value(); });
    while (const auto* event = cursor.item()) {
        const auto at = cursor.prefix().bytes + event->gap;
        if (at >= range.end.value())
            break;
        if (event->open)
            result.push_back(*info(FoldId{event->id}));
        cursor.next();
    }
    return result;
}

std::vector<FoldInfo> FoldLayer::boundaryInfos(ByteRange range) const noexcept {
    std::vector<FoldInfo> result;
    auto cursor = m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= range.begin.value(); });
    while (const auto* event = cursor.item()) {
        if (cursor.prefix().bytes + event->gap > range.end.value())
            break;
        result.push_back(*info(FoldId{event->id}));
        cursor.next();
    }
    return result;
}

bool FoldLayer::hasStart(ByteRange range) const noexcept {
    const auto first = m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= range.begin.value(); });
    const auto before = first.prefix().opens;
    const auto cursor = m_events.seek([&](const FoldEventSummary& sum) { return sum.opens > before; });
    return cursor.item() && cursor.prefix().bytes + cursor.item()->gap <= range.end.value();
}

bool FoldLayer::hasBoundary(ByteOffset offset, bool start) const noexcept {
    const auto first = m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= offset.value(); });
    const auto before = start ? first.prefix().opens : first.prefix().count - first.prefix().opens;
    const auto cursor = m_events.seek(
        [&](const FoldEventSummary& sum) { return (start ? sum.opens : sum.count - sum.opens) > before; });
    return cursor.item() && cursor.prefix().bytes + cursor.item()->gap == offset.value();
}

std::optional<FoldInfo> FoldLayer::atBoundary(ByteOffset offset, bool start) const noexcept {
    const auto first = m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= offset.value(); });
    const auto before = start ? first.prefix().opens : first.prefix().count - first.prefix().opens;
    const auto cursor = m_events.seek(
        [&](const FoldEventSummary& sum) { return (start ? sum.opens : sum.count - sum.opens) > before; });
    if (!cursor.item() || cursor.prefix().bytes + cursor.item()->gap != offset.value())
        return std::nullopt;
    return *info(FoldId{cursor.item()->id});
}

std::optional<FoldInfo> FoldLayer::firstStarting(ByteRange range) const noexcept {
    const auto first = m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= range.begin.value(); });
    const auto before = first.prefix().opens;
    const auto cursor = m_events.seek([&](const FoldEventSummary& sum) { return sum.opens > before; });
    if (!cursor.item() || cursor.prefix().bytes + cursor.item()->gap > range.end.value())
        return std::nullopt;
    return *info(FoldId{cursor.item()->id});
}

Result<FoldInfo> FoldLayer::info(FoldId id) const noexcept {
    if (!findRecord(id.value))
        return fail(ErrorCode::NotFound, "fold does not exist");
    const auto& item = record(id.value);
    return FoldInfo{id, {*m_foldAnchors.find(item.begin), *m_foldAnchors.find(item.end)}};
}

Result<std::pair<FoldLayer, FoldId>> FoldLayer::adding(ByteRange range, std::optional<FoldId> existing) const noexcept {
    if (range.begin >= range.end || range.end > m_text.size() || !m_text.isCharBoundary(range.begin) ||
        !m_text.isCharBoundary(range.end))
        return fail(ErrorCode::InvalidArgument, "invalid fold range");
    const auto splitsCrLf = [&](ByteOffset p) {
        return p.value() > 0 && p < m_text.size() && m_text.byteAt(p - 1) == '\r' && m_text.byteAt(p) == '\n';
    };
    if (splitsCrLf(range.begin) || splitsCrLf(range.end))
        return fail(ErrorCode::InvalidArgument, "fold splits CRLF");
    if (m_foldAnchors.size() + 2 > AnchorSet::kMaxAnchors)
        return fail(ErrorCode::OutOfRange, "too many fold anchors");
    if (!existing && m_nextFoldId == std::numeric_limits<std::uint64_t>::max())
        return fail(ErrorCode::OutOfRange, "fold identity space exhausted");
    if (existing && (existing->value == 0 || findRecord(existing->value)))
        return fail(ErrorCode::InvalidArgument, "fold identity already exists");
    const auto beginParents = containingInfos(range.begin);
    const auto endParents = containingInfos(range.end);
    std::uint64_t visited = beginParents.size() + endParents.size();
    for (const auto& parent : beginParents)
        if (parent.range.end < range.end)
            return fail(ErrorCode::InvalidArgument, "fold ranges overlap without nesting");
    for (const auto& parent : endParents)
        if (parent.range.begin > range.begin)
            return fail(ErrorCode::InvalidArgument, "fold ranges overlap without nesting");
    auto cursor = m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= range.begin.value(); });
    while (const auto* event = cursor.item()) {
        if (cursor.prefix().bytes + event->gap != range.begin.value())
            break;
        ++visited;
        if (event->open && info(FoldId{event->id})->range == range)
            return fail(ErrorCode::AlreadyExists, "fold range already exists");
        cursor.next();
    }
    const auto visible = m_runs.seek([&](const FoldRunSummary& sum) { return sum.buffer.bytes > range.begin.value(); });
    const bool hidden = visible.item() && visible.item()->foldId != 0 &&
                        range.end.value() <= visible.prefix().buffer.bytes + visible.item()->buffer.bytes;
    if (!hidden && m_size - (toFold(range.end, Bias::Left) - toFold(range.begin, Bias::Right)) + 3 > 0xFFFF'FFFE)
        return fail(ErrorCode::OutOfRange, "folded text exceeds display coordinates");
    FoldLayer next = *this;
    auto [starts, begin] = next.m_foldAnchors.withAnchor(range.begin, Bias::Right);
    auto [ends, end] = starts.withAnchor(range.end, Bias::Left);
    next.m_foldAnchors = std::move(ends);
    const FoldId id = existing.value_or(FoldId{next.m_nextFoldId});
    if (!existing)
        ++next.m_nextFoldId;
    const FoldRecord item{id.value, begin, end};
    auto [beforeId, afterId] =
        next.m_foldRecords.split([id](const FoldRecordSummary& sum) { return sum.maxId >= id.value; });
    next.m_foldRecords = FoldRecordTree::concat(
        FoldRecordTree::concat(beforeId, FoldRecordTree::fromItems(std::span(&item, 1))), afterId);
    next.m_events = next.changeEventsAt(range.begin.value(), FoldEvent{0, id.value, true}, std::nullopt);
    next.m_events = next.changeEventsAt(range.end.value(), FoldEvent{0, id.value, false}, std::nullopt);
    if (!hidden) {
        auto [through, right] = splitRunAt(next.m_runs, range.end.value(), m_text);
        auto [left, covered] = splitRunAt(through, range.begin.value(), m_text);
        (void)covered;
        const FoldRun folded{m_text.summaryOf(range), summarize("..."), id.value};
        next.m_runs =
            FoldRunTree::concat(FoldRunTree::concat(left, FoldRunTree::fromItems(std::span(&folded, 1))), right);
    }
    next.m_folds.reset();
    next.m_size = next.m_runs.summary().projected.bytes;
    next.m_lineCount = next.m_runs.summary().projected.lineBreaks + 1;
    next.m_stats = {hidden ? 0U : 1U, visited, 0};
    return std::pair(std::move(next), id);
}

Result<FoldLayer> FoldLayer::removing(FoldId id) const noexcept {
    if (!findRecord(id.value))
        return fail(ErrorCode::NotFound, "fold does not exist");
    const FoldRecord item = record(id.value);
    const ByteRange range{*m_foldAnchors.find(item.begin), *m_foldAnchors.find(item.end)};
    const auto visible = m_runs.seek([&](const FoldRunSummary& sum) { return sum.buffer.bytes > range.begin.value(); });
    const bool projected = visible.item() && visible.item()->foldId == id.value;
    FoldLayer next = *this;
    std::uint64_t visited = 0;
    if (projected) {
        std::vector<FoldRun> promoted;
        std::uint64_t previous = range.begin.value();
        auto cursor = m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= range.begin.value(); });
        while (cursor.item() && !(cursor.item()->id == id.value && cursor.item()->open)) {
            ++visited;
            cursor.next();
        }
        FROST_ASSERT(cursor.item() != nullptr);
        std::uint64_t index = cursor.prefix().count + 1;
        for (;;) {
            cursor = m_events.seek([index](const FoldEventSummary& sum) { return sum.count > index; });
            FROST_ASSERT(cursor.item() != nullptr);
            ++visited;
            const auto* event = cursor.item();
            if (event->id == id.value && !event->open)
                break;
            FROST_ASSERT(event->open);
            const auto& child = record(event->id);
            const ByteRange childRange{*m_foldAnchors.find(child.begin), *m_foldAnchors.find(child.end)};
            if (previous < childRange.begin.value()) {
                const auto identity = m_text.summaryOf({ByteOffset{previous}, childRange.begin});
                promoted.push_back({identity, identity, 0});
            }
            promoted.push_back({m_text.summaryOf(childRange), summarize("..."), event->id});
            previous = childRange.end.value();
            const auto [ignored, subtree] =
                m_events.split([index](const FoldEventSummary& sum) { return sum.count > index; });
            (void)ignored;
            const auto close = subtree.seek([](const FoldEventSummary& sum) { return sum.minDepth <= 0; });
            FROST_ASSERT(close.item() && !close.item()->open && close.item()->id == event->id);
            index += close.prefix().count + 1;
        }
        if (previous < range.end.value()) {
            const auto identity = m_text.summaryOf({ByteOffset{previous}, range.end});
            promoted.push_back({identity, identity, 0});
        }
        std::uint64_t projectedBytes = m_size - 3;
        for (const auto& run : promoted)
            projectedBytes += run.projected.bytes;
        if (projectedBytes > 0xFFFF'FFFE)
            return fail(ErrorCode::OutOfRange, "unfolded text exceeds display coordinates");
        auto [through, right] = splitRunAt(next.m_runs, range.end.value(), m_text);
        auto [left, covered] = splitRunAt(through, range.begin.value(), m_text);
        (void)covered;
        next.m_runs = concatIdentityRuns(concatIdentityRuns(left, FoldRunTree::fromItems(promoted)), right);
    }
    next.m_events = next.changeEventsAt(range.begin.value(), std::nullopt, FoldEvent{0, id.value, true});
    next.m_events = next.changeEventsAt(range.end.value(), std::nullopt, FoldEvent{0, id.value, false});
    next.m_foldAnchors = *next.m_foldAnchors.without(item.begin);
    next.m_foldAnchors = *next.m_foldAnchors.without(item.end);
    auto [before, tail] = next.m_foldRecords.split([&](const FoldRecordSummary& sum) { return sum.maxId >= id.value; });
    next.m_foldRecords = FoldRecordTree::concat(before, tail.withoutFront());
    next.m_folds.reset();
    next.m_size = next.m_runs.summary().projected.bytes;
    next.m_lineCount = next.m_runs.summary().projected.lineBreaks + 1;
    next.m_stats = {projected ? 1U : 0U, visited, 0};
    return next;
}

bool FoldLayer::intersects(ByteRange range) const noexcept {
    auto cursor = m_runs.seek([range](const FoldRunSummary& sum) { return sum.buffer.bytes > range.begin.value(); });
    if (range.empty()) {
        return cursor.item() != nullptr && cursor.item()->foldId != 0 &&
               cursor.prefix().buffer.bytes < range.begin.value();
    }
    while (const auto* run = cursor.item()) {
        if (cursor.prefix().buffer.bytes >= range.end.value())
            return false;
        if (run->foldId != 0)
            return true;
        cursor.next();
    }
    return false;
}

Result<void> FoldLayer::checkInvariants() const noexcept {
    if (auto checked = m_runs.checkInvariants(); !checked)
        return checked;
    if (auto checked = m_events.checkInvariants(); !checked)
        return checked;
    if (auto checked = m_foldRecords.checkInvariants(); !checked)
        return checked;
    if (m_events.summary().depth != 0 || m_events.summary().minDepth < 0 ||
        m_events.summary().count != 2 * m_foldRecords.summary().count)
        return fail(ErrorCode::Internal, "fold endpoints are not balanced");
    if (m_foldAnchors.version() != m_text.version() || m_foldAnchors.textSize() != m_text.size())
        return fail(ErrorCode::Internal, "fold anchors do not match the text");
    const auto& summary = m_runs.summary();
    if (summary.buffer != m_text.summary() || summary.projected.bytes != m_size ||
        summary.projected.lineBreaks + 1 != m_lineCount) {
        return fail(ErrorCode::Internal, "fold projection summary does not match buffer or rows");
    }
    return {};
}

FoldLine FoldLayer::toFoldLine(const LineInfo& info) noexcept {
    std::uint64_t breakBytes = 0;
    switch (info.lineBreak) {
    case LineBreak::None: breakBytes = 0; break;
    case LineBreak::Lf:
    case LineBreak::Cr: breakBytes = 1; break;
    case LineBreak::CrLf: breakBytes = 2; break;
    }
    return {info.content.begin.value(), info.content.end.value(), info.content.end.value() + breakBytes};
}

FoldLine FoldLayer::line(std::uint32_t line) const noexcept {
    if (m_runs.summary().folds == 0) {
        const auto info = m_text.lineInfo(LineIndex{line});
        FROST_ASSERT(info.has_value());
        return toFoldLine(*info);
    }
    const auto source = sourceLine(line);
    const auto info = m_text.lineInfo(LineIndex{source});
    FROST_ASSERT(info.has_value());
    const auto start = toFold(info->content.begin, Bias::Left);
    if (line + 1 == m_lineCount)
        return {start, m_size, m_size};
    const auto next = m_text.lineInfo(LineIndex{sourceLine(line + 1)});
    FROST_ASSERT(next.has_value());
    const auto end = toFold(next->content.begin, Bias::Left);
    const auto previous = toBuffer(end, Bias::Left).value();
    std::uint64_t breakBytes = 1;
    if (previous >= 2 && m_text.byteAt(ByteOffset{previous - 1}) == '\n' &&
        m_text.byteAt(ByteOffset{previous - 2}) == '\r')
        breakBytes = 2;
    return {start, end - breakBytes, end};
}

std::uint32_t FoldLayer::lineAt(std::uint64_t offset) const noexcept {
    const auto cursor = m_runs.seek([offset](const FoldRunSummary& sum) { return sum.projected.bytes > offset; });
    TextSummary summary = cursor.prefix().projected;
    if (const auto* run = cursor.item()) {
        const auto relative = offset - cursor.prefix().projected.bytes;
        if (run->foldId == 0) {
            const auto start = cursor.prefix().buffer.bytes;
            const auto source = m_text.clip(ByteOffset{start + relative});
            summary = text::combine(summary, m_text.summaryOf({ByteOffset{start}, source}));
            // Like Snapshot::lineAt, the position between a CR and LF belongs to the upper line.
            if (summary.endsWithCr && m_text.byteAt(source) == '\n')
                return summary.lineBreaks - 1;
        } else {
            summary = text::combine(summary, summarize(std::string_view("...").substr(0, relative)));
        }
    }
    return summary.lineBreaks;
}

std::uint32_t FoldLayer::sourceLine(std::uint32_t target) const noexcept {
    if (target == 0)
        return 0;
    const auto cursor = m_runs.seek([target](const FoldRunSummary& sum) { return sum.projected.lineBreaks >= target; });
    const auto* run = cursor.item();
    FROST_ASSERT(run != nullptr && run->foldId == 0);
    const auto prefix = cursor.prefix();
    auto first = m_text.lineAt(ByteOffset{prefix.buffer.bytes}).value();
    if (prefix.projected.endsWithCr && run->projected.startsWithLf)
        ++first;
    return first + target - prefix.projected.lineBreaks;
}

LineIndex FoldLayer::bufferLine(std::uint32_t line) const noexcept { return LineIndex{sourceLine(line)}; }

std::uint64_t FoldLayer::clip(std::uint64_t offset) const noexcept {
    if (m_runs.summary().folds == 0)
        return m_text.clip(ByteOffset{offset}).value();
    const auto cursor = m_runs.seek([offset](const FoldRunSummary& sum) { return sum.projected.bytes > offset; });
    if (cursor.item() && cursor.item()->foldId != 0)
        return offset;
    return toFold(m_text.clip(toBuffer(offset, Bias::Left)), Bias::Left);
}

std::uint64_t FoldLayer::toFold(ByteOffset offset, Bias bias) const noexcept {
    const auto cursor = m_runs.seek([offset](const FoldRunSummary& sum) { return sum.buffer.bytes > offset.value(); });
    const auto prefix = cursor.prefix();
    const auto* run = cursor.item();
    if (!run)
        return prefix.projected.bytes;
    const auto relative = offset.value() - prefix.buffer.bytes;
    if (run->foldId == 0)
        return prefix.projected.bytes + relative;
    return prefix.projected.bytes + ((relative != 0 && bias == Bias::Right) ? run->projected.bytes : 0);
}

ByteOffset FoldLayer::toBuffer(std::uint64_t offset, Bias bias) const noexcept {
    const auto cursor = m_runs.seek([offset](const FoldRunSummary& sum) { return sum.projected.bytes > offset; });
    const auto prefix = cursor.prefix();
    const auto* run = cursor.item();
    if (!run)
        return ByteOffset{prefix.buffer.bytes};
    const auto relative = offset - prefix.projected.bytes;
    if (run->foldId == 0)
        return ByteOffset{prefix.buffer.bytes + relative};
    return ByteOffset{prefix.buffer.bytes + ((relative != 0 && bias == Bias::Right) ? run->buffer.bytes : 0)};
}

ByteOffset FoldLayer::hitToBuffer(std::uint64_t offset) const noexcept {
    const auto cursor = m_runs.seek([offset](const FoldRunSummary& sum) { return sum.projected.bytes > offset; });
    if (const auto* run = cursor.item(); run && run->foldId != 0) {
        const auto relative = offset - cursor.prefix().projected.bytes;
        if (relative > 0)
            return ByteOffset{cursor.prefix().buffer.bytes +
                              (relative * 2 < run->projected.bytes ? 0 : run->buffer.bytes)};
    }
    return toBuffer(offset, Bias::Left);
}

void FoldLayer::appendSegments(std::uint64_t begin, std::uint64_t end, std::vector<RowSegment>& out) const noexcept {
    std::uint64_t at = begin;
    forEachChunk(begin, end, [&](std::string_view chunk, SegmentKind kind) {
        const auto length = chunk.size();
        const auto first = toBuffer(at, Bias::Right);
        const auto last = toBuffer(at + length, Bias::Left);
        if (kind == SegmentKind::Text) {
            if (!out.empty() && out.back().kind == kind && out.back().buffer.end == first) {
                out.back().buffer.end = last;
            } else {
                out.push_back({kind, DisplayColumn{static_cast<std::uint32_t>(at - begin)}, {first, last}, {}});
            }
        } else {
            // A wrapped placeholder can occupy multiple rows, but always maps to its full range.
            out.push_back({kind,
                           DisplayColumn{static_cast<std::uint32_t>(at - begin)},
                           {toBuffer(at, Bias::Left), toBuffer(at + length, Bias::Right)},
                           chunk});
        }
        at += length;
    });
}

FoldLayer FoldLayer::updated(std::span<const TextChange> changes, std::span<const RawEdit> edits, Snapshot newText,
                             std::vector<SpanEdit>& patch) const noexcept {
    if (m_foldRecords.summary().count != 0) {
        const BatchMapper mapper(edits);
        const auto map = [&](ByteOffset offset, Bias bias) {
            return ByteOffset{mapper.mapRandom(offset.value(), bias)};
        };
        if (mapper.empty()) {
            FoldLayer next = *this;
            next.m_foldAnchors = m_foldAnchors.transformed(edits, newText.version());
            next.m_text = std::move(newText);
            next.m_stats = {};
            return next;
        }
        // Defensive recovery for display-coordinate overflow and failed internal reconstruction.
        // Ordinary edits should use the local paths below; reachability is under review.
        const auto rebuildAfterOverflow = [&](Snapshot source) {
            const auto& old = folds();
            std::vector<ByteRange> ranges;
            std::vector<std::uint64_t> ids;
            std::size_t index = 0;
            for (auto cursor = m_events.first(); const auto* event = cursor.item(); cursor.next()) {
                if (!event->open)
                    continue;
                const auto original = old[index++];
                const ByteRange mapped{map(original.begin, Bias::Right), map(original.end, Bias::Left)};
                const auto splitsCrLf = [&](ByteOffset p) {
                    return p.value() > 0 && p < source.size() && source.byteAt(p - 1) == '\r' &&
                           source.byteAt(p) == '\n';
                };
                if (mapped.begin < mapped.end && !splitsCrLf(mapped.begin) && !splitsCrLf(mapped.end) &&
                    (ranges.empty() || ranges.back() != mapped)) {
                    ranges.push_back(mapped);
                    ids.push_back(event->id);
                }
            }
            std::int64_t bytes = static_cast<std::int64_t>(source.size().value());
            ByteOffset covered;
            for (const auto range : ranges) {
                if (range.begin < covered)
                    continue;
                bytes += 3 - static_cast<std::int64_t>(range.size());
                covered = range.end;
            }
            if (bytes > 0xFFFF'FFFE) {
                ranges.clear();
                ids.clear();
            }
            FoldLayer rebuilt(std::move(source), std::move(ranges), std::move(ids), m_nextFoldId);
            rebuilt.m_stats.visitedFolds = old.size();
            rebuilt.m_stats.materializedFoldLists = 1;
            patch.push_back({0, lineCount(), 0, rebuilt.lineCount()});
            return rebuilt;
        };
        // A replacement that fully consumes every touched fold can remove those IDs in one
        // interval operation. Unaffected nested or distant folds keep their identities.
        if (edits.size() == 1 && !edits.front().range.empty()) {
            const RawEdit& edit = edits.front();
            auto cursor =
                m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= edit.range.begin.value(); });
            std::vector<FoldInfo> consumed;
            bool onlyConsumed = true;
            while (const auto* event = cursor.item()) {
                if (cursor.prefix().bytes + event->gap > edit.range.end.value())
                    break;
                const auto fold = *info(FoldId{event->id});
                if (fold.range.begin < edit.range.begin || fold.range.end > edit.range.end) {
                    onlyConsumed = false;
                    break;
                }
                consumed.push_back(fold);
                cursor.next();
            }
            if (onlyConsumed && !consumed.empty()) {
                std::ranges::sort(consumed, [](const auto& a, const auto& b) {
                    return a.range.begin > b.range.begin ||
                           (a.range.begin == b.range.begin && a.range.end < b.range.end);
                });
                consumed.erase(std::unique(consumed.begin(), consumed.end(),
                                           [](const auto& a, const auto& b) { return a.id == b.id; }),
                               consumed.end());
                FoldLayer next = *this;
                const auto run =
                    m_runs.seek([&](const FoldRunSummary& sum) { return sum.buffer.bytes > edit.range.begin.value(); });
                const bool hidden =
                    run.item() && run.item()->foldId != 0 && edit.range.begin.value() > run.prefix().buffer.bytes &&
                    edit.range.end.value() < run.prefix().buffer.bytes + run.item()->buffer.bytes &&
                    std::ranges::none_of(
                        consumed, [&](const auto& fold) { return fold.id.value == run.item()->foldId; });
                // Remove the entire contiguous event interval once, even when thousands of
                // consumed nested ranges share an endpoint. There is no transient promotion.
                auto [before, tail] =
                    m_events.split([&](const FoldEventSummary& sum) { return sum.bytes >= edit.range.begin.value(); });
                const auto base = before.summary().bytes;
                auto [discard, after] =
                    tail.split([&](const FoldEventSummary& sum) { return base + sum.bytes > edit.range.end.value(); });
                if (!after.empty()) {
                    FoldEvent first = *after.front();
                    const auto gap = static_cast<std::int64_t>(first.gap + discard.summary().bytes) +
                                     static_cast<std::int64_t>(edit.newBytes) -
                                     static_cast<std::int64_t>(edit.range.size());
                    FROST_ASSERT(gap >= 0);
                    first.gap = static_cast<std::uint64_t>(gap);
                    after = FoldEventTree::concat(FoldEventTree::fromItems(std::span(&first, 1)), after.withoutFront());
                }
                next.m_events = FoldEventTree::concat(before, after);
                for (const FoldInfo& fold : consumed) {
                    const FoldRecord item = next.record(fold.id.value);
                    next.m_foldAnchors = *next.m_foldAnchors.without(item.begin);
                    next.m_foldAnchors = *next.m_foldAnchors.without(item.end);
                    auto [left, right] = next.m_foldRecords.split(
                        [&](const FoldRecordSummary& sum) { return sum.maxId >= fold.id.value; });
                    next.m_foldRecords = FoldRecordTree::concat(left, right.withoutFront());
                }
                next.m_foldAnchors = next.m_foldAnchors.transformed(edits, newText.version());
                if (hidden) {
                    FoldRun changed = *run.item();
                    const auto start = run.prefix().buffer.bytes;
                    changed.buffer =
                        newText.summaryOf({ByteOffset{start}, ByteOffset{start + changed.buffer.bytes -
                                                                         edit.range.size() + edit.newBytes}});
                    auto [left, right] =
                        m_runs.split([&](const FoldRunSummary& sum) { return sum.buffer.bytes > start; });
                    next.m_runs =
                        FoldRunTree::concat(FoldRunTree::concat(left, FoldRunTree::fromItems(std::span(&changed, 1))),
                                            right.withoutFront());
                } else {
                    auto [through, right] = splitRunAt(m_runs, edit.range.end.value(), m_text);
                    auto [left, discardRuns] = splitRunAt(through, edit.range.begin.value(), m_text);
                    (void)discardRuns;
                    if (edit.newBytes != 0) {
                        const auto replacement =
                            newText.summaryOf({edit.range.begin, edit.range.begin + edit.newBytes});
                        const FoldRun identity{replacement, replacement, 0};
                        left = concatIdentityRuns(left, FoldRunTree::fromItems(std::span(&identity, 1)));
                    }
                    next.m_runs = concatIdentityRuns(left, right);
                }
                if (next.m_runs.summary().projected.bytes > 0xFFFF'FFFE)
                    return rebuildAfterOverflow(std::move(newText));
                next.m_text = std::move(newText);
                next.m_folds.reset();
                next.m_size = next.m_runs.summary().projected.bytes;
                next.m_lineCount = next.m_runs.summary().projected.lineBreaks + 1;
                next.m_stats = {hidden ? 1U : 0U, consumed.size(), 0, 1};
                if (!hidden) {
                    const auto oldFirst = lineAt(toFold(edit.range.begin, Bias::Left));
                    const auto oldLast = lineAt(toFold(edit.range.end, Bias::Right));
                    const auto newFirst = next.lineAt(next.toFold(edit.range.begin, Bias::Left));
                    const auto newLast = next.lineAt(next.toFold(edit.range.begin + edit.newBytes, Bias::Right));
                    patch.push_back({oldFirst, oldLast + 1, newFirst, newLast + 1});
                }
                return next;
            }
        }
        // Insertions at run boundaries create an identity run. At fold boundaries closing
        // endpoints stick left and opening endpoints stick right.
        if (edits.size() == 1 && edits.front().range.empty() && edits.front().newBytes != 0) {
            const RawEdit& edit = edits.front();
            const auto at = edit.range.begin;
            const auto insertedEnd = at + edit.newBytes;
            const auto splitsCrLf = [&](ByteOffset point) {
                return point.value() > 0 && point < newText.size() && newText.byteAt(point - 1) == '\r' &&
                       newText.byteAt(point) == '\n';
            };
            const bool foldBoundary = hasBoundary(at, true) || hasBoundary(at, false);
            const auto atRun = m_runs.seek([&](const FoldRunSummary& sum) { return sum.buffer.bytes > at.value(); });
            const bool runBoundary = atRun.prefix().buffer.bytes == at.value();
            if (!intersects({at, at}) && runBoundary &&
                (!foldBoundary || (!splitsCrLf(at) && !splitsCrLf(insertedEnd)))) {
                if (m_size + edit.newBytes > 0xFFFF'FFFE)
                    return rebuildAfterOverflow(std::move(newText));
                FoldLayer next = *this;
                const auto inserted = newText.summaryOf({at, insertedEnd});
                const FoldRun run{inserted, inserted, 0};
                auto [left, right] =
                    m_runs.split([&](const FoldRunSummary& sum) { return sum.buffer.bytes > at.value(); });
                next.m_runs =
                    concatIdentityRuns(concatIdentityRuns(left, FoldRunTree::fromItems(std::span(&run, 1))), right);
                auto [before, after] = m_events.split([&](const FoldEventSummary& sum) {
                    return sum.bytes > at.value() || (sum.bytes == at.value() && sum.lastOpen);
                });
                if (!after.empty()) {
                    FoldEvent first = *after.front();
                    first.gap += edit.newBytes;
                    next.m_events = FoldEventTree::concat(
                        FoldEventTree::concat(before, FoldEventTree::fromItems(std::span(&first, 1))),
                        after.withoutFront());
                }
                next.m_foldAnchors = m_foldAnchors.transformed(edits, newText.version());
                next.m_folds.reset();
                next.m_text = std::move(newText);
                next.m_size = next.m_runs.summary().projected.bytes;
                next.m_lineCount = next.m_runs.summary().projected.lineBreaks + 1;
                next.m_stats = {1, 0, 0};
                const auto oldLine = lineAt(toFold(at, Bias::Left));
                patch.push_back({oldLine, oldLine + 1, next.lineAt(next.toFold(at, Bias::Left)),
                                 next.lineAt(next.toFold(insertedEnd, Bias::Right)) + 1});
                return next;
            }
        }
        // A replacement at a shared opening endpoint moves that whole ordered group together.
        // Preserve the existing anchor and record identities and rewrite each event group once.
        if (edits.size() == 1 && !edits.front().range.empty()) {
            const RawEdit& edit = edits.front();
            const auto first =
                m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= edit.range.begin.value(); });
            const auto after =
                m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes > edit.range.begin.value(); });
            const auto count = after.prefix().count - first.prefix().count;
            const auto opens = after.prefix().opens - first.prefix().opens;
            const auto newStart = edit.range.begin + edit.newBytes;
            const bool splitsCrLf = newStart.value() > 0 && newStart < newText.size() &&
                                    newText.byteAt(newStart - 1) == '\r' && newText.byteAt(newStart) == '\n';
            const bool followingOutside =
                !after.item() || after.prefix().bytes + after.item()->gap > edit.range.end.value();
            const auto run =
                m_runs.seek([&](const FoldRunSummary& sum) { return sum.buffer.bytes > edit.range.begin.value(); });
            const auto runStart = run.prefix().buffer.bytes;
            const bool containsEdit =
                run.item() && run.item()->foldId != 0 && edit.range.end.value() < runStart + run.item()->buffer.bytes;
            if (count != 0 && count == opens && followingOutside && !splitsCrLf && containsEdit) {
                const bool hidden = runStart < edit.range.begin.value();
                FoldLayer next = *this;
                auto [before, tail] =
                    m_events.split([&](const FoldEventSummary& sum) { return sum.bytes >= edit.range.begin.value(); });
                const auto base = before.summary().bytes;
                auto [group, suffix] = tail.split(
                    [&](const FoldEventSummary& sum) { return base + sum.bytes > edit.range.begin.value(); });
                FoldEvent groupFirst = *group.front();
                groupFirst.gap += edit.newBytes;
                group =
                    FoldEventTree::concat(FoldEventTree::fromItems(std::span(&groupFirst, 1)), group.withoutFront());
                if (!suffix.empty()) {
                    FoldEvent suffixFirst = *suffix.front();
                    FROST_ASSERT(suffixFirst.gap >= edit.range.size());
                    suffixFirst.gap -= edit.range.size();
                    suffix = FoldEventTree::concat(FoldEventTree::fromItems(std::span(&suffixFirst, 1)),
                                                   suffix.withoutFront());
                }
                next.m_events = FoldEventTree::concat(FoldEventTree::concat(before, group), suffix);
                next.m_foldAnchors = m_foldAnchors.transformed(edits, newText.version());
                FoldRun changed = *run.item();
                const auto oldEnd = runStart + changed.buffer.bytes;
                const auto newEnd = oldEnd - edit.range.size() + edit.newBytes;
                const auto foldedStart = hidden ? ByteOffset{runStart} : newStart;
                changed.buffer = newText.summaryOf({foldedStart, ByteOffset{newEnd}});
                auto [left, right] =
                    m_runs.split([&](const FoldRunSummary& sum) { return sum.buffer.bytes > runStart; });
                if (!hidden && edit.newBytes != 0) {
                    const auto summary = newText.summaryOf({edit.range.begin, newStart});
                    const FoldRun identity{summary, summary, 0};
                    left = concatIdentityRuns(left, FoldRunTree::fromItems(std::span(&identity, 1)));
                }
                next.m_runs = concatIdentityRuns(
                    concatIdentityRuns(left, FoldRunTree::fromItems(std::span(&changed, 1))), right.withoutFront());
                if (next.m_runs.summary().projected.bytes > 0xFFFF'FFFE)
                    return rebuildAfterOverflow(std::move(newText));
                next.m_text = std::move(newText);
                next.m_folds.reset();
                next.m_size = next.m_runs.summary().projected.bytes;
                next.m_lineCount = next.m_runs.summary().projected.lineBreaks + 1;
                next.m_stats = {1, 0, 0, 1};
                if (!hidden) {
                    const auto oldLine = lineAt(toFold(edit.range.begin, Bias::Left));
                    const auto newFirst = next.lineAt(next.toFold(edit.range.begin, Bias::Left));
                    const auto newLast = next.lineAt(next.toFold(newStart, Bias::Right));
                    patch.push_back({oldLine, oldLine + 1, newFirst, newLast + 1});
                }
                return next;
            }
        }
        // Re-anchor only folds whose endpoints the batch touches. Remove inner ranges first,
        // transform the remaining index once, then restore surviving ranges with their IDs.
        std::vector<FoldInfo> touched;
        std::vector<ByteRange> affected;
        affected.reserve(edits.size());
        for (const RawEdit& edit : edits) {
            ByteRange region = edit.range;
            if (!edit.isNoOp()) {
                auto cursor =
                    m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= edit.range.begin.value(); });
                while (const auto* event = cursor.item()) {
                    if (cursor.prefix().bytes + event->gap > edit.range.end.value())
                        break;
                    const auto fold = *info(FoldId{event->id});
                    region.begin = std::min(region.begin, fold.range.begin);
                    region.end = std::max(region.end, fold.range.end);
                    touched.push_back(fold);
                    cursor.next();
                }
            }
            affected.push_back(region);
        }
        if (!touched.empty()) {
            std::ranges::sort(touched, [](const auto& a, const auto& b) {
                return a.range.begin > b.range.begin || (a.range.begin == b.range.begin && a.range.end < b.range.end);
            });
            touched.erase(
                std::unique(touched.begin(), touched.end(), [](const auto& a, const auto& b) { return a.id == b.id; }),
                touched.end());
            std::set<std::uint64_t> touchedIds;
            FoldLayer working = *this;
            std::uint64_t promotionVisits = 0;
            std::vector<std::uint64_t> hiddenIds;
            std::vector<std::uint64_t> hiddenEndpoints;
            for (const auto& fold : touched) {
                touchedIds.insert(fold.id.value);
                const auto visible =
                    m_runs.seek([&](const FoldRunSummary& sum) { return sum.buffer.bytes > fold.range.begin.value(); });
                if (!visible.item() || visible.item()->foldId != fold.id.value) {
                    hiddenIds.push_back(fold.id.value);
                    hiddenEndpoints.push_back(fold.range.begin.value());
                    hiddenEndpoints.push_back(fold.range.end.value());
                }
            }
            std::ranges::sort(hiddenIds);
            std::ranges::sort(hiddenEndpoints);
            hiddenEndpoints.erase(std::unique(hiddenEndpoints.begin(), hiddenEndpoints.end()), hiddenEndpoints.end());
            std::uint64_t scannedEvents = 0;
            for (const auto at : hiddenEndpoints)
                working.m_events = working.removeEventsAt(at, hiddenIds, scannedEvents);
            for (const auto id : hiddenIds) {
                const auto item = working.record(id);
                working.m_foldAnchors = *working.m_foldAnchors.without(item.begin);
                working.m_foldAnchors = *working.m_foldAnchors.without(item.end);
                auto [left, right] =
                    working.m_foldRecords.split([&](const FoldRecordSummary& sum) { return sum.maxId >= id; });
                working.m_foldRecords = FoldRecordTree::concat(left, right.withoutFront());
            }
            if (!hiddenIds.empty())
                working.m_folds.reset();
            for (const auto& fold : touched) {
                if (std::ranges::binary_search(hiddenIds, fold.id.value))
                    continue;
                auto removed = working.removing(fold.id);
                if (!removed)
                    return rebuildAfterOverflow(std::move(newText));
                working = std::move(*removed);
                promotionVisits += working.m_stats.visitedFolds;
            }
            std::vector<SpanEdit> ignored;
            FoldLayer next = working.updated(changes, edits, newText, ignored);
            next.m_nextFoldId = m_nextFoldId;
            auto work = next.m_stats;
            work.eventIntervalsRewritten += hiddenEndpoints.size();
            work.eventItemsScanned += scannedEvents;
            bool valid = true;
            std::vector<EventPoint> pendingEvents;
            std::map<std::uint64_t, std::set<std::uint64_t>> knownStarts;
            for (auto it = touched.rbegin(); it != touched.rend(); ++it) {
                const ByteRange mapped{map(it->range.begin, Bias::Right), map(it->range.end, Bias::Left)};
                const auto splitsCrLf = [&](ByteOffset p) {
                    return p.value() > 0 && p < newText.size() && newText.byteAt(p - 1) == '\r' &&
                           newText.byteAt(p) == '\n';
                };
                if (mapped.begin >= mapped.end || splitsCrLf(mapped.begin) || splitsCrLf(mapped.end))
                    continue;
                const auto visible = next.m_runs.seek(
                    [&](const FoldRunSummary& sum) { return sum.buffer.bytes > mapped.begin.value(); });
                const bool hidden = visible.item() && visible.item()->foldId != 0 &&
                                    mapped.end.value() <= visible.prefix().buffer.bytes + visible.item()->buffer.bytes;
                if (hidden) {
                    auto [known, fresh] = knownStarts.try_emplace(mapped.begin.value());
                    if (fresh) {
                        auto cursor = next.m_events.seek(
                            [&](const FoldEventSummary& sum) { return sum.bytes >= mapped.begin.value(); });
                        while (const auto* event = cursor.item()) {
                            if (cursor.prefix().bytes + event->gap != mapped.begin.value())
                                break;
                            ++work.eventItemsScanned;
                            if (event->open)
                                known->second.insert(next.info(FoldId{event->id})->range.end.value());
                            cursor.next();
                        }
                    }
                    if (!known->second.insert(mapped.end.value()).second)
                        continue;
                    if (next.m_foldAnchors.size() + 2 > AnchorSet::kMaxAnchors) {
                        valid = false;
                        break;
                    }
                    auto [starts, begin] = next.m_foldAnchors.withAnchor(mapped.begin, Bias::Right);
                    auto [ends, end] = starts.withAnchor(mapped.end, Bias::Left);
                    next.m_foldAnchors = std::move(ends);
                    const FoldRecord item{it->id.value, begin, end};
                    auto [left, right] =
                        next.m_foldRecords.split([&](const FoldRecordSummary& sum) { return sum.maxId >= item.id; });
                    next.m_foldRecords = FoldRecordTree::concat(
                        FoldRecordTree::concat(left, FoldRecordTree::fromItems(std::span(&item, 1))), right);
                    pendingEvents.push_back({mapped.begin.value(), mapped.end.value(), item.id, true});
                    pendingEvents.push_back({mapped.end.value(), mapped.begin.value(), item.id, false});
                    continue;
                }
                auto restored = next.adding(mapped, it->id);
                if (!restored) {
                    if (restored.error().code() == ErrorCode::AlreadyExists)
                        continue;
                    valid = false;
                    break;
                }
                work.visitedFolds += restored->first.m_stats.visitedFolds;
                next = std::move(restored->first);
            }
            if (valid && !pendingEvents.empty()) {
                std::ranges::sort(pendingEvents, eventLess);
                for (std::size_t first = 0; first < pendingEvents.size();) {
                    const auto at = pendingEvents[first].at;
                    auto last = first + 1;
                    while (last < pendingEvents.size() && pendingEvents[last].at == at)
                        ++last;
                    auto [before, tail] =
                        next.m_events.split([at](const FoldEventSummary& sum) { return sum.bytes >= at; });
                    const auto base = before.summary().bytes;
                    std::vector<EventPoint> points(pendingEvents.begin() + static_cast<std::ptrdiff_t>(first),
                                                   pendingEvents.begin() + static_cast<std::ptrdiff_t>(last));
                    std::uint64_t count = 0;
                    for (auto cursor = tail.first(); const auto* event = cursor.item(); cursor.next()) {
                        if (base + cursor.prefix().bytes + event->gap != at)
                            break;
                        ++count;
                        ++work.eventItemsScanned;
                        const auto& item = next.record(event->id);
                        const auto other = *next.m_foldAnchors.find(event->open ? item.end : item.begin);
                        points.push_back({at, other.value(), event->id, event->open});
                    }
                    std::ranges::sort(points, eventLess);
                    auto [discard, after] =
                        tail.split([count](const FoldEventSummary& sum) { return sum.count > count; });
                    (void)discard;
                    if (count == 0 && !after.empty()) {
                        FoldEvent head = *after.front();
                        head.gap -= at - base;
                        after =
                            FoldEventTree::concat(FoldEventTree::fromItems(std::span(&head, 1)), after.withoutFront());
                    }
                    std::vector<FoldEvent> group;
                    group.reserve(points.size());
                    for (const auto& point : points)
                        group.push_back({group.empty() ? at - base : 0, point.id, point.open});
                    next.m_events =
                        FoldEventTree::concat(FoldEventTree::concat(before, FoldEventTree::fromItems(group)), after);
                    ++work.eventIntervalsRewritten;
                    first = last;
                }
                next.m_folds.reset();
            }
            if (valid) {
                work.visitedFolds += touched.size() + promotionVisits;
                next.m_stats = work;
                bool hiddenOnly = true;
                for (const RawEdit& edit : edits) {
                    if (edit.isNoOp())
                        continue;
                    const auto run = m_runs.seek(
                        [&](const FoldRunSummary& sum) { return sum.buffer.bytes > edit.range.begin.value(); });
                    if (!run.item() || run.item()->foldId == 0 || touchedIds.contains(run.item()->foldId) ||
                        edit.range.begin.value() <= run.prefix().buffer.bytes ||
                        edit.range.end.value() >= run.prefix().buffer.bytes + run.item()->buffer.bytes) {
                        hiddenOnly = false;
                        break;
                    }
                }
                if (!hiddenOnly) {
                    for (std::size_t i = 0; i < edits.size(); ++i) {
                        if (edits[i].isNoOp())
                            continue;
                        const auto oldFirst = lineAt(toFold(affected[i].begin, Bias::Left));
                        const auto oldLast = lineAt(toFold(affected[i].end, Bias::Right));
                        const auto newBegin = map(affected[i].begin, Bias::Left);
                        const auto newEnd = map(affected[i].end, Bias::Right);
                        const SpanEdit changed{oldFirst, oldLast + 1, next.lineAt(next.toFold(newBegin, Bias::Left)),
                                               next.lineAt(next.toFold(newEnd, Bias::Right)) + 1};
                        if (!patch.empty() &&
                            (changed.oldFirst < patch.back().oldEnd || changed.newFirst < patch.back().newEnd)) {
                            patch.back().oldFirst = std::min(patch.back().oldFirst, changed.oldFirst);
                            patch.back().newFirst = std::min(patch.back().newFirst, changed.newFirst);
                            patch.back().oldEnd = std::max(patch.back().oldEnd, changed.oldEnd);
                            patch.back().newEnd = std::max(patch.back().newEnd, changed.newEnd);
                        } else
                            patch.push_back(changed);
                    }
                }
                return next;
            }
        }
        // Batch edits in original coordinates are spliced from right to left. Edits within the
        // same run share one replacement, and distinct changed lines retain distinct patches.
        struct ChangedRun {
            std::uint64_t begin;
            std::uint64_t end;
            FoldRun run;
        };
        std::vector<ChangedRun> changedRuns;
        bool local = !edits.empty();
        std::int64_t projectedDelta = 0;
        for (const RawEdit& edit : edits) {
            if (edit.isNoOp())
                continue;
            const auto event =
                m_events.seek([&](const FoldEventSummary& sum) { return sum.bytes >= edit.range.begin.value(); });
            const auto cursor =
                m_runs.seek([&](const FoldRunSummary& sum) { return sum.buffer.bytes > edit.range.begin.value(); });
            const auto* run = cursor.item();
            auto begin = cursor.prefix().buffer.bytes;
            if (!run && edit.range.begin == m_text.size() && m_runs.back() && m_runs.back()->foldId == 0) {
                run = m_runs.back();
                begin -= run->buffer.bytes;
            }
            if ((event.item() && event.prefix().bytes + event.item()->gap <= edit.range.end.value()) || !run ||
                edit.range.begin.value() < begin || edit.range.end.value() > begin + run->buffer.bytes) {
                local = false;
                break;
            }
            if (changedRuns.empty() || changedRuns.back().begin != begin)
                changedRuns.push_back({begin, begin + run->buffer.bytes, *run});
            if (run->foldId == 0)
                projectedDelta +=
                    static_cast<std::int64_t>(edit.newBytes) - static_cast<std::int64_t>(edit.range.size());
        }
        if (local && !changedRuns.empty()) {
            if (static_cast<std::int64_t>(m_size) + projectedDelta > 0xFFFF'FFFE)
                return rebuildAfterOverflow(std::move(newText));
            FoldLayer next = *this;
            next.m_stats = {changedRuns.size(), 0, 0};
            next.m_foldAnchors = m_foldAnchors.transformed(edits, newText.version());
            next.m_folds.reset();
            for (auto it = changedRuns.rbegin(); it != changedRuns.rend(); ++it) {
                FoldRun changed = it->run;
                changed.buffer =
                    newText.summaryOf({map(ByteOffset{it->begin}, Bias::Left), map(ByteOffset{it->end}, Bias::Right)});
                if (changed.foldId == 0)
                    changed.projected = changed.buffer;
                auto [left, tail] =
                    next.m_runs.split([&](const FoldRunSummary& sum) { return sum.buffer.bytes > it->begin; });
                const auto replacement =
                    changed.buffer.bytes == 0 ? FoldRunTree{} : FoldRunTree::fromItems(std::span(&changed, 1));
                next.m_runs = concatIdentityRuns(concatIdentityRuns(left, replacement), tail.withoutFront());
            }
            for (const RawEdit& edit : edits | std::views::reverse) {
                if (edit.isNoOp())
                    continue;
                auto [before, after] = next.m_events.split(
                    [&](const FoldEventSummary& sum) { return sum.bytes > edit.range.end.value(); });
                if (after.empty())
                    continue;
                FoldEvent first = *after.front();
                const auto delta =
                    static_cast<std::int64_t>(edit.newBytes) - static_cast<std::int64_t>(edit.range.size());
                const auto gap = static_cast<std::int64_t>(first.gap) + delta;
                FROST_ASSERT(gap >= 0);
                first.gap = static_cast<std::uint64_t>(gap);
                next.m_events =
                    FoldEventTree::concat(FoldEventTree::concat(before, FoldEventTree::fromItems(std::span(&first, 1))),
                                          after.withoutFront());
            }
            next.m_text = std::move(newText);
            next.m_size = next.m_runs.summary().projected.bytes;
            next.m_lineCount = next.m_runs.summary().projected.lineBreaks + 1;
            for (const RawEdit& edit : edits) {
                if (edit.isNoOp() || intersects(edit.range))
                    continue;
                const auto start = map(edit.range.begin, Bias::Left);
                const auto end = map(edit.range.end, Bias::Right);
                SpanEdit changed{
                    lineAt(toFold(edit.range.begin, Bias::Left)), lineAt(toFold(edit.range.end, Bias::Right)) + 1,
                    next.lineAt(next.toFold(start, Bias::Left)), next.lineAt(next.toFold(end, Bias::Right)) + 1};
                if (!patch.empty() &&
                    (changed.oldFirst < patch.back().oldEnd || changed.newFirst < patch.back().newEnd)) {
                    patch.back().oldEnd = std::max(patch.back().oldEnd, changed.oldEnd);
                    patch.back().newEnd = std::max(patch.back().newEnd, changed.newEnd);
                } else
                    patch.push_back(changed);
            }
            return next;
        }
        // Defensive reconstruction: this should require either an unexpected reinsertion failure
        // or a raw edit crossing projection runs without a corresponding endpoint. Ordinary-edit
        // locality tests cover nested, adjacent and batched examples; they are not a proof.
        const auto& oldFolds = folds();
        std::vector<ByteRange> mapped;
        std::vector<std::uint64_t> mappedIds;
        std::size_t index = 0;
        for (auto cursor = m_events.first(); const auto* event = cursor.item(); cursor.next()) {
            if (!event->open)
                continue;
            const ByteRange fold = oldFolds[index++];
            const ByteRange range{map(fold.begin, Bias::Right), map(fold.end, Bias::Left)};
            const auto splitsCrLf = [&newText](ByteOffset p) {
                return p.value() > 0 && p < newText.size() && newText.byteAt(p - 1) == '\r' &&
                       newText.byteAt(p) == '\n';
            };
            if (range.begin < range.end && !splitsCrLf(range.begin) && !splitsCrLf(range.end) &&
                (mapped.empty() || mapped.back() != range)) {
                mapped.push_back(range);
                mappedIds.push_back(event->id);
            }
        }
        std::int64_t projectedBytes = static_cast<std::int64_t>(newText.size().value());
        ByteOffset covered;
        for (const auto range : mapped) {
            if (range.begin < covered)
                continue;
            projectedBytes += 3 - static_cast<std::int64_t>(range.size());
            covered = range.end;
        }
        if (projectedBytes > 0xFFFF'FFFE)
            return rebuildAfterOverflow(std::move(newText));
        FoldLayer next(std::move(newText), std::move(mapped), std::move(mappedIds), m_nextFoldId);
        next.m_stats.visitedFolds = oldFolds.size();
        next.m_stats.materializedFoldLists = 1;
        if (next.size() > 0xFFFF'FFFE) {
            FoldLayer unfolded(next.buffer(), {}, {}, m_nextFoldId);
            patch.push_back({0, lineCount(), 0, unfolded.lineCount()});
            return unfolded;
        }
        // When every raw edit remains strictly inside a surviving visible fold, the displayed
        // bytes and line structure are unchanged. Do not ask wrap or block layers to rebuild.
        const bool hiddenOnly = !edits.empty() && std::ranges::all_of(edits, [&](const RawEdit& edit) {
            const auto cursor =
                m_runs.seek([&](const FoldRunSummary& sum) { return sum.buffer.bytes > edit.range.begin.value(); });
            const auto* run = cursor.item();
            if (!run || run->foldId == 0)
                return false;
            const ByteRange oldRange{ByteOffset{cursor.prefix().buffer.bytes},
                                     ByteOffset{cursor.prefix().buffer.bytes + run->buffer.bytes}};
            if (edit.range.begin <= oldRange.begin || edit.range.end >= oldRange.end)
                return false;
            const ByteRange mappedRange{map(oldRange.begin, Bias::Right), map(oldRange.end, Bias::Left)};
            return std::ranges::find(next.folds(), mappedRange) != next.folds().end();
        });
        if (hiddenOnly)
            return next;
        ByteOffset begin = edits.front().range.begin;
        ByteOffset end = edits.back().range.end;
        // Fold removal can expose its entire contents. Include any fold touched by an edit,
        // including its parent, in the changed region on both sides.
        for (const ByteRange fold : oldFolds) {
            if (fold.begin <= end && fold.end >= begin) {
                begin = std::min(begin, fold.begin);
                end = std::max(end, fold.end);
            }
        }
        const auto oldFirst = lineAt(toFold(begin, Bias::Left));
        const auto oldLast = lineAt(toFold(end, Bias::Right));
        const auto newBegin = map(begin, Bias::Left);
        const auto newEnd = map(end, Bias::Right);
        const auto newFirst = next.lineAt(next.toFold(newBegin, Bias::Left));
        const auto newLast = next.lineAt(next.toFold(newEnd, Bias::Right));
        patch.push_back({oldFirst, oldLast + 1, newFirst, newLast + 1});
        return next;
    }
    const std::vector<SpanEdit> lines = composeLinePatch(changes);
    patch.insert(patch.end(), lines.begin(), lines.end());
    return FoldLayer(std::move(newText), {}, {}, m_nextFoldId);
}

} // namespace frost::text::detail
