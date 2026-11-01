// SPDX-License-Identifier: Apache-2.0
//
// Lossy UTF-8, UTF-16 and UTF-32 decoders and the exact UTF-16/UTF-32 encoders
// (frost/text/unicode_codec.hpp; ADR 0018, D5 and D16).
/*
 * The UTF-8 decoder implements the Unicode Standard's practice "U+FFFD Substitution of Maximal
 * Subparts" (Unicode 16.0, chapter 3, section 3.9, with tables 3-8 to 3-11 as its examples), using
 * the well-formed byte ranges of table 3-7. The implementation is our own: a maximal subpart is the
 * longest prefix of a well-formed sequence that starts at the ill-formed offset, or one byte when
 * the byte there cannot start one. Valid runs are found with io::validUtf8Prefix (frost::io's
 * table 3-7 validator) and appended in one append.
 */

#include "frost/text/unicode_codec.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/utf.hpp"

#include "unicode_internal.hpp"

#include <cstddef>
#include <cstdint>

namespace frost::text {
namespace {

using detail::appendScalarUtf8;
using detail::octetAt;

// Records one replacement at the call-local input offset `inputAt`, appending U+FFFD. `outStart`
// is out.size() when the call began; the stats offsets are absolute in the stream (DecodeStats).
void replace(std::string& out, DecodeStats& stats, std::size_t outStart, std::size_t inputAt) noexcept {
    if (!stats.firstInvalidInput) {
        stats.firstInvalidInput = stats.inputBytes + inputAt;
        stats.firstReplacementOutput = stats.outputBytes + (out.size() - outStart);
    }
    ++stats.replacements;
    out.append(kReplacementUtf8);
}

// Adds a finished call's consumed and appended counts to the stream totals.
std::size_t finish(std::string& out, DecodeStats& stats, std::size_t outStart, std::size_t consumed) noexcept {
    stats.inputBytes += consumed;
    stats.outputBytes += out.size() - outStart;
    return consumed;
}

/*
 * The maximal subpart at `i`, where io::validUtf8Prefix reported an ill-formed sequence: 1..3
 * bytes. `atEnd` is set when the subpart is a proper prefix of a well-formed sequence that runs
 * into the end of `b` (it could still become valid if more input followed).
 */
std::size_t maximalSubpart(std::string_view b, std::size_t i, bool& atEnd) noexcept {
    atEnd = false;
    const std::uint8_t b0 = octetAt(b, i);
    // Table 3-7: the length and the second-byte range follow from the lead byte.
    std::size_t length = 0;
    std::uint8_t lo = 0x80;
    std::uint8_t hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        length = 2;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        length = 3;
        lo = b0 == 0xE0 ? 0xA0 : 0x80;
        hi = b0 == 0xED ? 0x9F : 0xBF;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        length = 4;
        lo = b0 == 0xF0 ? 0x90 : 0x80;
        hi = b0 == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 1; // 80..C1 and F5..FF never start a sequence.
    }
    std::size_t k = 1;
    if (i + 1 < b.size()) {
        const std::uint8_t b1 = octetAt(b, i + 1);
        if (b1 >= lo && b1 <= hi) {
            k = 2;
            while (k < length && i + k < b.size() && detail::isContinuation(octetAt(b, i + k))) {
                ++k;
            }
        }
    }
    // validUtf8Prefix stopped at i, so the sequence is not complete there.
    FROST_ASSERT(k < length);
    atEnd = i + k == b.size();
    return k;
}

// Reads a 16-bit unit at byte offset i, assembling it byte by byte (no alignment assumptions).
std::uint32_t unit16(std::string_view b, std::size_t i, Endian order) noexcept {
    const std::uint32_t first = octetAt(b, i);
    const std::uint32_t second = octetAt(b, i + 1);
    return order == Endian::Little ? (first | (second << 8)) : ((first << 8) | second);
}

std::uint32_t unit32(std::string_view b, std::size_t i, Endian order) noexcept {
    std::uint32_t value = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        const std::uint32_t byte = octetAt(b, i + (order == Endian::Little ? 3 - k : k));
        value = (value << 8) | byte;
    }
    return value;
}

void putUnit16(std::uint32_t u, Endian order, std::string& out) noexcept {
    const auto lo = static_cast<char>(static_cast<std::uint8_t>(u & 0xFF));
    const auto hi = static_cast<char>(static_cast<std::uint8_t>(u >> 8));
    if (order == Endian::Little) {
        out.push_back(lo);
        out.push_back(hi);
    } else {
        out.push_back(hi);
        out.push_back(lo);
    }
}

void putUnit32(std::uint32_t u, Endian order, std::string& out) noexcept {
    for (std::size_t k = 0; k < 4; ++k) {
        const std::size_t shift = order == Endian::Little ? 8 * k : 8 * (3 - k);
        out.push_back(static_cast<char>(static_cast<std::uint8_t>((u >> shift) & 0xFF)));
    }
}

} // namespace

std::size_t appendUtf8Lossy(std::string_view bytes, std::string& out, DecodeStats& stats, InputEnd end) noexcept {
    const std::size_t outStart = out.size();
    std::size_t i = 0;
    while (i < bytes.size()) {
        // Budget: B15. The valid run is found by io's word-at-a-time validator and appended at once.
        const std::size_t valid = io::validUtf8Prefix(bytes.substr(i));
        out.append(bytes.substr(i, valid));
        i += valid;
        if (i == bytes.size()) {
            break;
        }
        bool atEnd = false;
        const std::size_t k = maximalSubpart(bytes, i, atEnd);
        if (atEnd && end == InputEnd::More) {
            break; // A proper prefix that the next call may complete; leave it unconsumed.
        }
        replace(out, stats, outStart, i);
        i += k;
    }
    return finish(out, stats, outStart, i);
}

std::size_t appendUtf16Lossy(std::string_view bytes, Endian order, std::string& out, DecodeStats& stats,
                             InputEnd end) noexcept {
    const std::size_t outStart = out.size();
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i + 2 <= n) {
        const std::uint32_t u = unit16(bytes, i, order);
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
            i += 2;
        } else if (u < 0xD800 || u > 0xDFFF) {
            appendScalarUtf8(static_cast<char32_t>(u), out);
            i += 2;
        } else if (u <= 0xDBFF) {
            if (i + 4 <= n) {
                const std::uint32_t v = unit16(bytes, i + 2, order);
                if (v >= 0xDC00 && v <= 0xDFFF) {
                    appendScalarUtf8(static_cast<char32_t>(0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00)), out);
                    i += 4;
                } else {
                    replace(out, stats, outStart, i); // A high surrogate not followed by a low one.
                    i += 2;
                }
            } else if (end == InputEnd::More) {
                return finish(out, stats, outStart, i); // The next unit may be its low surrogate.
            } else {
                replace(out, stats, outStart, i); // A trailing high surrogate.
                i += 2;
            }
        } else {
            replace(out, stats, outStart, i); // A lone low surrogate.
            i += 2;
        }
    }
    if (i < n && end == InputEnd::Final) {
        replace(out, stats, outStart, i); // An odd trailing byte.
        i = n;
    }
    return finish(out, stats, outStart, i);
}

std::size_t appendUtf32Lossy(std::string_view bytes, Endian order, std::string& out, DecodeStats& stats,
                             InputEnd end) noexcept {
    const std::size_t outStart = out.size();
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const std::uint32_t u = unit32(bytes, i, order);
        if (u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) {
            replace(out, stats, outStart, i);
        } else if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else {
            appendScalarUtf8(static_cast<char32_t>(u), out);
        }
    }
    if (i < n && end == InputEnd::Final) {
        replace(out, stats, outStart, i); // 1-3 trailing bytes.
        i = n;
    }
    return finish(out, stats, outStart, i);
}

void appendUtf16(std::string_view utf8, Endian order, std::string& out) noexcept {
    for (std::size_t i = 0; i < utf8.size();) {
        std::size_t length = 0;
        const auto c = static_cast<std::uint32_t>(detail::decodeValidUtf8Scalar(utf8, i, length));
        i += length;
        if (c >= 0x10000) {
            const std::uint32_t v = c - 0x10000;
            putUnit16(0xD800 + (v >> 10), order, out);
            putUnit16(0xDC00 + (v & 0x3FF), order, out);
        } else {
            putUnit16(c, order, out);
        }
    }
}

void appendUtf32(std::string_view utf8, Endian order, std::string& out) noexcept {
    for (std::size_t i = 0; i < utf8.size();) {
        std::size_t length = 0;
        const auto c = static_cast<std::uint32_t>(detail::decodeValidUtf8Scalar(utf8, i, length));
        i += length;
        putUnit32(c, order, out);
    }
}

} // namespace frost::text
