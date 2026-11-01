// SPDX-License-Identifier: Apache-2.0
//
// The line patch of a ChangeSet, and the identity fold and inlay layers (display_layers.hpp).

#include "display_layers.hpp"

#include <cstddef>

namespace frost::text::detail {

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
    const Result<LineInfo> info = m_text.lineInfo(LineIndex{line});
    FROST_ASSERT(info.has_value());
    return toFoldLine(*info);
}

std::uint32_t FoldLayer::lineAt(std::uint64_t offset) const noexcept {
    return m_text.lineAt(ByteOffset{offset}).value();
}

std::uint64_t FoldLayer::clip(std::uint64_t offset) const noexcept { return m_text.clip(ByteOffset{offset}).value(); }

std::uint64_t FoldLayer::toFold(ByteOffset offset, Bias /*bias*/) const noexcept { return offset.value(); }

ByteOffset FoldLayer::toBuffer(std::uint64_t offset, Bias /*bias*/) const noexcept { return ByteOffset{offset}; }

FoldLayer FoldLayer::updated(std::span<const TextChange> changes, Snapshot newText,
                             std::vector<SpanEdit>& patch) const noexcept {
    const std::vector<SpanEdit> lines = composeLinePatch(changes);
    patch.insert(patch.end(), lines.begin(), lines.end());
    return FoldLayer(std::move(newText));
}

// --- InlayLayer ---------------------------------------------------------------------------------

void InlayLayer::appendSegments(std::uint32_t /*wrapRow*/, ByteRange buffer,
                                std::vector<RowSegment>& out) const noexcept {
    if (!buffer.empty()) {
        out.push_back(RowSegment{SegmentKind::Text, DisplayColumn{0}, buffer, {}});
    }
}

InlayLayer InlayLayer::updated(std::span<const SpanEdit> rows, std::vector<SpanEdit>& out) const noexcept {
    out.insert(out.end(), rows.begin(), rows.end());
    return *this;
}

} // namespace frost::text::detail
