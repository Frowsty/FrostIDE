// SPDX-License-Identifier: Apache-2.0
//
// The independent oracles of the frost::diff tests (oracles.hpp). Deliberately simple and written
// from the format descriptions, not from frost::diff's code.

#include "diff/support/oracles.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>

namespace frost::diff::test {

std::vector<std::string> naiveSplit(std::string_view text, LineSplit split) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        current += c;
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c == '\r' && split == LineSplit::Lsp) {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                current += '\n';
                ++i;
            }
            lines.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

void naiveIds(const std::vector<std::string>& a, const std::vector<std::string>& b, std::vector<std::uint32_t>& idsA,
              std::vector<std::uint32_t>& idsB) {
    std::map<std::string, std::uint32_t> ids;
    const auto id = [&](const std::string& line) {
        const auto [it, inserted] = ids.emplace(line, static_cast<std::uint32_t>(ids.size()));
        return it->second;
    };
    idsA.clear();
    idsB.clear();
    for (const std::string& line : a) {
        idsA.push_back(id(line));
    }
    for (const std::string& line : b) {
        idsB.push_back(id(line));
    }
}

std::uint64_t lcsLength(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b) {
    std::vector<std::uint64_t> previous(b.size() + 1, 0);
    std::vector<std::uint64_t> row(b.size() + 1, 0);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        for (std::size_t j = 1; j <= b.size(); ++j) {
            row[j] = a[i - 1] == b[j - 1] ? previous[j - 1] + 1 : std::max(previous[j], row[j - 1]);
        }
        std::swap(previous, row);
    }
    return previous[b.size()];
}

std::uint64_t editDistance(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b) {
    return a.size() + b.size() - 2 * lcsLength(a, b);
}

Result<std::string> applyHunks(const std::vector<std::string>& linesA, const std::vector<std::string>& linesB,
                               std::span<const Hunk> hunks) {
    std::string out;
    std::size_t i = 0;
    for (const Hunk& hunk : hunks) {
        if (hunk.a.begin.value() < i || hunk.a.end < hunk.a.begin || hunk.a.end.value() > linesA.size() ||
            hunk.b.end < hunk.b.begin || hunk.b.end.value() > linesB.size()) {
            return fail(ErrorCode::InvalidArgument, "hunk out of order or out of bounds");
        }
        for (; i < hunk.a.begin.value(); ++i) {
            out += linesA[i];
        }
        for (std::size_t j = hunk.b.begin.value(); j < hunk.b.end.value(); ++j) {
            out += linesB[j];
        }
        i = hunk.a.end.value();
    }
    for (; i < linesA.size(); ++i) {
        out += linesA[i];
    }
    return out;
}

namespace {

// "-l,s" or "+l" after the sign: line and count (count 1 when omitted).
bool parseRange(std::string_view text, std::uint64_t& line, std::uint64_t& count) {
    const std::size_t comma = text.find(',');
    const std::string_view first = text.substr(0, comma);
    if (std::from_chars(first.data(), first.data() + first.size(), line).ec != std::errc() || first.empty()) {
        return false;
    }
    count = 1;
    if (comma != std::string_view::npos) {
        const std::string_view second = text.substr(comma + 1);
        if (second.empty() || std::from_chars(second.data(), second.data() + second.size(), count).ec != std::errc()) {
            return false;
        }
    }
    return true;
}

struct PatchLine {
    std::string_view text; // Without its LF.
    bool hasNewline = false;
};

std::vector<PatchLine> splitLf(std::string_view text) {
    std::vector<PatchLine> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t lf = text.find('\n', start);
        if (lf == std::string_view::npos) {
            lines.push_back(PatchLine{text.substr(start), false});
            break;
        }
        lines.push_back(PatchLine{text.substr(start, lf - start), true});
        start = lf + 1;
    }
    return lines;
}

} // namespace

Result<std::string> applyUnified(std::string_view original, std::string_view patch) {
    // The original as LF lines, each keeping its LF (the last may lack one).
    std::vector<std::string> source;
    for (const PatchLine& line : splitLf(original)) {
        source.push_back(std::string(line.text) + (line.hasNewline ? "\n" : ""));
    }
    const std::vector<PatchLine> lines = splitLf(patch);
    std::string out;
    std::size_t cursor = 0; // Next source line to copy.
    std::size_t k = 0;
    while (k < lines.size() && !lines[k].text.starts_with("@@ ")) {
        if (!lines[k].text.starts_with("--- ") && !lines[k].text.starts_with("+++ ")) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("unexpected line before the first hunk: {}", lines[k].text));
        }
        ++k;
    }
    while (k < lines.size()) {
        const std::string_view header = lines[k].text;
        if (!header.starts_with("@@ -")) {
            return fail(ErrorCode::InvalidArgument, std::format("expected a hunk header: {}", header));
        }
        const std::size_t plus = header.find(" +", 4);
        const std::size_t close = plus == std::string_view::npos ? plus : header.find(" @@", plus + 2);
        std::uint64_t oldLine = 0, oldCount = 0, newLine = 0, newCount = 0;
        if (close == std::string_view::npos || !parseRange(header.substr(4, plus - 4), oldLine, oldCount) ||
            !parseRange(header.substr(plus + 2, close - plus - 2), newLine, newCount)) {
            return fail(ErrorCode::InvalidArgument, std::format("malformed hunk header: {}", header));
        }
        ++k;
        // POSIX: with a count of 0 the line number names the line after which the change applies.
        if (oldCount != 0 && oldLine == 0) {
            return fail(ErrorCode::InvalidArgument, std::format("malformed hunk header: {}", header));
        }
        const std::size_t at = oldCount == 0 ? oldLine : oldLine - 1;
        if (at < cursor || at + oldCount > source.size()) {
            return fail(ErrorCode::InvalidArgument, std::format("hunk at line {} is out of order or range", oldLine));
        }
        for (; cursor < at; ++cursor) {
            out += source[cursor];
        }
        // The hunk's old lines (context and '-') and new lines (context and '+'), rebuilt from
        // the body; a "\ No newline at end of file" line removes the LF of the line before it.
        std::string expectedOld;
        std::string replacement;
        std::uint64_t seenOld = 0;
        std::uint64_t seenNew = 0;
        bool lastOld = false; // Whether the previous body line went to expectedOld / replacement.
        bool lastNew = false;
        while (k < lines.size()) {
            const std::string_view body = lines[k].text;
            if (body.starts_with('\\')) {
                if (!lastOld && !lastNew) {
                    return fail(ErrorCode::InvalidArgument, "misplaced \"\\ No newline at end of file\"");
                }
                if (lastOld) {
                    expectedOld.pop_back();
                }
                if (lastNew) {
                    replacement.pop_back();
                }
                lastOld = lastNew = false;
                ++k;
                continue;
            }
            if (seenOld == oldCount && seenNew == newCount) {
                break;
            }
            if (body.empty() || (body.front() != ' ' && body.front() != '-' && body.front() != '+')) {
                return fail(ErrorCode::InvalidArgument, std::format("bad body line: '{}'", body));
            }
            const std::string content = std::string(body.substr(1)) + "\n";
            lastOld = body.front() != '+';
            lastNew = body.front() != '-';
            if (lastOld) {
                expectedOld += content;
                ++seenOld;
            }
            if (lastNew) {
                replacement += content;
                ++seenNew;
            }
            ++k;
        }
        if (seenOld != oldCount || seenNew != newCount) {
            return fail(ErrorCode::InvalidArgument, std::format("hunk at line {} is truncated", oldLine));
        }
        std::string actualOld;
        for (std::size_t i = at; i < at + oldCount; ++i) {
            actualOld += source[i];
        }
        if (actualOld != expectedOld) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("hunk at line {} does not match the original", oldLine));
        }
        out += replacement;
        cursor = at + oldCount;
    }
    for (; cursor < source.size(); ++cursor) {
        out += source[cursor];
    }
    return out;
}

Result<std::vector<MarkedRegion>> parseMarkers(std::string_view text, std::uint32_t size, LineSplit split) {
    // Lines with their breaks.
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' || (text[i] == '\r' && split == LineSplit::Lsp)) {
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            lines.push_back(text.substr(start, i + 1 - start));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        lines.push_back(text.substr(start));
    }
    // A marker line of character c: exactly `size` of c, then a space or the line's end; returns
    // the label (the rest without the break), or false.
    const auto marker = [&](std::string_view line, char c, std::string& label) {
        if (line.size() < size || line.find_first_not_of(c) < size) {
            return false;
        }
        std::string_view rest = line.substr(size);
        while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) {
            rest.remove_suffix(1);
        }
        if (rest.empty()) {
            label.clear();
            return line.size() == size || line[size] == '\n' || line[size] == '\r';
        }
        if (rest.front() != ' ') {
            return false;
        }
        label = std::string(rest.substr(1));
        return true;
    };
    std::vector<MarkedRegion> regions;
    enum class State { Plain, Ours, Base, Theirs } state = State::Plain;
    MarkedRegion current;
    std::string label;
    for (const std::string_view line : lines) {
        switch (state) {
        case State::Plain:
            if (marker(line, '<', label)) {
                if (!current.text.empty()) {
                    regions.push_back(std::move(current));
                }
                current = MarkedRegion{};
                current.conflict = true;
                current.oursLabel = label;
                state = State::Ours;
            } else if (marker(line, '|', label) || marker(line, '=', label) || marker(line, '>', label)) {
                return fail(ErrorCode::InvalidArgument, "a conflict marker outside a conflict");
            } else {
                current.text += line;
            }
            break;
        case State::Ours:
            if (marker(line, '|', label)) {
                current.baseLabel = label;
                state = State::Base;
            } else if (marker(line, '<', label) || marker(line, '=', label) || marker(line, '>', label)) {
                return fail(ErrorCode::InvalidArgument, "a marker out of order in the ours section");
            } else {
                current.ours += line;
            }
            break;
        case State::Base:
            if (marker(line, '=', label)) {
                state = State::Theirs;
            } else if (marker(line, '<', label) || marker(line, '|', label) || marker(line, '>', label)) {
                return fail(ErrorCode::InvalidArgument, "a marker out of order in the base section");
            } else {
                current.base += line;
            }
            break;
        case State::Theirs:
            if (marker(line, '>', label)) {
                current.theirsLabel = label;
                regions.push_back(std::move(current));
                current = MarkedRegion{};
                state = State::Plain;
            } else if (marker(line, '<', label) || marker(line, '|', label) || marker(line, '=', label)) {
                return fail(ErrorCode::InvalidArgument, "a marker out of order in the theirs section");
            } else {
                current.theirs += line;
            }
            break;
        }
    }
    if (state != State::Plain) {
        return fail(ErrorCode::InvalidArgument, "unterminated conflict");
    }
    if (!current.text.empty()) {
        regions.push_back(std::move(current));
    }
    return regions;
}

} // namespace frost::diff::test
