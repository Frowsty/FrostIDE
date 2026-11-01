// SPDX-License-Identifier: Apache-2.0
//
// Private string helpers of frost::project (text_util.hpp).
#include "text_util.hpp"

#include "frost/io/utf.hpp"

#include <algorithm>
#include <cstddef>
#include <format>

namespace frost::project::detail {

std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = toLowerAscii(c);
    }
    return out;
}

std::string toUpperAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = toUpperAscii(c);
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (toLowerAscii(a[i]) != toLowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

bool istartsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

bool iendsWith(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && iequals(text.substr(text.size() - suffix.size()), suffix);
}

int icompare(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char x = toLowerAscii(a[i]);
        const char y = toLowerAscii(b[i]);
        if (x != y) {
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
        }
    }
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    const int bytes = a.compare(b);
    return bytes < 0 ? -1 : (bytes > 0 ? 1 : 0);
}

std::string_view trim(std::string_view text) noexcept {
    constexpr std::string_view kSpace = " \t\r\n";
    const std::size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kSpace);
    return text.substr(first, last - first + 1);
}

std::vector<std::string_view> splitTrimmed(std::string_view text, char separator) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(separator, start), text.size());
        const std::string_view piece = trim(text.substr(start, end - start));
        if (!piece.empty()) {
            out.push_back(piece);
        }
        start = end + 1;
    }
    return out;
}

std::string normalizeGuid(std::string_view guid) {
    std::string_view g = trim(guid);
    if (g.size() == 38 && g.front() == '{' && g.back() == '}') {
        g = g.substr(1, 36);
    }
    if (g.size() == 36 && g[8] == '-' && g[13] == '-' && g[18] == '-' && g[23] == '-') {
        return '{' + toUpperAscii(g) + '}';
    }
    return toUpperAscii(trim(guid));
}

std::string lowerExtension(std::string_view fileName) {
    const std::size_t dot = fileName.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return toLowerAscii(fileName.substr(dot));
}

std::string_view stem(std::string_view fileName) noexcept {
    const std::size_t dot = fileName.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return fileName;
    }
    return fileName.substr(0, dot);
}

Result<std::string> decodeText(std::string_view bytes, std::string_view path) {
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        return std::string(bytes.substr(3));
    }
    const bool le = bytes.starts_with("\xFF\xFE");
    const bool be = bytes.starts_with("\xFE\xFF");
    if (!le && !be) {
        return std::string(bytes);
    }
    const std::string_view body = bytes.substr(2);
    if (body.size() % 2 != 0) {
        return fail(ErrorCode::Corrupt, std::format("{}: UTF-16 text with an odd number of bytes", path));
    }
    std::u16string units(body.size() / 2, u'\0');
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto b0 = static_cast<unsigned char>(body[2 * i]);
        const auto b1 = static_cast<unsigned char>(body[(2 * i) + 1]);
        units[i] = le ? static_cast<char16_t>(b0 | (b1 << 8)) : static_cast<char16_t>((b0 << 8) | b1);
    }
    auto utf8 = io::utf16ToUtf8(units);
    if (!utf8) {
        return fail(ErrorCode::Corrupt, std::format("{}: {}", path, utf8.error().message()));
    }
    return std::move(*utf8);
}

} // namespace frost::project::detail
