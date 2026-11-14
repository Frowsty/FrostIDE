// SPDX-License-Identifier: Apache-2.0
//
// The golden-file readers of the frost::diff tests (goldens.hpp).

#include "diff/support/goldens.hpp"

#include "diff/support/files.hpp"
#include "support/test_env.hpp"

#include <charconv>
#include <format>

namespace frost::diff::test {
namespace {

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1); // Goldens are LF files; tolerate a CRLF checkout.
        }
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

Result<HunkTuple> parseHunkLine(std::string_view line) {
    HunkTuple hunk{};
    std::size_t at = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        while (at < line.size() && line[at] == ' ') {
            ++at;
        }
        const auto [end, ec] = std::from_chars(line.data() + at, line.data() + line.size(), hunk[k]);
        if (ec != std::errc()) {
            return fail(ErrorCode::Corrupt, std::format("bad hunk line '{}'", line));
        }
        at = static_cast<std::size_t>(end - line.data());
    }
    if (at != line.size()) {
        return fail(ErrorCode::Corrupt, std::format("bad hunk line '{}'", line));
    }
    return hunk;
}

bool skippable(std::string_view line) { return line.empty() || line.front() == '#'; }

template<class Golden, class Parse>
Result<std::vector<Golden>> loadAll(std::string_view directory, Parse parse) {
    auto names = listNames(directory, false);
    if (!names) {
        return std::unexpected(std::move(names.error()));
    }
    std::vector<Golden> goldens;
    for (const std::string& name : *names) {
        if (!name.ends_with(".hunks")) {
            continue;
        }
        auto text = readBinaryFile(joinUtf8(directory, name));
        if (!text) {
            return std::unexpected(std::move(text.error()));
        }
        auto golden = parse(name.substr(0, name.size() - 6), *text);
        if (!golden) {
            return std::unexpected(std::move(golden.error().withContext(name)));
        }
        goldens.push_back(std::move(*golden));
    }
    return goldens;
}

} // namespace

HunkList hunkTuples(const LineDiff& diff) {
    HunkList out;
    out.reserve(diff.hunks.size());
    for (const Hunk& hunk : diff.hunks) {
        out.push_back({hunk.a.begin.value(), hunk.a.end.value(), hunk.b.begin.value(), hunk.b.end.value()});
    }
    return out;
}

std::string formatHunks(const HunkList& hunks) {
    std::string out = "[";
    for (std::size_t k = 0; k < hunks.size(); ++k) {
        out += std::format("{}({},{},{},{})", k == 0 ? "" : ",", hunks[k][0], hunks[k][1], hunks[k][2], hunks[k][3]);
    }
    out += ']';
    return out;
}

std::string digitLines(std::string_view digits) {
    std::string out;
    for (const char d : digits) {
        out += 'L';
        out += d;
        out += '\n';
    }
    return out;
}

Result<std::string> decodeSequenceField(std::string_view field) {
    if (field == "-") {
        return std::string();
    }
    if (field.empty()) {
        return fail(ErrorCode::Corrupt, "empty sequence field (use - for the empty sequence)");
    }
    if (field.front() != '"') {
        if (field.find_first_not_of("0123456789") != std::string_view::npos) {
            return fail(ErrorCode::Corrupt, std::format("bad digit string '{}'", field));
        }
        return digitLines(field);
    }
    if (field.size() < 2 || field.back() != '"') {
        return fail(ErrorCode::Corrupt, std::format("unterminated string '{}'", field));
    }
    std::string out;
    const std::string_view body = field.substr(1, field.size() - 2);
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\') {
            out += body[i];
            continue;
        }
        if (++i >= body.size()) {
            return fail(ErrorCode::Corrupt, "a trailing backslash");
        }
        switch (body[i]) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case '\\': out += '\\'; break;
        case '"': out += '"'; break;
        case 'x': {
            const int high = i + 1 < body.size() ? hexValue(body[i + 1]) : -1;
            const int low = i + 2 < body.size() ? hexValue(body[i + 2]) : -1;
            if (high < 0 || low < 0) {
                return fail(ErrorCode::Corrupt, "bad \\x escape");
            }
            out += static_cast<char>(high * 16 + low);
            i += 2;
            break;
        }
        default: return fail(ErrorCode::Corrupt, std::format("unknown escape \\{}", body[i]));
        }
    }
    return out;
}

Result<HistogramGolden> parseHistogramGolden(std::string name, std::string_view text) {
    const std::vector<std::string_view> lines = splitLines(text);
    if (lines.size() < 2) {
        return fail(ErrorCode::Corrupt, "a golden needs A and B lines");
    }
    HistogramGolden golden;
    golden.name = std::move(name);
    auto a = decodeSequenceField(lines[0]);
    auto b = decodeSequenceField(lines[1]);
    if (!a || !b) {
        return std::unexpected(std::move(!a ? a.error() : b.error()));
    }
    golden.a = std::move(*a);
    golden.b = std::move(*b);
    for (std::size_t k = 2; k < lines.size(); ++k) {
        if (skippable(lines[k])) {
            continue;
        }
        auto hunk = parseHunkLine(lines[k]);
        if (!hunk) {
            return std::unexpected(std::move(hunk.error()));
        }
        golden.hunks.push_back(*hunk);
    }
    return golden;
}

Result<DialectGolden> parseDialectGolden(std::string name, std::string_view text) {
    const std::vector<std::string_view> lines = splitLines(text);
    if (lines.size() < 2) {
        return fail(ErrorCode::Corrupt, "a golden needs A and B lines");
    }
    DialectGolden golden;
    golden.name = std::move(name);
    auto a = decodeSequenceField(lines[0]);
    auto b = decodeSequenceField(lines[1]);
    if (!a || !b) {
        return std::unexpected(std::move(!a ? a.error() : b.error()));
    }
    golden.a = std::move(*a);
    golden.b = std::move(*b);
    HunkList* target = nullptr;
    bool sawClassic = false;
    bool sawNew = false;
    for (std::size_t k = 2; k < lines.size(); ++k) {
        if (skippable(lines[k])) {
            continue;
        }
        if (lines[k] == "classic:") {
            target = &golden.classic;
            sawClassic = true;
        } else if (lines[k] == "new:") {
            target = &golden.fresh;
            sawNew = true;
        } else if (target == nullptr) {
            return fail(ErrorCode::Corrupt, "a hunk line before \"classic:\"");
        } else {
            auto hunk = parseHunkLine(lines[k]);
            if (!hunk) {
                return std::unexpected(std::move(hunk.error()));
            }
            target->push_back(*hunk);
        }
    }
    if (!sawClassic || !sawNew) {
        return fail(ErrorCode::Corrupt, "a dialect golden needs \"classic:\" and \"new:\" lists");
    }
    return golden;
}

Result<std::vector<HistogramGolden>> loadHistogramGoldens(std::string_view directory) {
    return loadAll<HistogramGolden>(
        directory, [](std::string name, std::string_view text) { return parseHistogramGolden(std::move(name), text); });
}

Result<std::vector<DialectGolden>> loadDialectGoldens(std::string_view directory) {
    return loadAll<DialectGolden>(
        directory, [](std::string name, std::string_view text) { return parseDialectGolden(std::move(name), text); });
}

std::string testDataPath(std::string_view relative) { return joinUtf8(FROST_DIFF_TEST_DATA, relative); }

bool updateGoldens() { return frost::test::environment("FROST_DIFF_UPDATE_GOLDENS") == "1"; }

} // namespace frost::diff::test
