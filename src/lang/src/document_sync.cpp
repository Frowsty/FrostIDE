// SPDX-License-Identifier: Apache-2.0
//
// Document synchronisation helpers (document_sync.hpp): encodings, positions and didChange
// content changes from ChangeSets.

#include "frost/lang/document_sync.hpp"

#include <limits>

namespace frost::lang {
namespace {

[[nodiscard]] std::optional<std::uint32_t> nonNegative32(const Json* json) noexcept {
    if (json == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> value = json->asInteger();
    if (!value || *value < 0 || *value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

[[nodiscard]] std::vector<ContentChange> fullText(const text::Snapshot& after) {
    return {ContentChange{.range = std::nullopt, .text = after.text()}};
}

[[nodiscard]] char lowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

} // namespace

std::string_view toString(PositionEncoding encoding) noexcept {
    return encoding == PositionEncoding::Utf8 ? "utf-8" : "utf-16";
}

std::optional<PositionEncoding> parsePositionEncoding(std::string_view kind) noexcept {
    if (kind == "utf-8") {
        return PositionEncoding::Utf8;
    }
    if (kind == "utf-16") {
        return PositionEncoding::Utf16;
    }
    return std::nullopt;
}

Json toJson(LspPosition position) {
    return Json::object().set("line", position.line).set("character", position.character);
}

Json toJson(const LspRange& range) {
    return Json::object().set("start", toJson(range.start)).set("end", toJson(range.end));
}

std::optional<LspPosition> parsePosition(const Json& json) {
    const std::optional<std::uint32_t> line = nonNegative32(json.find("line"));
    const std::optional<std::uint32_t> character = nonNegative32(json.find("character"));
    if (!line || !character) {
        return std::nullopt;
    }
    return LspPosition{.line = *line, .character = *character};
}

std::optional<LspRange> parseRange(const Json& json) {
    const Json* start = json.find("start");
    const Json* end = json.find("end");
    if (start == nullptr || end == nullptr) {
        return std::nullopt;
    }
    const std::optional<LspPosition> a = parsePosition(*start);
    const std::optional<LspPosition> b = parsePosition(*end);
    if (!a || !b) {
        return std::nullopt;
    }
    return LspRange{.start = *a, .end = *b};
}

LspPosition toLspPosition(const text::Snapshot& snapshot, ByteOffset offset, PositionEncoding encoding) noexcept {
    if (encoding == PositionEncoding::Utf8) {
        const text::Point point = snapshot.pointAt(offset);
        return LspPosition{.line = point.line.value(), .character = point.column.value()};
    }
    const text::Utf16Point point = snapshot.utf16PointAt(offset);
    return LspPosition{.line = point.line.value(), .character = point.column.value()};
}

Result<ByteOffset> fromLspPosition(const text::Snapshot& snapshot, LspPosition position,
                                   PositionEncoding encoding) noexcept {
    if (encoding == PositionEncoding::Utf8) {
        return snapshot.offsetAt(
            text::Point{.line = LineIndex{position.line}, .column = ByteColumn{position.character}});
    }
    return snapshot.offsetAt(
        text::Utf16Point{.line = LineIndex{position.line}, .column = Utf16Column{position.character}});
}

Json toJson(const ContentChange& change) {
    Json out = Json::object();
    if (change.range) {
        out.set("range", toJson(*change.range));
    }
    out.set("text", change.text);
    return out;
}

std::vector<ContentChange> contentChanges(const text::ChangeSet& changes, const text::Snapshot& before,
                                          const text::Snapshot& after, PositionEncoding encoding) {
    if (changes.changes.empty() || changes.baseVersion == changes.version) {
        return {};
    }
    const std::vector<text::TextChange>& steps = changes.changes;
    // Shape checks: descending, non-overlapping steps whose deltas explain the size change.
    std::int64_t totalDelta = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const text::TextChange& step = steps[i];
        if (step.start > step.oldEnd || step.start > step.newEnd) {
            return fullText(after);
        }
        if (i + 1 < steps.size() && steps[i + 1].oldEnd > step.start) {
            return fullText(after);
        }
        totalDelta += static_cast<std::int64_t>(step.newEnd.value()) - static_cast<std::int64_t>(step.oldEnd.value());
    }
    if (static_cast<std::int64_t>(before.size().value()) + totalDelta !=
        static_cast<std::int64_t>(after.size().value())) {
        return fullText(after);
    }
    std::vector<ContentChange> out;
    out.reserve(steps.size());
    // later[i] = the size delta of the steps applied after step i (all of which lie before it).
    std::int64_t later = totalDelta;
    for (const text::TextChange& step : steps) {
        later -= static_cast<std::int64_t>(step.newEnd.value()) - static_cast<std::int64_t>(step.oldEnd.value());
        const std::int64_t begin = static_cast<std::int64_t>(step.start.value()) + later;
        const std::int64_t end = static_cast<std::int64_t>(step.newEnd.value()) + later;
        if (begin < 0 || end < begin || static_cast<std::uint64_t>(end) > after.size().value() ||
            step.oldEnd > before.size()) {
            return fullText(after);
        }
        LspRange range;
        if (encoding == PositionEncoding::Utf8) {
            range.start =
                LspPosition{.line = step.startPoint.line.value(), .character = step.startPoint.column.value()};
            range.end =
                LspPosition{.line = step.oldEndPoint.line.value(), .character = step.oldEndPoint.column.value()};
        } else {
            range.start = toLspPosition(before, step.start, encoding);
            range.end = toLspPosition(before, step.oldEnd, encoding);
        }
        out.push_back(
            ContentChange{.range = range,
                          .text = after.text(text::ByteRange{.begin = ByteOffset{static_cast<std::uint64_t>(begin)},
                                                             .end = ByteOffset{static_cast<std::uint64_t>(end)}})});
    }
    return out;
}

std::string_view languageIdForPath(std::string_view path) noexcept {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t separator = path.find_last_of("/\\");
    if (dot == std::string_view::npos || (separator != std::string_view::npos && dot < separator)) {
        return "cpp";
    }
    std::string extension;
    for (char c : path.substr(dot + 1)) {
        extension += lowerAscii(c);
    }
    if (extension == "c") {
        return "c";
    }
    if (extension == "m") {
        return "objective-c";
    }
    if (extension == "mm") {
        return "objective-cpp";
    }
    if (extension == "cu" || extension == "cuh") {
        return "cuda";
    }
    return "cpp";
}

} // namespace frost::lang
