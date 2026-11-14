// SPDX-License-Identifier: Apache-2.0
//
// Internal EditorView helpers shared across editor_view*.cpp.

#pragma once

#include "frost/text/editor/editor.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include <QString>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui {

// The MIME type that marks a clipboard line copy (copy or cut with no selection), so paste puts
// it above the caret's line as IntelliJ does.
inline constexpr const char* kLineCopyMime = "application/x-frostide-line-copy";

[[nodiscard]] inline bool candidateLess(text::ByteRange a, text::ByteRange b) noexcept {
    return a.begin < b.begin || (a.begin == b.begin && a.end > b.end);
}

// Keep cancellation responsive even when a generated file contains many candidate regions.
[[nodiscard]] inline bool orderCandidates(std::vector<text::ByteRange>& ranges, const std::stop_token& stop) {
    constexpr std::size_t chunk = 64;
    for (std::size_t first = 0; first < ranges.size(); first += chunk) {
        if (stop.stop_requested())
            return false;
        const auto end = std::min(first + chunk, ranges.size());
        std::sort(ranges.begin() + static_cast<std::ptrdiff_t>(first),
                  ranges.begin() + static_cast<std::ptrdiff_t>(end), candidateLess);
    }
    std::vector<text::ByteRange> scratch(ranges.size());
    for (std::size_t width = chunk; width < ranges.size(); width *= 2) {
        for (std::size_t first = 0; first < ranges.size(); first += width * 2) {
            const auto mid = std::min(first + width, ranges.size());
            const auto end = std::min(mid + width, ranges.size());
            auto left = first;
            auto right = mid;
            for (auto out = first; out < end; ++out) {
                if ((out % chunk) == 0 && stop.stop_requested())
                    return false;
                scratch[out] = right == end || (left < mid && !candidateLess(ranges[right], ranges[left]))
                                   ? ranges[left++]
                                   : ranges[right++];
            }
        }
        ranges.swap(scratch);
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if ((i % chunk) == 0 && stop.stop_requested())
            return false;
        if (kept == 0 || ranges[i] != ranges[kept - 1])
            ranges[kept++] = ranges[i];
    }
    ranges.resize(kept);
    return !stop.stop_requested();
}

inline std::uint64_t buildCandidateMax(const std::vector<text::ByteRange>& ranges, std::vector<std::uint64_t>& tree,
                                       std::size_t node, std::size_t first, std::size_t end,
                                       const std::stop_token& stop) {
    if (stop.stop_requested())
        return 0;
    if (end - first == 1)
        return tree[node] = ranges[first].end.value();
    const auto mid = first + (end - first) / 2;
    return tree[node] = std::max(buildCandidateMax(ranges, tree, node * 2, first, mid, stop),
                                 buildCandidateMax(ranges, tree, node * 2 + 1, mid, end, stop));
}

[[nodiscard]] inline std::optional<std::size_t> findCandidate(const std::vector<std::uint64_t>& tree, std::size_t node,
                                                              std::size_t first, std::size_t end, std::size_t limit,
                                                              std::uint64_t offset) noexcept {
    if (first >= limit || tree[node] < offset)
        return std::nullopt;
    if (end - first == 1)
        return first;
    const auto mid = first + (end - first) / 2;
    if (auto right = findCandidate(tree, node * 2 + 1, mid, end, limit, offset))
        return right;
    return findCandidate(tree, node * 2, first, mid, limit, offset);
}

[[nodiscard]] inline text::LineBreak lineBreakFor(const std::string& setting) noexcept {
    if (setting == "lf") {
        return text::LineBreak::Lf;
    }
    if (setting == "crlf") {
        return text::LineBreak::CrLf;
    }
    if (setting == "cr") {
        return text::LineBreak::Cr;
    }
#ifdef _WIN32
    return text::LineBreak::CrLf;
#else
    return text::LineBreak::Lf;
#endif
}

[[nodiscard]] inline std::string_view breakText(text::LineBreak lineBreak) noexcept {
    switch (lineBreak) {
    case text::LineBreak::Lf: return "\n";
    case text::LineBreak::Cr: return "\r";
    case text::LineBreak::CrLf: return "\r\n";
    case text::LineBreak::None: break;
    }
    return "\n";
}

[[nodiscard]] inline std::string breakToken(text::LineBreak lineBreak) {
    switch (lineBreak) {
    case text::LineBreak::Cr: return "CR";
    case text::LineBreak::CrLf: return "CRLF";
    case text::LineBreak::Lf:
    case text::LineBreak::None: break;
    }
    return "LF";
}

[[nodiscard]] inline text::EditorSettings modelSettings(const EditorViewSettings& settings) noexcept {
    text::EditorSettings out;
    out.tabSize = std::max<std::uint32_t>(settings.tabSize, 1);
    out.indentSize = std::max<std::uint32_t>(settings.indentSize, 1);
    out.insertSpaces = settings.insertSpaces;
    out.defaultLineBreak = lineBreakFor(settings.lineSeparator);
    return out;
}

[[nodiscard]] inline std::string utf8(const QString& text) { return text.toStdString(); }

// `text` with every line break (CRLF, CR, LF) replaced by `lineBreak`.
[[nodiscard]] inline std::string normaliseBreaks(std::string_view text, std::string_view lineBreak) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            out += lineBreak;
        } else if (text[i] == '\n') {
            out += lineBreak;
        } else {
            out += text[i];
        }
    }
    return out;
}

} // namespace frost::ui
