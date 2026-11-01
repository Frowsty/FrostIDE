// SPDX-License-Identifier: Apache-2.0
//
// TextSummary's monoid (combine) and its O(n) scanner (summarize); see frost/text/text_summary.hpp
// for the rules. The scanner counts per byte, with a SWAR fast path over 8-byte words that hold
// no CR or LF.

#include "frost/text/text_summary.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace frost::text {
namespace {

// The better of two (length, row) candidates: more characters wins, ties go to the smaller row.
void takeBest(std::uint32_t& bestChars, std::uint32_t& bestRow, std::uint32_t chars, std::uint32_t row) noexcept {
    if (chars > bestChars || (chars == bestChars && row < bestRow)) {
        bestChars = chars;
        bestRow = row;
    }
}

constexpr std::uint64_t kOnes = 0x0101'0101'0101'0101ULL;
constexpr std::uint64_t kHighs = 0x8080'8080'8080'8080ULL;

// True when some byte of `word` equals `value` (the exact "has zero byte" test applied to
// word ^ value-in-every-byte; Bit Twiddling Hacks, S. E. Anderson, public domain).
constexpr bool hasByte(std::uint64_t word, std::uint8_t value) noexcept {
    const std::uint64_t x = word ^ (kOnes * value);
    return ((x - kOnes) & ~x & kHighs) != 0;
}

// Per-line counters while scanning.
struct LineCounters {
    std::uint64_t chars = 0;
    std::uint64_t bytes = 0;
    std::uint64_t utf16 = 0;
};

} // namespace

TextSummary combine(const TextSummary& a, const TextSummary& b) noexcept {
    if (a.bytes == 0) {
        return b;
    }
    if (b.bytes == 0) {
        return a;
    }
    TextSummary out;
    out.bytes = a.bytes + b.bytes;
    out.utf16 = a.utf16 + b.utf16;
    out.chars = a.chars + b.chars;
    out.lineFeeds = a.lineFeeds + b.lineFeeds;
    const bool crlf = a.endsWithCr && b.startsWithLf;
    out.lineBreaks = a.lineBreaks + b.lineBreaks - (crlf ? 1u : 0u);
    out.startsWithLf = a.startsWithLf;
    out.endsWithCr = b.endsWithCr;
    out.longestLineChars = a.longestLineChars;
    out.longestLine = a.longestLine;
    if (crlf) {
        // a.lineBreaks >= 1 (a ends with CR) and b.lineBreaks >= 1 (b starts with LF), so a's first
        // row and b's last row are complete.
        out.firstLineChars = a.firstLineChars;
        out.lastLineChars = b.lastLineChars;
        out.lastLineBytes = b.lastLineBytes;
        out.lastLineUtf16 = b.lastLineUtf16;
        if (b.longestLineChars > 0) {
            takeBest(out.longestLineChars, out.longestLine, b.longestLineChars, a.lineBreaks + b.longestLine - 1);
        }
        return out;
    }
    const std::uint32_t joined = a.lastLineChars + b.firstLineChars;
    out.firstLineChars = a.lineBreaks == 0 ? joined : a.firstLineChars;
    if (b.lineBreaks == 0) {
        out.lastLineChars = a.lastLineChars + static_cast<std::uint32_t>(b.chars);
        out.lastLineBytes = a.lastLineBytes + static_cast<std::uint32_t>(b.bytes);
        out.lastLineUtf16 = a.lastLineUtf16 + static_cast<std::uint32_t>(b.utf16);
    } else {
        out.lastLineChars = b.lastLineChars;
        out.lastLineBytes = b.lastLineBytes;
        out.lastLineUtf16 = b.lastLineUtf16;
    }
    takeBest(out.longestLineChars, out.longestLine, joined, a.lineBreaks);
    takeBest(out.longestLineChars, out.longestLine, b.longestLineChars, a.lineBreaks + b.longestLine);
    return out;
}

TextSummary summarize(std::string_view utf8) noexcept {
    TextSummary out;
    const std::size_t n = utf8.size();
    if (n == 0) {
        return out;
    }
    const char* data = utf8.data();
    out.bytes = n;
    out.startsWithLf = data[0] == '\n';
    out.endsWithCr = data[n - 1] == '\r';

    // Line counters are 64-bit while scanning; stored values fit in 32 bits for inputs within the
    // buffer limit of 0xFFFF'FFFE bytes (ADR 0012, D6).
    LineCounters line;
    std::uint64_t chars = 0;
    std::uint64_t utf16 = 0;
    std::uint32_t row = 0;
    bool previousCr = false;
    const auto endLine = [&]() noexcept {
        const auto lineChars = static_cast<std::uint32_t>(line.chars);
        if (row == 0) {
            out.firstLineChars = lineChars;
            out.longestLineChars = lineChars;
            out.longestLine = 0;
        } else if (lineChars > out.longestLineChars) {
            out.longestLineChars = lineChars;
            out.longestLine = row;
        }
        ++row;
        line = {};
    };

    std::size_t i = 0;
    while (i < n) {
        if (n - i >= 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, data + i, sizeof word);
            if (!hasByte(word, '\n') && !hasByte(word, '\r')) {
                // Continuation bytes are 10xxxxxx: bit 7 set and bit 6 clear. Bytes >= 0xF0 have
                // bits 7..4 set. Shifting left by k moves bit 7-k of each byte to its bit 7.
                const std::uint64_t continuation = word & ~(word << 1) & kHighs;
                const std::uint64_t fourByteLeads = word & (word << 1) & (word << 2) & (word << 3) & kHighs;
                const auto lead = static_cast<std::uint64_t>(8 - std::popcount(continuation));
                const auto pairs = static_cast<std::uint64_t>(std::popcount(fourByteLeads));
                line.bytes += 8;
                line.chars += lead;
                line.utf16 += lead + pairs;
                chars += lead;
                utf16 += lead + pairs;
                previousCr = false;
                i += 8;
                continue;
            }
        }
        const std::size_t stop = n - i >= 8 ? i + 8 : n;
        for (; i < stop; ++i) {
            const auto byte = static_cast<std::uint8_t>(data[i]);
            if (byte == '\n') {
                ++out.lineFeeds;
                ++chars;
                ++utf16;
                if (previousCr) {
                    previousCr = false; // The CR already ended the line; this LF completes its CRLF.
                    continue;
                }
                endLine();
            } else if (byte == '\r') {
                ++chars;
                ++utf16;
                endLine();
                previousCr = true;
            } else {
                previousCr = false;
                const std::uint64_t lead = (byte & 0xC0u) != 0x80u ? 1u : 0u;
                const std::uint64_t pair = byte >= 0xF0u ? 1u : 0u;
                line.bytes += 1;
                line.chars += lead;
                line.utf16 += lead + pair;
                chars += lead;
                utf16 += lead + pair;
            }
        }
    }

    out.chars = chars;
    out.utf16 = utf16;
    out.lineBreaks = row;
    const auto lastChars = static_cast<std::uint32_t>(line.chars);
    out.lastLineChars = lastChars;
    out.lastLineBytes = static_cast<std::uint32_t>(line.bytes);
    out.lastLineUtf16 = static_cast<std::uint32_t>(line.utf16);
    if (row == 0) {
        out.firstLineChars = lastChars;
        out.longestLineChars = lastChars;
        out.longestLine = 0;
    } else if (lastChars > out.longestLineChars) {
        out.longestLineChars = lastChars;
        out.longestLine = row;
    }
    return out;
}

} // namespace frost::text
