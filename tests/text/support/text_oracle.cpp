// SPDX-License-Identifier: Apache-2.0
//
// TextOracle (text_oracle.hpp): the brute-force std::string model behind the piece-tree
// differential, stress and fuzz tests. Every rule is reimplemented from its definition.

#include "text/support/text_oracle.hpp"

#include <algorithm>
#include <utility>

namespace frost::text::test {

std::size_t oracleDecode(std::string_view text, std::size_t i, char32_t& scalar) noexcept {
    const auto byte = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[k])); };
    const std::uint32_t b0 = byte(i);
    std::size_t length = 0;
    std::uint32_t value = 0;
    if ((b0 >> 7) == 0) {
        scalar = b0;
        return 1;
    }
    if ((b0 >> 5) == 0b110) {
        length = 2;
        value = b0 & 0x1F;
    } else if ((b0 >> 4) == 0b1110) {
        length = 3;
        value = b0 & 0x0F;
    } else if ((b0 >> 3) == 0b11110) {
        length = 4;
        value = b0 & 0x07;
    } else {
        return 0; // A stray continuation byte (10xxxxxx) or F8..FF.
    }
    if (text.size() - i < length) {
        return 0; // Truncated.
    }
    for (std::size_t k = 1; k < length; ++k) {
        const std::uint32_t b = byte(i + k);
        if ((b >> 6) != 0b10) {
            return 0;
        }
        value = (value << 6) | (b & 0x3F);
    }
    if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return 0;
    }
    const std::size_t shortest = value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
    if (shortest != length) {
        return 0; // Non-shortest form.
    }
    scalar = static_cast<char32_t>(value);
    return length;
}

std::size_t oracleValidPrefix(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        char32_t scalar = 0;
        const std::size_t length = oracleDecode(text, i, scalar);
        if (length == 0) {
            return i;
        }
        i += length;
    }
    return text.size();
}

TextSummary oracleSummary(std::string_view text) {
    TextSummary s;
    s.bytes = text.size();
    s.startsWithLf = !text.empty() && text.front() == '\n';
    s.endsWithCr = !text.empty() && text.back() == '\r';
    std::uint32_t line = 0;
    std::uint64_t lineChars = 0;
    std::uint64_t lineBytes = 0;
    std::uint64_t lineUnits = 0;
    const auto endLine = [&] {
        const auto chars = static_cast<std::uint32_t>(lineChars);
        if (line == 0) {
            s.firstLineChars = chars;
        }
        if (line == 0 || chars > s.longestLineChars) {
            s.longestLineChars = chars;
            s.longestLine = line;
        }
        ++line;
        lineChars = lineBytes = lineUnits = 0;
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '\r' || text[i] == '\n') {
            const bool crlf = text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n';
            s.lineFeeds += crlf || text[i] == '\n' ? 1 : 0;
            s.chars += crlf ? 2 : 1;
            s.utf16 += crlf ? 2 : 1;
            i += crlf ? 2 : 1;
            endLine();
            continue;
        }
        char32_t scalar = 0;
        const std::size_t length = std::max<std::size_t>(oracleDecode(text, i, scalar), 1);
        const std::uint64_t units = scalar >= 0x10000 ? 2 : 1;
        s.chars += 1;
        s.utf16 += units;
        lineChars += 1;
        lineBytes += length;
        lineUnits += units;
        i += length;
    }
    s.lineBreaks = line;
    s.lastLineChars = static_cast<std::uint32_t>(lineChars);
    s.lastLineBytes = static_cast<std::uint32_t>(lineBytes);
    s.lastLineUtf16 = static_cast<std::uint32_t>(lineUnits);
    endLine();
    return s;
}

Point linearPointAt(std::string_view text, std::uint64_t x) {
    const std::uint64_t y = crlfInterior(text, x) ? x - 1 : x;
    std::uint32_t line = 0;
    std::uint64_t start = 0;
    for (std::uint64_t i = 0; i < y;) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            i += 2; // y is not CRLF-interior, so the whole CRLF lies before it.
        } else if (text[i] == '\r' || text[i] == '\n') {
            i += 1;
        } else {
            ++i;
            continue;
        }
        ++line;
        start = i;
    }
    return {LineIndex{line}, ByteColumn{static_cast<std::uint32_t>(y - start)}};
}

std::uint64_t linearLspOffset(std::string_view text, Point point) {
    std::uint64_t start = 0;
    for (std::uint32_t line = 0; line < point.line.value(); ++line) {
        while (text[start] != '\r' && text[start] != '\n') {
            ++start;
        }
        start += text[start] == '\r' && start + 1 < text.size() && text[start + 1] == '\n' ? 2 : 1;
    }
    std::uint64_t end = start;
    while (end < text.size() && text[end] != '\r' && text[end] != '\n') {
        ++end;
    }
    return start + std::min<std::uint64_t>(point.column.value(), end - start);
}

bool crlfInterior(std::string_view text, std::uint64_t x) noexcept {
    return x > 0 && x < text.size() && text[x - 1] == '\r' && text[x] == '\n';
}

ByteChange expectedChange(std::string_view before, std::uint64_t a, std::uint64_t b, std::string_view inserted) {
    const std::uint64_t oldSize = before.size();
    const std::uint64_t length = inserted.size();
    const std::uint64_t newSize = oldSize - (b - a) + length;
    const auto oldByte = [&](std::uint64_t i) -> int {
        return i < oldSize ? static_cast<unsigned char>(before[i]) : -1;
    };
    const auto newByte = [&](std::uint64_t i) -> int {
        if (i >= newSize) {
            return -1;
        }
        if (i < a) {
            return static_cast<unsigned char>(before[i]);
        }
        if (i < a + length) {
            return static_cast<unsigned char>(inserted[i - a]);
        }
        return static_cast<unsigned char>(before[i - a - length + b]);
    };
    ByteChange change{a, b, a + length};
    if (a > 0 && oldByte(a - 1) == '\r' && (oldByte(a) == '\n' || newByte(a) == '\n')) {
        change.start = a - 1;
    }
    const std::uint64_t eo = b;
    const std::uint64_t en = a + length;
    if (oldByte(eo) == '\n' && ((eo > 0 && oldByte(eo - 1) == '\r') || (en > 0 && newByte(en - 1) == '\r'))) {
        change.oldEnd = eo + 1;
        change.newEnd = en + 1;
    }
    return change;
}

TextOracle::TextOracle(std::string text) : m_text(std::move(text)) {}

void TextOracle::setText(std::string text) { m_text = std::move(text); }

void TextOracle::replace(std::uint64_t a, std::uint64_t b, std::string_view inserted) {
    m_text.replace(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a), inserted);
}

bool TextOracle::isCharBoundary(std::uint64_t x) const noexcept {
    if (x > m_text.size()) {
        return false;
    }
    if (x == m_text.size()) {
        return true;
    }
    // Inside a sequence exactly when one of the 3 bytes before starts a sequence reaching past x.
    for (std::uint64_t back = 1; back <= 3 && back <= x; ++back) {
        char32_t scalar = 0;
        if (oracleDecode(m_text, static_cast<std::size_t>(x - back), scalar) > back) {
            return false;
        }
    }
    return true;
}

std::uint64_t TextOracle::floorChar(std::uint64_t x) const noexcept {
    while (!isCharBoundary(x)) {
        --x;
    }
    return x;
}

std::optional<ErrorCode> TextOracle::checkEdit(std::uint64_t a, std::uint64_t b,
                                               std::string_view inserted) const noexcept {
    if (a > b) {
        return ErrorCode::InvalidArgument;
    }
    if (b > m_text.size()) {
        return ErrorCode::OutOfRange;
    }
    if (!isCharBoundary(a) || !isCharBoundary(b)) {
        return ErrorCode::InvalidArgument;
    }
    if (!oracleValid(inserted)) {
        return ErrorCode::InvalidArgument;
    }
    return std::nullopt;
}

void TextOracle::rebuild() {
    const std::size_t n = m_text.size();
    m_lineStarts.assign(1, 0);
    m_contentEnds.clear();
    m_breaks.clear();
    m_utf16Before.assign(n + 1, 0);
    m_charsBefore.assign(n + 1, 0);
    std::uint64_t units = 0;
    std::uint64_t chars = 0;
    // Raw pointers: this loop runs over every byte of every full check, also in Debug builds,
    // where hardened operator[] would dominate the differential's run time.
    std::uint64_t* const utf16Before = m_utf16Before.data();
    std::uint64_t* const charsBefore = m_charsBefore.data();
    for (std::size_t i = 0; i < n;) {
        char32_t scalar = 0;
        std::size_t length = oracleDecode(m_text, i, scalar);
        if (length == 0) {
            length = 1; // Ill-formed text never reaches the oracle; keep the loop finite anyway.
        }
        for (std::size_t k = 0; k < length && i + k < n; ++k) {
            utf16Before[i + k] = units;
            charsBefore[i + k] = chars;
        }
        units += scalar >= 0x10000 ? 2 : 1;
        chars += 1;
        i += length;
    }
    utf16Before[n] = units;
    charsBefore[n] = chars;
    for (std::size_t i = 0; i < n;) {
        const char c = m_text[i];
        if (c == '\r') {
            m_contentEnds.push_back(i);
            if (i + 1 < n && m_text[i + 1] == '\n') {
                m_breaks.push_back(LineBreak::CrLf);
                i += 2;
            } else {
                m_breaks.push_back(LineBreak::Cr);
                i += 1;
            }
            m_lineStarts.push_back(i);
        } else if (c == '\n') {
            m_contentEnds.push_back(i);
            m_breaks.push_back(LineBreak::Lf);
            i += 1;
            m_lineStarts.push_back(i);
        } else {
            ++i;
        }
    }
    m_contentEnds.push_back(n);
    m_breaks.push_back(LineBreak::None);
}

TextSummary TextOracle::summary() const {
    TextSummary s;
    const std::size_t n = m_text.size();
    s.bytes = n;
    s.utf16 = m_utf16Before[n];
    s.chars = m_charsBefore[n];
    s.lineBreaks = lineCount() - 1;
    s.lineFeeds = static_cast<std::uint32_t>(std::count(m_text.begin(), m_text.end(), '\n'));
    s.startsWithLf = n > 0 && m_text.front() == '\n';
    s.endsWithCr = n > 0 && m_text.back() == '\r';
    for (std::uint32_t line = 0; line < lineCount(); ++line) {
        const auto chars =
            static_cast<std::uint32_t>(m_charsBefore[m_contentEnds[line]] - m_charsBefore[m_lineStarts[line]]);
        if (line == 0) {
            s.firstLineChars = chars;
        }
        if (line == 0 || chars > s.longestLineChars) {
            s.longestLineChars = chars;
            s.longestLine = line;
        }
    }
    const std::uint32_t last = lineCount() - 1;
    s.lastLineChars =
        static_cast<std::uint32_t>(m_charsBefore[m_contentEnds[last]] - m_charsBefore[m_lineStarts[last]]);
    s.lastLineBytes = static_cast<std::uint32_t>(m_contentEnds[last] - m_lineStarts[last]);
    s.lastLineUtf16 =
        static_cast<std::uint32_t>(m_utf16Before[m_contentEnds[last]] - m_utf16Before[m_lineStarts[last]]);
    return s;
}

std::uint32_t TextOracle::lineOf(std::uint64_t y) const {
    const auto it = std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), y);
    return static_cast<std::uint32_t>(it - m_lineStarts.begin() - 1);
}

std::uint64_t TextOracle::crlfFloor(std::uint64_t y) const noexcept { return crlfInterior(m_text, y) ? y - 1 : y; }

Point TextOracle::pointAt(std::uint64_t x) const {
    const std::uint64_t y = crlfFloor(x);
    const std::uint32_t line = lineOf(y);
    return {LineIndex{line}, ByteColumn{static_cast<std::uint32_t>(y - m_lineStarts[line])}};
}

Utf16Point TextOracle::utf16PointAt(std::uint64_t x) const {
    const std::uint64_t y = crlfFloor(floorChar(x));
    const std::uint32_t line = lineOf(y);
    return {LineIndex{line},
            Utf16Column{static_cast<std::uint32_t>(m_utf16Before[y] - m_utf16Before[m_lineStarts[line]])}};
}

std::uint64_t TextOracle::utf16At(std::uint64_t x) const { return m_utf16Before[floorChar(x)]; }

std::uint64_t TextOracle::clip(std::uint64_t x) const { return floorChar(std::min<std::uint64_t>(x, m_text.size())); }

std::optional<std::uint64_t> TextOracle::offsetAt(Point point) const {
    const std::uint32_t line = point.line.value();
    if (line >= lineCount()) {
        return std::nullopt;
    }
    const std::uint64_t length = m_contentEnds[line] - m_lineStarts[line];
    return floorChar(m_lineStarts[line] + std::min<std::uint64_t>(point.column.value(), length));
}

std::optional<std::uint64_t> TextOracle::offsetAt(Utf16Point point) const {
    const std::uint32_t line = point.line.value();
    if (line >= lineCount()) {
        return std::nullopt;
    }
    const std::uint64_t first = m_utf16Before[m_lineStarts[line]];
    const std::uint64_t last = m_utf16Before[m_contentEnds[line]];
    return offsetAtUtf16(std::min(first + point.column.value(), last));
}

std::optional<std::uint64_t> TextOracle::offsetAtUtf16(std::uint64_t units) const {
    if (units > utf16Size()) {
        return std::nullopt;
    }
    // The last position whose count is <= units, floored to its character's start.
    const auto it = std::upper_bound(m_utf16Before.begin(), m_utf16Before.end(), units);
    return floorChar(static_cast<std::uint64_t>(it - m_utf16Before.begin() - 1));
}

std::optional<LineInfo> TextOracle::lineInfo(std::uint32_t line) const {
    if (line >= lineCount()) {
        return std::nullopt;
    }
    return LineInfo{LineIndex{line}, ByteRange{ByteOffset{m_lineStarts[line]}, ByteOffset{m_contentEnds[line]}},
                    m_breaks[line]};
}

std::uint64_t TextOracle::lspOffset(Point point) const {
    const std::uint32_t line = point.line.value();
    const std::uint64_t length = m_contentEnds[line] - m_lineStarts[line];
    return m_lineStarts[line] + std::min<std::uint64_t>(point.column.value(), length);
}

} // namespace frost::text::test
