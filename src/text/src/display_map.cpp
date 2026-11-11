// SPDX-License-Identifier: Apache-2.0
//
// DisplayMap (frost/text/display_map.hpp): the four layers of display_layers.hpp composed into
// buffer <-> display mapping, updates from ChangeSets, row descriptions, hit testing and vertical
// caret movement.

#include "frost/text/display_map.hpp"

#include "frost/core/assert.hpp"

#include "display_layers.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace frost::text {

namespace {

using detail::BlockRowRef;
using detail::DisplayLayers;
using detail::SpanEdit;
using detail::WrapRow;

constexpr char32_t kZeroWidthJoiner = 0x200D;

[[nodiscard]] TextWidth effectiveWidth(const WrapSettings& settings, const ITextShaper& shaper) noexcept {
    switch (settings.mode) {
    case WrapMode::Off: return 0;
    case WrapMode::Column:
        return static_cast<TextWidth>(std::max<std::uint32_t>(settings.columns, 1)) * shaper.cellWidth();
    case WrapMode::Width: return std::max<TextWidth>(settings.width, 1);
    }
    return 0;
}

// A fold toggle changes only the display lines intersecting its source range. Blocks keep their
// anchors and are placed again in that region, including anchors in the hidden text.
void replaceFoldProjection(DisplayLayers& layers, detail::FoldLayer next, ByteRange range) noexcept {
    if (layers.fold.sameProjection(next)) {
        layers.fold = std::move(next);
        return;
    }
    const SpanEdit lineEdit{layers.fold.lineAt(layers.fold.toFold(range.begin, Bias::Left)),
                            layers.fold.lineAt(layers.fold.toFold(range.end, Bias::Right)) + 1,
                            next.lineAt(next.toFold(range.begin, Bias::Left)),
                            next.lineAt(next.toFold(range.end, Bias::Right)) + 1};
    std::vector<SpanEdit> rows;
    auto wrap = layers.wrap.updated(layers.fold, next, std::span(&lineEdit, 1), *layers.shaper, rows);
    std::vector<SpanEdit> inlayRows;
    auto inlay = detail::InlayLayer(layers.inlay.inlays(), next, wrap).updated(rows, inlayRows);
    const detail::RegionEdit region{lineEdit, inlayRows.front()};
    std::vector<RowEdit> ignored;
    layers.block.apply({}, next.buffer().version(), std::span(&region, 1), next, wrap, ignored);
    layers.fold = std::move(next);
    layers.wrap = std::move(wrap);
    layers.inlay = std::move(inlay);
}

// Calls fn(char32_t cp, std::uint32_t column) for the code points of text row `row` in display
// order, until fn returns false.
template<class F>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): visitor is reused as an lvalue.
void forEachDisplayChar(const Snapshot& text, const DisplayRowInfo& row, F&& fn) noexcept {
    for (const RowSegment& segment : row.segments) {
        std::uint32_t column = segment.column.value();
        const auto visit = [&](std::string_view chunk) {
            for (std::size_t i = 0; i < chunk.size();) {
                const DecodedChar decoded = decodeUtf8(chunk.substr(i));
                if (!fn(decoded.cp, column)) {
                    return false;
                }
                column += decoded.bytes;
                i += decoded.bytes;
            }
            return true;
        };
        if (segment.kind == SegmentKind::Text) {
            for (const std::string_view chunk : text.chunks(segment.buffer)) {
                if (!visit(chunk)) {
                    return;
                }
            }
        } else if (!visit(segment.text)) {
            return;
        }
    }
}

// The length of a text row's display text in bytes.
[[nodiscard]] std::uint32_t displayLength(const DisplayRowInfo& row) noexcept {
    if (row.segments.empty()) {
        return 0;
    }
    const RowSegment& last = row.segments.back();
    const std::uint64_t bytes = last.kind == SegmentKind::Text ? last.buffer.size() : last.text.size();
    return last.column.value() + static_cast<std::uint32_t>(bytes);
}

} // namespace

bool DisplayRowCursor::next(DisplayRowInfo& row) noexcept {
    if (m_map == nullptr || m_next >= m_map->rowCount()) {
        return false;
    }
    m_map->rowInfo(DisplayRow{m_next++}, row);
    return true;
}

DisplayMap::DisplayMap(Snapshot text, const ITextShaper& shaper, WrapSettings wrap) noexcept
    : m_layers(std::make_unique<DisplayLayers>()) {
    DisplayLayers& layers = *m_layers;
    layers.shaper = &shaper;
    layers.settings = wrap;
    layers.fold = detail::FoldLayer(std::move(text));
    layers.wrap = detail::WrapLayer(layers.fold, shaper, effectiveWidth(wrap, shaper));
    layers.block = detail::BlockLayer(layers.fold.buffer(), layers.wrap.rowCount());
}

DisplayMap::DisplayMap(DisplayMap&& other) noexcept = default;
DisplayMap& DisplayMap::operator=(DisplayMap&& other) noexcept = default;
DisplayMap::~DisplayMap() = default;

const Snapshot& DisplayMap::text() const noexcept { return m_layers->fold.buffer(); }

std::uint64_t DisplayMap::version() const noexcept { return text().version(); }

const WrapSettings& DisplayMap::wrap() const noexcept { return m_layers->settings; }

TextWidth DisplayMap::wrapWidth() const noexcept { return m_layers->wrap.width(); }

std::uint32_t DisplayMap::rowCount() const noexcept { return m_layers->block.rowCount(); }

std::vector<RowEdit> DisplayMap::wholeEdit(std::uint32_t oldRows) const noexcept {
    return {RowEdit{DisplayRow{0}, DisplayRow{oldRows}, DisplayRow{0}, DisplayRow{rowCount()}}};
}

Result<std::vector<RowEdit>> DisplayMap::apply(const ChangeSet& changes, const Snapshot& text) noexcept {
    DisplayLayers& layers = *m_layers;
    if (changes.baseVersion != version()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the change set applies to version {}, the display map is at version {}",
                                changes.baseVersion, version()));
    }
    if (text.version() != changes.version) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the snapshot is at version {}, the change set produces version {}", text.version(),
                                changes.version));
    }
    if (changes.version == changes.baseVersion) {
        return std::vector<RowEdit>{};
    }
    if (changes.edits.empty()) {
        return fail(ErrorCode::InvalidArgument, "a version-changing display update requires raw edits");
    }

    // Up the stack: buffer lines -> fold lines -> wrap rows -> inlay rows -> display rows.
    std::vector<SpanEdit> lines;
    detail::FoldLayer fold = layers.fold.updated(changes.changes, changes.edits, text, lines);
    // A raw edit can split or form CRLF: the first affected line may then move to the
    // preceding line on only one side. Widen starts until the unchanged prefixes align.
    std::vector<SpanEdit> aligned;
    std::int64_t shift = 0;
    for (SpanEdit edit : lines) {
        const auto expected = static_cast<std::int64_t>(edit.oldFirst) + shift;
        if (static_cast<std::int64_t>(edit.newFirst) > expected)
            edit.newFirst = static_cast<std::uint32_t>(expected);
        else if (static_cast<std::int64_t>(edit.newFirst) < expected)
            edit.oldFirst -= static_cast<std::uint32_t>(expected - edit.newFirst);
        if (!aligned.empty() && (edit.oldFirst < aligned.back().oldEnd || edit.newFirst < aligned.back().newEnd)) {
            aligned.back().oldEnd = std::max(aligned.back().oldEnd, edit.oldEnd);
            aligned.back().newEnd = std::max(aligned.back().newEnd, edit.newEnd);
        } else {
            aligned.push_back(edit);
        }
        shift = static_cast<std::int64_t>(aligned.back().newEnd) - aligned.back().oldEnd;
    }
    lines = std::move(aligned);
    std::int64_t lineCount = layers.fold.lineCount();
    for (const SpanEdit& edit : lines) {
        lineCount += (std::int64_t{edit.newEnd} - edit.newFirst) - (std::int64_t{edit.oldEnd} - edit.oldFirst);
    }
    FROST_ASSERT(lineCount == fold.lineCount());
    std::vector<SpanEdit> wrapRows;
    detail::WrapLayer wrap = layers.wrap.updated(layers.fold, fold, lines, *layers.shaper, wrapRows);
    std::vector<SpanEdit> inlayRows;
    const detail::InlayLayer inlay = layers.inlay.updated(changes.edits, fold, wrap, wrapRows, inlayRows);
    FROST_ASSERT(inlayRows.size() == lines.size());
    std::vector<detail::RegionEdit> regions;
    regions.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        regions.push_back(detail::RegionEdit{lines[i], inlayRows[i]});
    }
    std::vector<RowEdit> patch;
    layers.block.apply(changes.edits, changes.version, regions, fold, wrap, patch);
    layers.fold = std::move(fold);
    layers.wrap = std::move(wrap);
    layers.inlay = inlay;
    return patch;
}

std::vector<RowEdit> DisplayMap::reset(Snapshot text) noexcept {
    DisplayLayers& layers = *m_layers;
    const std::uint32_t oldRows = rowCount();
    const auto nextFoldId = layers.fold.nextFoldId();
    layers.fold = detail::FoldLayer(std::move(text), {}, {}, nextFoldId);
    layers.wrap = detail::WrapLayer(layers.fold, *layers.shaper, effectiveWidth(layers.settings, *layers.shaper));
    layers.inlay = detail::InlayLayer();
    layers.block = detail::BlockLayer(layers.fold.buffer(), layers.wrap.rowCount());
    return wholeEdit(oldRows);
}

std::vector<RowEdit> DisplayMap::setWrap(WrapSettings wrap) noexcept {
    DisplayLayers& layers = *m_layers;
    const std::uint32_t oldRows = rowCount();
    layers.settings = wrap;
    layers.wrap = detail::WrapLayer(layers.fold, *layers.shaper, effectiveWidth(wrap, *layers.shaper));
    layers.inlay = detail::InlayLayer(layers.inlay.inlays(), layers.fold, layers.wrap);
    layers.block.rebuild(layers.fold, layers.wrap);
    return wholeEdit(oldRows);
}

std::vector<RowEdit> DisplayMap::setShaper(const ITextShaper& shaper) noexcept {
    m_layers->shaper = &shaper;
    return setWrap(m_layers->settings);
}

Result<std::vector<RowEdit>> DisplayMap::setFolds(std::vector<ByteRange> ranges) noexcept {
    const Snapshot& source = text();
    std::ranges::sort(ranges, [](const ByteRange& a, const ByteRange& b) {
        return a.begin < b.begin || (a.begin == b.begin && a.end > b.end);
    });
    ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());
    std::vector<ByteRange> parents;
    std::int64_t projectedSize = static_cast<std::int64_t>(source.size().value());
    for (const ByteRange range : ranges) {
        if (range.begin >= range.end || range.end > source.size())
            return fail(ErrorCode::InvalidArgument, "fold range is empty or outside the buffer");
        const auto splitsCrLf = [&source](ByteOffset p) {
            return p.value() > 0 && p < source.size() && source.byteAt(p - 1) == '\r' && source.byteAt(p) == '\n';
        };
        if (!source.isCharBoundary(range.begin) || !source.isCharBoundary(range.end) || splitsCrLf(range.begin) ||
            splitsCrLf(range.end))
            return fail(ErrorCode::InvalidArgument, "fold splits UTF-8 or CRLF");
        while (!parents.empty() && range.begin >= parents.back().end)
            parents.pop_back();
        if (!parents.empty() && range.end > parents.back().end)
            return fail(ErrorCode::InvalidArgument, "fold ranges overlap without nesting");
        if (parents.empty())
            projectedSize += 3 - static_cast<std::int64_t>(range.size());
        parents.push_back(range);
    }
    if (projectedSize > 0xFFFF'FFFE)
        return fail(ErrorCode::OutOfRange, "folded text is too large for display coordinates");
    if (ranges.size() > std::numeric_limits<std::uint64_t>::max() - m_layers->fold.nextFoldId())
        return fail(ErrorCode::OutOfRange, "fold identity space exhausted");
    const auto oldRows = rowCount();
    DisplayLayers& layers = *m_layers;
    layers.fold = detail::FoldLayer(source, std::move(ranges), {}, layers.fold.nextFoldId());
    layers.wrap = detail::WrapLayer(layers.fold, *layers.shaper, effectiveWidth(layers.settings, *layers.shaper));
    layers.inlay = detail::InlayLayer(layers.inlay.inlays(), layers.fold, layers.wrap);
    layers.block.rebuild(layers.fold, layers.wrap);
    return wholeEdit(oldRows);
}

const std::vector<ByteRange>& DisplayMap::folds() const noexcept { return m_layers->fold.folds(); }
bool DisplayMap::intersectsFold(ByteRange range) const noexcept { return m_layers->fold.intersects(range); }
std::vector<ByteRange> DisplayMap::foldsContaining(ByteOffset offset) const noexcept {
    FROST_PRE(offset <= text().size());
    return m_layers->fold.containing(offset);
}
std::vector<FoldInfo> DisplayMap::foldInfosContaining(ByteOffset offset) const noexcept {
    FROST_PRE(offset <= text().size());
    return m_layers->fold.containingInfos(offset);
}
std::vector<FoldInfo> DisplayMap::foldInfosIntersecting(ByteRange range) const noexcept {
    FROST_PRE(range.begin <= range.end && range.end <= text().size());
    return m_layers->fold.intersectingInfos(range);
}
std::vector<FoldInfo> DisplayMap::foldInfosWithBoundaryIn(ByteRange range) const noexcept {
    FROST_PRE(range.begin <= range.end && range.end <= text().size());
    return m_layers->fold.boundaryInfos(range);
}
bool DisplayMap::foldStartsIn(ByteRange range) const noexcept { return m_layers->fold.hasStart(range); }
bool DisplayMap::hasFoldBoundary(ByteOffset offset, bool start) const noexcept {
    return m_layers->fold.hasBoundary(offset, start);
}
FoldUpdateStats DisplayMap::foldUpdateStatsForTesting() const noexcept { return m_layers->fold.updateStats(); }

Result<FoldId> DisplayMap::addFold(ByteRange range) noexcept {
    auto added = m_layers->fold.adding(range);
    if (!added)
        return std::unexpected(added.error());
    const FoldId id = added->second;
    replaceFoldProjection(*m_layers, std::move(added->first), range);
    return id;
}

Result<void> DisplayMap::removeFold(FoldId id) noexcept {
    auto old = m_layers->fold.info(id);
    if (!old)
        return std::unexpected(old.error());
    auto removed = m_layers->fold.removing(id);
    if (!removed)
        return std::unexpected(removed.error());
    replaceFoldProjection(*m_layers, std::move(*removed), old->range);
    return {};
}

std::optional<FoldInfo> DisplayMap::firstFoldStartingIn(ByteRange range) const noexcept {
    return m_layers->fold.firstStarting(range);
}
std::optional<FoldInfo> DisplayMap::foldAtBoundary(ByteOffset offset, bool start) const noexcept {
    return m_layers->fold.atBoundary(offset, start);
}

void DisplayMap::setInlays(std::vector<Inlay> inlays) noexcept {
    DisplayLayers& layers = *m_layers;
    layers.inlay = detail::InlayLayer(std::move(inlays), layers.fold, layers.wrap);
}

const std::vector<DisplayMap::Inlay>& DisplayMap::inlays() const noexcept { return m_layers->inlay.inlays(); }

Result<BlockId> DisplayMap::addBlock(const BlockDecoration& block) noexcept {
    return m_layers->block.add(block, m_layers->fold, m_layers->wrap);
}

Result<void> DisplayMap::removeBlock(BlockId id) noexcept {
    return m_layers->block.remove(id, m_layers->fold, m_layers->wrap);
}

Result<BlockInfo> DisplayMap::blockInfo(BlockId id) const noexcept {
    return m_layers->block.info(id, m_layers->fold, m_layers->wrap);
}

std::size_t DisplayMap::blockCount() const noexcept { return m_layers->block.blockCount(); }

DisplayPoint DisplayMap::toDisplayPoint(ByteOffset offset, Bias bias) const noexcept {
    const DisplayLayers& layers = *m_layers;
    FROST_PRE(offset <= text().size());
    const std::uint64_t folded = layers.fold.toFold(offset, bias);
    const std::uint32_t wrapRow = layers.wrap.rowAt(folded, bias, layers.fold);
    const WrapRow row = layers.wrap.row(wrapRow, layers.fold);
    FROST_ASSERT(folded >= row.start);
    const auto column = static_cast<std::uint32_t>(std::min(folded, row.end) - row.start);
    return DisplayPoint{DisplayRow{layers.block.toOutputRow(wrapRow)},
                        DisplayColumn{layers.inlay.toInlayColumn(wrapRow, column, bias)}};
}

Result<DisplayPoint> DisplayMap::toDisplayPoint(Point point, Bias bias) const noexcept {
    const Result<ByteOffset> offset = text().offsetAt(point);
    if (!offset) {
        return std::unexpected(offset.error());
    }
    return toDisplayPoint(*offset, bias);
}

ByteOffset DisplayMap::toOffset(DisplayPoint point) const noexcept {
    const DisplayLayers& layers = *m_layers;
    const std::uint32_t row = std::min(point.row.value(), rowCount() - 1);
    const BlockRowRef ref = layers.block.fromOutputRow(row);
    if (ref.isBlock) {
        // Above blocks sit before the first row of their line, Below blocks after the last one, so
        // the boundary has a text row on that side.
        if (layers.block.decoration(ref.id).placement == BlockPlacement::Above) {
            return layers.fold.toBuffer(layers.wrap.row(ref.inputRow, layers.fold).start, Bias::Right);
        }
        FROST_ASSERT(ref.inputRow > 0);
        return layers.fold.toBuffer(layers.wrap.row(ref.inputRow - 1, layers.fold).end, Bias::Left);
    }
    const WrapRow wrapped = layers.wrap.row(ref.inputRow, layers.fold);
    const std::uint32_t column = layers.inlay.toWrapColumn(ref.inputRow, point.column.value(), Bias::Left);
    const std::uint64_t folded = layers.fold.clip(std::min<std::uint64_t>(wrapped.start + column, wrapped.end));
    return layers.fold.hitToBuffer(folded);
}

Point DisplayMap::toPoint(DisplayPoint point) const noexcept { return text().pointAt(toOffset(point)); }

DisplayRowInfo DisplayMap::rowInfo(DisplayRow row) const noexcept {
    DisplayRowInfo info;
    rowInfo(row, info);
    return info;
}

void DisplayMap::rowInfo(DisplayRow row, DisplayRowInfo& out) const noexcept {
    const DisplayLayers& layers = *m_layers;
    FROST_PRE(row.value() < rowCount());
    out.row = row;
    out.segments.clear();
    const BlockRowRef ref = layers.block.fromOutputRow(row.value());
    if (ref.isBlock) {
        const BlockDecoration& block = layers.block.decoration(ref.id);
        const ByteOffset at = toOffset(DisplayPoint{row, DisplayColumn{0}});
        out.kind = RowKind::Block;
        out.line = layers.fold.bufferLine(layers.block.lineOf(ref.id, layers.fold));
        out.buffer = ByteRange{at, at};
        out.startsLine = true;
        out.endsLine = true;
        out.block = BlockId{ref.id};
        out.blockRow = ref.rowInBlock;
        out.placement = block.placement;
        out.tag = block.tag;
        return;
    }
    const WrapRow wrapped = layers.wrap.row(ref.inputRow, layers.fold);
    out.kind = RowKind::Text;
    out.line = layers.fold.bufferLine(wrapped.line);
    out.buffer =
        ByteRange{layers.fold.toBuffer(wrapped.start, Bias::Right), layers.fold.toBuffer(wrapped.end, Bias::Left)};
    out.startsLine = wrapped.startsLine;
    out.endsLine = wrapped.endsLine;
    out.block = BlockId{};
    out.blockRow = 0;
    out.placement = BlockPlacement::Below;
    out.tag = 0;
    layers.fold.appendSegments(wrapped.start, wrapped.end, out.segments);
    if (!out.segments.empty()) {
        out.buffer = {out.segments.front().buffer.begin, out.segments.back().buffer.end};
    }
    layers.inlay.appendSegments(ref.inputRow, out.buffer, out.segments);
    if (!out.segments.empty()) {
        out.buffer = {out.segments.front().buffer.begin, out.segments.back().buffer.end};
    }
}

DisplayRowCursor DisplayMap::rows(DisplayRow first) const noexcept {
    return DisplayRowCursor(this, std::min(first.value(), rowCount()));
}

TextWidth DisplayMap::xAt(DisplayPoint point) const noexcept {
    const DisplayRowInfo info = rowInfo(DisplayRow{std::min(point.row.value(), rowCount() - 1)});
    if (info.kind == RowKind::Block) {
        return 0;
    }
    const ITextShaper& shaper = *m_layers->shaper;
    TextWidth x = 0;
    forEachDisplayChar(text(), info, [&](char32_t cp, std::uint32_t column) {
        if (column >= point.column.value()) {
            return false;
        }
        x += shaper.advance(cp, x);
        return true;
    });
    return x;
}

DisplayHit DisplayMap::hitTest(DisplayRow row, TextWidth x) const noexcept {
    const DisplayRow clamped{std::min(row.value(), rowCount() - 1)};
    const DisplayRowInfo info = rowInfo(clamped);
    if (info.kind == RowKind::Block) {
        const ByteOffset offset = toOffset(DisplayPoint{clamped, DisplayColumn{0}});
        const Bias bias = info.placement == BlockPlacement::Above ? Bias::Right : Bias::Left;
        return DisplayHit{offset, bias, toDisplayPoint(offset, bias), info.block};
    }
    // Walk the clusters (a character and the Zero-width ones after it) until x falls before the
    // middle of one.
    const ITextShaper& shaper = *m_layers->shaper;
    const std::uint32_t length = displayLength(info);
    std::uint32_t column = length;
    TextWidth clusterX = 0;
    TextWidth clusterWidth = 0;
    std::uint32_t clusterStart = 0;
    bool open = false;
    bool afterJoiner = false;
    bool found = false;
    forEachDisplayChar(text(), info, [&](char32_t cp, std::uint32_t at) {
        const bool attaches = charWidth(cp) == CharWidth::Zero || afterJoiner;
        afterJoiner = cp == kZeroWidthJoiner;
        if (open && !attaches) {
            if (2 * (x - clusterX) < clusterWidth) {
                column = clusterStart;
                found = true;
                return false;
            }
            clusterX += clusterWidth;
            clusterWidth = 0;
            open = false;
        }
        if (!open) {
            open = true;
            clusterStart = at;
        }
        clusterWidth += shaper.advance(cp, clusterX + clusterWidth);
        return true;
    });
    if (!found && open && 2 * (x - clusterX) < clusterWidth) {
        column = clusterStart;
    }
    const DisplayPoint point{clamped, DisplayColumn{column}};
    const Bias bias = column == length && !info.endsLine ? Bias::Left : Bias::Right;
    return DisplayHit{toOffset(point), bias, point, std::nullopt};
}

std::uint32_t DisplayMap::nearestTextRow(std::uint32_t row, int step) const noexcept {
    const detail::BlockLayer& block = m_layers->block;
    const BlockRowRef ref = block.fromOutputRow(row);
    if (!ref.isBlock) {
        return row;
    }
    // The text rows next to a block are the input rows on either side of its boundary.
    const bool down = step > 0 ? ref.inputRow < block.inputRowCount() : ref.inputRow == 0;
    return down ? block.toOutputRow(ref.inputRow) : block.toOutputRow(ref.inputRow - 1);
}

VerticalMove DisplayMap::moveVertically(ByteOffset from, Bias bias, std::optional<TextWidth> goalX,
                                        std::int32_t rows) const noexcept {
    const DisplayPoint start = toDisplayPoint(from, bias);
    const TextWidth goal = goalX ? *goalX : xAt(start);
    const std::int64_t target =
        std::clamp<std::int64_t>(std::int64_t{start.row.value()} + rows, 0, std::int64_t{rowCount()} - 1);
    const std::uint32_t textRow = nearestTextRow(static_cast<std::uint32_t>(target), rows < 0 ? -1 : 1);
    const DisplayHit hit = hitTest(DisplayRow{textRow}, goal);
    return VerticalMove{hit.offset, hit.bias, goal, hit.point};
}

Result<void> DisplayMap::checkInvariants() const noexcept {
    const DisplayLayers& layers = *m_layers;
    if (Result<void> fold = layers.fold.checkInvariants(); !fold) {
        return fold;
    }
    if (Result<void> wrap = layers.wrap.checkInvariants(layers.fold); !wrap) {
        return wrap;
    }
    if (Result<void> inlay = layers.inlay.checkInvariants(layers.fold, layers.wrap); !inlay) {
        return inlay;
    }
    if (Result<void> block = layers.block.checkInvariants(layers.fold, layers.wrap); !block) {
        return block;
    }
    for (std::uint32_t r = 0; r < rowCount(); ++r) {
        const DisplayRowInfo info = rowInfo(DisplayRow{r});
        if (info.kind == RowKind::Block) {
            continue;
        }
        std::uint32_t expectedCol = 0;
        ByteOffset prevEnd = info.buffer.begin;
        for (const RowSegment& seg : info.segments) {
            if (seg.column.value() != expectedCol) {
                return fail(ErrorCode::Internal, "D4: segment column is not contiguous");
            }
            if (seg.buffer.begin < prevEnd || seg.buffer.end < seg.buffer.begin || seg.buffer.end > info.buffer.end) {
                return fail(ErrorCode::Internal, "D4: segment buffer range is out of order");
            }
            if (seg.kind == SegmentKind::Text) {
                if (seg.buffer.empty() || !seg.text.empty()) {
                    return fail(ErrorCode::Internal, "D4: malformed Text segment");
                }
                expectedCol += static_cast<std::uint32_t>(seg.buffer.size());
            } else if (seg.kind == SegmentKind::Inlay) {
                if (!seg.buffer.empty() || seg.text.empty()) {
                    return fail(ErrorCode::Internal, "D4: malformed Inlay segment");
                }
                expectedCol += static_cast<std::uint32_t>(seg.text.size());
            } else {
                if (seg.buffer.empty() || seg.text.empty()) {
                    return fail(ErrorCode::Internal, "D4: malformed FoldPlaceholder segment");
                }
                expectedCol += static_cast<std::uint32_t>(seg.text.size());
            }
            prevEnd = seg.buffer.end;
        }
    }
    return {};
}

namespace detail {

namespace {

void sortInlays(std::vector<InlayLayer::Inlay>& inlays) noexcept {
    std::ranges::stable_sort(inlays, [](const InlayLayer::Inlay& a, const InlayLayer::Inlay& b) {
        if (a.offset != b.offset) {
            return a.offset < b.offset;
        }
        return static_cast<std::uint8_t>(a.bias) < static_cast<std::uint8_t>(b.bias);
    });
}

} // namespace

InlayLayer::InlayLayer(std::vector<Inlay> inlays) noexcept : m_inlays(std::move(inlays)) {
    sortInlays(m_inlays);
    m_projected.reserve(m_inlays.size());
    for (const Inlay& item : m_inlays) {
        if (item.text.empty()) {
            continue;
        }
        m_projected.push_back(ProjectedInlay{
            .wrapRow = 0,
            .wrapCol = static_cast<std::uint32_t>(item.offset.value()),
            .offset = item.offset,
            .bias = item.bias,
            .text = item.text,
        });
    }
    m_projectedPrefixWidths.reserve(m_projected.size() + 1);
    m_projectedPrefixWidths.push_back(0);
    for (const ProjectedInlay& item : m_projected) {
        m_projectedPrefixWidths.push_back(m_projectedPrefixWidths.back() +
                                          static_cast<std::uint32_t>(item.text.size()));
    }
}

InlayLayer::InlayLayer(std::vector<Inlay> inlays, const FoldLayer& fold, const WrapLayer& wrap) noexcept
    : m_inlays(std::move(inlays)) {
    const Snapshot& buf = fold.buffer();
    for (Inlay& item : m_inlays) {
        item.offset = buf.clip(std::min(item.offset, buf.size()));
    }
    sortInlays(m_inlays);
    rebuildProjection(fold, wrap);
}

void InlayLayer::rebuildProjection(const FoldLayer& fold, const WrapLayer& wrap) noexcept {
    m_projected.clear();
    m_projected.reserve(m_inlays.size());
    for (const Inlay& item : m_inlays) {
        if (item.text.empty() || fold.intersects({item.offset, item.offset})) {
            continue;
        }
        const std::uint64_t folded = fold.toFold(item.offset, item.bias);
        const std::uint32_t wRow = wrap.rowAt(folded, item.bias, fold);
        const WrapRow row = wrap.row(wRow, fold);
        const auto wCol = static_cast<std::uint32_t>(std::clamp(folded, row.start, row.end) - row.start);
        m_projected.push_back(ProjectedInlay{
            .wrapRow = wRow,
            .wrapCol = wCol,
            .offset = item.offset,
            .bias = item.bias,
            .text = item.text,
        });
    }
    // Both offset mapping and the offset -> (wrap row, column) projection are monotone, so
    // sorted inlays stay sorted; rebuilding this index does not need another O(n log n) sort.
    m_projectedPrefixWidths.reserve(m_projected.size() + 1);
    m_projectedPrefixWidths.clear();
    m_projectedPrefixWidths.push_back(0);
    for (const ProjectedInlay& item : m_projected) {
        m_projectedPrefixWidths.push_back(m_projectedPrefixWidths.back() +
                                          static_cast<std::uint32_t>(item.text.size()));
    }
}

std::uint32_t InlayLayer::toInlayColumn(std::uint32_t row, std::uint32_t column, Bias bias) const noexcept {
    const auto [first, last] = std::ranges::equal_range(m_projected, row, {}, &ProjectedInlay::wrapRow);
    if (first == last) {
        return column;
    }
    const auto included = bias == Bias::Right
                              ? std::ranges::upper_bound(first, last, column, {}, &ProjectedInlay::wrapCol)
                              : std::ranges::lower_bound(first, last, column, {}, &ProjectedInlay::wrapCol);
    const auto beginIndex = static_cast<std::size_t>(first - m_projected.begin());
    const auto endIndex = static_cast<std::size_t>(included - m_projected.begin());
    return column + (m_projectedPrefixWidths[endIndex] - m_projectedPrefixWidths[beginIndex]);
}

std::uint32_t InlayLayer::toWrapColumn(std::uint32_t row, std::uint32_t column, Bias bias) const noexcept {
    const auto [first, last] = std::ranges::equal_range(m_projected, row, {}, &ProjectedInlay::wrapRow);
    if (first == last) {
        return column;
    }
    const auto beginIndex = static_cast<std::size_t>(first - m_projected.begin());
    const auto endIndex = static_cast<std::size_t>(last - m_projected.begin());
    std::size_t low = beginIndex;
    std::size_t high = endIndex;
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        const std::uint32_t start =
            m_projected[mid].wrapCol + (m_projectedPrefixWidths[mid] - m_projectedPrefixWidths[beginIndex]);
        if (start < column || (start == column && bias == Bias::Right)) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    const std::size_t candidate = low == beginIndex ? beginIndex : low - 1;
    if (candidate < endIndex) {
        const ProjectedInlay& item = m_projected[candidate];
        const std::uint32_t start =
            item.wrapCol + (m_projectedPrefixWidths[candidate] - m_projectedPrefixWidths[beginIndex]);
        const std::uint32_t finish = start + static_cast<std::uint32_t>(item.text.size());
        if (column >= start && column <= finish) {
            return item.wrapCol;
        }
        if (column > finish) {
            return column - (m_projectedPrefixWidths[candidate + 1] - m_projectedPrefixWidths[beginIndex]);
        }
    }
    return column;
}

void InlayLayer::appendSegments(std::uint32_t wrapRow, ByteRange buffer, std::vector<RowSegment>& out) const noexcept {
    if (out.empty() && !buffer.empty()) {
        out.push_back(RowSegment{SegmentKind::Text, DisplayColumn{0}, buffer, {}});
    }
    const auto [first, last] = std::ranges::equal_range(m_projected, wrapRow, {}, &ProjectedInlay::wrapRow);
    if (first == last) {
        return;
    }
    std::vector<RowSegment> merged;
    merged.reserve(out.size() + static_cast<std::size_t>(last - first) * 2);
    auto inlayIt = first;
    std::uint32_t extraCols = 0;

    for (RowSegment seg : out) {
        std::uint32_t segWrapStart = seg.column.value();
        const std::uint32_t segWrapLen = seg.kind == SegmentKind::Text ? static_cast<std::uint32_t>(seg.buffer.size())
                                                                       : static_cast<std::uint32_t>(seg.text.size());
        const std::uint32_t segWrapEnd = segWrapStart + segWrapLen;

        while (inlayIt != last && inlayIt->wrapCol <= segWrapStart) {
            merged.push_back(RowSegment{
                SegmentKind::Inlay,
                DisplayColumn{inlayIt->wrapCol + extraCols},
                ByteRange{inlayIt->offset, inlayIt->offset},
                inlayIt->text,
            });
            extraCols += static_cast<std::uint32_t>(inlayIt->text.size());
            ++inlayIt;
        }

        if (seg.kind == SegmentKind::Text) {
            while (inlayIt != last && inlayIt->wrapCol < segWrapEnd) {
                const std::uint32_t prefixBytes = inlayIt->wrapCol - segWrapStart;
                const ByteOffset splitAt = seg.buffer.begin + prefixBytes;
                if (splitAt > seg.buffer.begin) {
                    merged.push_back(RowSegment{
                        SegmentKind::Text,
                        DisplayColumn{segWrapStart + extraCols},
                        ByteRange{seg.buffer.begin, splitAt},
                        {},
                    });
                    segWrapStart = inlayIt->wrapCol;
                    seg.buffer.begin = splitAt;
                }
                merged.push_back(RowSegment{
                    SegmentKind::Inlay,
                    DisplayColumn{inlayIt->wrapCol + extraCols},
                    ByteRange{inlayIt->offset, inlayIt->offset},
                    inlayIt->text,
                });
                extraCols += static_cast<std::uint32_t>(inlayIt->text.size());
                ++inlayIt;
            }
            if (!seg.buffer.empty()) {
                seg.column = DisplayColumn{segWrapStart + extraCols};
                merged.push_back(seg);
            }
        } else {
            seg.column = DisplayColumn{segWrapStart + extraCols};
            merged.push_back(seg);
        }
    }

    while (inlayIt != last) {
        merged.push_back(RowSegment{
            SegmentKind::Inlay,
            DisplayColumn{inlayIt->wrapCol + extraCols},
            ByteRange{inlayIt->offset, inlayIt->offset},
            inlayIt->text,
        });
        extraCols += static_cast<std::uint32_t>(inlayIt->text.size());
        ++inlayIt;
    }

    out = std::move(merged);
}

InlayLayer InlayLayer::updated(std::span<const SpanEdit> rows, std::vector<SpanEdit>& out) const noexcept {
    out.insert(out.end(), rows.begin(), rows.end());
    return *this;
}

InlayLayer InlayLayer::updated(std::span<const RawEdit> edits, const FoldLayer& fold, const WrapLayer& wrap,
                               std::span<const SpanEdit> rows, std::vector<SpanEdit>& out) const noexcept {
    out.insert(out.end(), rows.begin(), rows.end());
    if (m_inlays.empty()) {
        return *this;
    }
    std::vector<Inlay> mapped = m_inlays;
    for (Inlay& item : mapped) {
        item.offset = mapOffset(item.offset, item.bias, edits);
    }
    const auto less = [](const Inlay& a, const Inlay& b) {
        if (a.offset != b.offset) {
            return a.offset < b.offset;
        }
        return static_cast<std::uint8_t>(a.bias) < static_cast<std::uint8_t>(b.bias);
    };
    if (!std::ranges::is_sorted(mapped, less)) {
        sortInlays(mapped);
    }
    InlayLayer next;
    next.m_inlays = std::move(mapped);
    next.rebuildProjection(fold, wrap);
    return next;
}

Result<void> InlayLayer::checkInvariants(const FoldLayer& fold, const WrapLayer& wrap) const noexcept {
    for (std::size_t i = 0; i < m_inlays.size(); ++i) {
        if (m_inlays[i].offset > fold.buffer().size() || !fold.buffer().isCharBoundary(m_inlays[i].offset)) {
            return fail(ErrorCode::Internal, "D4: inlay offset is out of range or not a char boundary");
        }
        if (i > 0) {
            const Inlay& prev = m_inlays[i - 1];
            const Inlay& cur = m_inlays[i];
            if (cur.offset < prev.offset || (cur.offset == prev.offset && static_cast<std::uint8_t>(cur.bias) <
                                                                              static_cast<std::uint8_t>(prev.bias))) {
                return fail(ErrorCode::Internal, "D4: inlays are not sorted by (offset, bias)");
            }
        }
    }
    for (std::size_t i = 0; i < m_projected.size(); ++i) {
        const ProjectedInlay& proj = m_projected[i];
        if (proj.wrapRow >= wrap.rowCount()) {
            return fail(ErrorCode::Internal, "D4: projected inlay wrapRow out of bounds");
        }
        const WrapRow row = wrap.row(proj.wrapRow, fold);
        if (proj.wrapCol > row.end - row.start) {
            return fail(ErrorCode::Internal, "D4: projected inlay wrapCol out of bounds");
        }
        const std::uint32_t leftCol = toInlayColumn(proj.wrapRow, proj.wrapCol, Bias::Left);
        const std::uint32_t rightCol = toInlayColumn(proj.wrapRow, proj.wrapCol, Bias::Right);
        if (toWrapColumn(proj.wrapRow, leftCol, Bias::Left) != proj.wrapCol ||
            toWrapColumn(proj.wrapRow, rightCol, Bias::Right) != proj.wrapCol) {
            return fail(ErrorCode::Internal, "D4: inlay column round-trip failed");
        }
        if (i > 0) {
            const ProjectedInlay& prev = m_projected[i - 1];
            if (proj.wrapRow < prev.wrapRow || (proj.wrapRow == prev.wrapRow && proj.wrapCol < prev.wrapCol)) {
                return fail(ErrorCode::Internal, "D4: projected inlays are not sorted");
            }
        }
    }
    return {};
}

} // namespace detail

} // namespace frost::text
