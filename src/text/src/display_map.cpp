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

// Calls fn(char32_t cp, std::uint32_t column) for the code points of text row `row` in display
// order, until fn returns false.
template<class F>
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

    // Up the stack: buffer lines -> fold lines -> wrap rows -> inlay rows -> display rows.
    std::vector<SpanEdit> lines;
    detail::FoldLayer fold = layers.fold.updated(changes.changes, text, lines);
    std::int64_t lineCount = layers.fold.lineCount();
    for (const SpanEdit& edit : lines) {
        lineCount += (std::int64_t{edit.newEnd} - edit.newFirst) - (std::int64_t{edit.oldEnd} - edit.oldFirst);
    }
    FROST_ASSERT(lineCount == fold.lineCount());
    std::vector<SpanEdit> wrapRows;
    detail::WrapLayer wrap = layers.wrap.updated(layers.fold, fold, lines, *layers.shaper, wrapRows);
    std::vector<SpanEdit> inlayRows;
    const detail::InlayLayer inlay = layers.inlay.updated(wrapRows, inlayRows);
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
    layers.fold = detail::FoldLayer(std::move(text));
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
    layers.block.rebuild(layers.fold, layers.wrap);
    return wholeEdit(oldRows);
}

std::vector<RowEdit> DisplayMap::setShaper(const ITextShaper& shaper) noexcept {
    m_layers->shaper = &shaper;
    return setWrap(m_layers->settings);
}

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
    return layers.fold.toBuffer(folded, Bias::Left);
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
    layers.inlay.appendSegments(ref.inputRow, out.buffer, out.segments);
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
    if (Result<void> wrap = layers.wrap.checkInvariants(layers.fold); !wrap) {
        return wrap;
    }
    return layers.block.checkInvariants(layers.fold, layers.wrap);
}

} // namespace frost::text
