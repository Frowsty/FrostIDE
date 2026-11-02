// SPDX-License-Identifier: Apache-2.0
//
// MergePanes (frost/ui/nav/merge_layout.hpp): one display map per side of the merge with filler
// blocks that align the chunks.

#include "frost/ui/nav/merge_layout.hpp"

#include "frost/core/assert.hpp"
#include "frost/text/text_buffer.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace frost::ui::nav {
namespace {

[[nodiscard]] const diff::LineSequence& sideOf(const diff::MergeModel& model, MergePane pane) noexcept {
    switch (pane) {
    case MergePane::Yours: return model.ours();
    case MergePane::Base: return model.base();
    case MergePane::Disk: return model.theirs();
    }
    return model.base();
}

[[nodiscard]] diff::LineRange rangeOf(const diff::MergeChunk& chunk, MergePane pane) noexcept {
    switch (pane) {
    case MergePane::Yours: return chunk.ours;
    case MergePane::Base: return chunk.base;
    case MergePane::Disk: return chunk.theirs;
    }
    return chunk.base;
}

constexpr std::array kPanes{MergePane::Yours, MergePane::Base, MergePane::Disk};

} // namespace

Result<MergePanes> MergePanes::create(const diff::MergeModel& model, const text::ITextShaper& shaper) {
    MergePanes panes;
    panes.m_chunks = model.merge().chunks;
    for (const MergePane pane : kPanes) {
        const diff::LineSequence& side = sideOf(model, pane);
        auto buffer = text::TextBuffer::fromUtf8(std::string(side.bytes()));
        if (!buffer) {
            return std::unexpected(std::move(buffer.error()));
        }
        text::DisplayMap map(buffer->snapshot(), shaper);
        const std::size_t p = index(pane);
        panes.m_chunkOfLine[p].assign(side.size(), 0);
        panes.m_fillers[p].assign(panes.m_chunks.size(), std::nullopt);
        const std::string_view bytes = side.bytes();
        // An empty side, or one ending with a break, shows a trailing empty line: fillers go above it.
        const bool endsWithBreak = bytes.empty() || bytes.back() == '\n' || bytes.back() == '\r';
        for (std::size_t c = 0; c < panes.m_chunks.size(); ++c) {
            const diff::MergeChunk& chunk = panes.m_chunks[c];
            const diff::LineRange range = rangeOf(chunk, pane);
            for (LineIndex line = range.begin; line < range.end; ++line) {
                panes.m_chunkOfLine[p][line.value()] = static_cast<std::uint32_t>(c);
            }
            const std::uint32_t height = std::max({chunk.ours.size(), chunk.base.size(), chunk.theirs.size()});
            const std::uint32_t deficit = height - range.size();
            if (deficit == 0) {
                continue;
            }
            text::BlockDecoration filler;
            filler.rows = deficit;
            filler.tag = c + 1;
            if (!range.empty()) {
                filler.at = side.lineRange(range.end - 1).begin;
                filler.placement = text::BlockPlacement::Below;
            } else if (range.begin.value() < side.size()) {
                filler.at = side.lineRange(range.begin).begin;
                filler.placement = text::BlockPlacement::Above;
            } else {
                filler.at = ByteOffset{bytes.size()};
                filler.placement = endsWithBreak ? text::BlockPlacement::Above : text::BlockPlacement::Below;
            }
            auto added = map.addBlock(filler);
            if (!added) {
                return std::unexpected(std::move(added.error()));
            }
            panes.m_fillers[p][c] = *added;
            panes.m_fillerRows[p] += deficit;
        }
        panes.m_maps.push_back(std::move(map));
    }
    return panes;
}

std::uint32_t MergePanes::rowCount() const noexcept {
    std::uint32_t rows = 0;
    for (const text::DisplayMap& map : m_maps) {
        rows = std::max(rows, map.rowCount());
    }
    return rows;
}

text::DisplayRow MergePanes::chunkRow(MergePane pane, std::size_t chunk) const {
    FROST_PRE(chunk < m_chunks.size());
    const std::size_t p = index(pane);
    const diff::LineRange range = rangeOf(m_chunks[chunk], pane);
    const text::DisplayMap& map = m_maps[p];
    if (!range.empty()) {
        // The first line's start in the pane's own text: count the bytes of the lines before it.
        const auto offset = map.text().offsetAt(text::Point{range.begin, ByteColumn{0}});
        if (offset) {
            return map.toDisplayPoint(*offset).row;
        }
    }
    if (const std::optional<text::BlockId>& filler = m_fillers[p][chunk]; filler) {
        if (auto info = map.blockInfo(*filler); info) {
            return info->row;
        }
    }
    return text::DisplayRow{map.rowCount() - 1};
}

std::optional<std::size_t> MergePanes::chunkAt(MergePane pane, text::DisplayRow row) const {
    const std::size_t p = index(pane);
    const text::DisplayMap& map = m_maps[p];
    if (row.value() >= map.rowCount()) {
        return std::nullopt;
    }
    const text::DisplayRowInfo info = map.rowInfo(row);
    if (info.kind == text::RowKind::Block) {
        return info.tag == 0 ? std::nullopt : std::optional<std::size_t>(info.tag - 1);
    }
    if (info.line.value() >= m_chunkOfLine[p].size()) {
        return std::nullopt;
    }
    return m_chunkOfLine[p][info.line.value()];
}

} // namespace frost::ui::nav
