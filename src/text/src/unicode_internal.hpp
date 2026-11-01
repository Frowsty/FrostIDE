// SPDX-License-Identifier: Apache-2.0
//
// Private Unicode helpers of frost::text's codecs (ADR 0018), shared by unicode_codec.cpp,
// legacy_codec.cpp, text_file.cpp, encoding_detection.cpp and codec_win.cpp: scalar-to-UTF-8
// encoding, decoding one scalar of well-formed UTF-8, UTF-16 units to UTF-8, char-boundary
// helpers, table 3-7's lead-byte shapes and the proper-prefix test of the streaming decoders.
// Not public API. Header-only, pure
// and thread-safe; the bit layouts are the Unicode Standard's (chapter 3, table 3-6 and 3-7).
#pragma once

#include "frost/core/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace frost::text::detail {

// The byte at `i` as an unsigned value.
[[nodiscard]] inline std::uint8_t octetAt(std::string_view s, std::size_t i) noexcept {
    return static_cast<std::uint8_t>(s[i]);
}

// True for UTF-8 continuation bytes (10xxxxxx).
[[nodiscard]] constexpr bool isContinuation(std::uint8_t b) noexcept { return (b & 0xC0u) == 0x80u; }

// Appends the UTF-8 of `scalar` (any value up to 0x10FFFF; surrogates are encoded as the 3-byte
// pattern, which callers that validate their output reject). O(1).
inline void appendScalarUtf8(char32_t scalar, std::string& out) noexcept {
    const auto put = [&out](std::uint32_t b) { out.push_back(static_cast<char>(static_cast<std::uint8_t>(b))); };
    const auto c = static_cast<std::uint32_t>(scalar);
    if (c < 0x80) {
        put(c);
    } else if (c < 0x800) {
        put(0xC0 | (c >> 6));
        put(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        put(0xE0 | (c >> 12));
        put(0x80 | ((c >> 6) & 0x3F));
        put(0x80 | (c & 0x3F));
    } else {
        put(0xF0 | (c >> 18));
        put(0x80 | ((c >> 12) & 0x3F));
        put(0x80 | ((c >> 6) & 0x3F));
        put(0x80 | (c & 0x3F));
    }
}

// The length (1-4) of the UTF-8 sequence that a well-formed lead byte starts.
[[nodiscard]] constexpr std::size_t utf8SequenceLength(std::uint8_t lead) noexcept {
    return lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
}

// Decodes the scalar of the well-formed UTF-8 sequence at `i` and stores its length in `length`.
// Precondition: a well-formed sequence starts at `i` (the caller validated the text); a lead or
// continuation byte out of place fails a FROST_ASSERT. O(1).
[[nodiscard]] inline char32_t decodeValidUtf8Scalar(std::string_view s, std::size_t i, std::size_t& length) noexcept {
    const std::uint8_t b0 = octetAt(s, i);
    length = utf8SequenceLength(b0);
    FROST_ASSERT(!isContinuation(b0) && s.size() - i >= length);
    if (length == 1) {
        return b0;
    }
    std::uint32_t value = b0 & (length == 2 ? 0x1Fu : length == 3 ? 0x0Fu : 0x07u);
    for (std::size_t k = 1; k < length; ++k) {
        const std::uint8_t b = octetAt(s, i + k);
        FROST_ASSERT(isContinuation(b));
        value = (value << 6) | (b & 0x3Fu);
    }
    return static_cast<char32_t>(value);
}

// The largest char boundary <= i of well-formed UTF-8 `s` (i <= s.size()). O(1): at most 3 steps.
[[nodiscard]] inline std::size_t floorCharBoundary(std::string_view s, std::size_t i) noexcept {
    while (i > 0 && i < s.size() && isContinuation(octetAt(s, i))) {
        --i;
    }
    return i;
}

// The shape table 3-7 gives a multi-byte sequence by its lead byte: the sequence length (2-4) and
// the range of its second byte; later bytes are always 80..BF. Length 0: the byte cannot start a
// multi-byte sequence (00..C1, F5..FF).
struct SequenceShape {
    std::size_t length = 0;
    std::uint8_t secondLow = 0x80;
    std::uint8_t secondHigh = 0xBF;
};

// The one copy of table 3-7's lead-byte rows, shared by the lossy UTF-8 decoder's maximal subparts
// and isProperUtf8Prefix (detection's cut samples). O(1).
[[nodiscard]] constexpr SequenceShape multiByteShape(std::uint8_t lead) noexcept {
    if (lead >= 0xC2 && lead <= 0xDF) {
        return {2, 0x80, 0xBF};
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return {3, lead == 0xE0 ? std::uint8_t{0xA0} : std::uint8_t{0x80},
                lead == 0xED ? std::uint8_t{0x9F} : std::uint8_t{0xBF}};
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        return {4, lead == 0xF0 ? std::uint8_t{0x90} : std::uint8_t{0x80},
                lead == 0xF4 ? std::uint8_t{0x8F} : std::uint8_t{0xBF}};
    }
    return {};
}

/*
 * True when `tail` (1..3 bytes) is a proper prefix of a well-formed UTF-8 sequence (table 3-7): a
 * valid lead byte whose second byte, if present, is in the lead's second-byte range and whose
 * later bytes are 80..BF, with fewer bytes than the sequence needs. The streaming decoders leave
 * such a tail unconsumed with InputEnd::More. O(1).
 */
[[nodiscard]] inline bool isProperUtf8Prefix(std::string_view tail) noexcept {
    if (tail.empty() || tail.size() > 3) {
        return false;
    }
    const SequenceShape shape = multiByteShape(octetAt(tail, 0));
    if (shape.length == 0 || tail.size() >= shape.length) {
        return false;
    }
    for (std::size_t k = 1; k < tail.size(); ++k) {
        const std::uint8_t b = octetAt(tail, k);
        const std::uint8_t low = k == 1 ? shape.secondLow : std::uint8_t{0x80};
        const std::uint8_t high = k == 1 ? shape.secondHigh : std::uint8_t{0xBF};
        if (b < low || b > high) {
            return false;
        }
    }
    return true;
}

/*
 * Appends the UTF-8 of UTF-16 code units already in native order (char16_t, or wchar_t on
 * Windows). A surrogate pair becomes one scalar; an unpaired surrogate is appended as its 3-byte
 * pattern, which is ill-formed UTF-8, so a caller that validates its output (decodeLegacy) reports
 * the codec bug instead of hiding it. O(n).
 */
template<class Unit>
void appendUtf16UnitsAsUtf8(std::basic_string_view<Unit> units, std::string& out) noexcept {
    const std::size_t n = units.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto u = static_cast<std::uint32_t>(static_cast<std::uint16_t>(units[i]));
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
            continue;
        }
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            const auto v = static_cast<std::uint32_t>(static_cast<std::uint16_t>(units[i + 1]));
            if (v >= 0xDC00 && v <= 0xDFFF) {
                appendScalarUtf8(static_cast<char32_t>(0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00)), out);
                ++i;
                continue;
            }
        }
        appendScalarUtf8(static_cast<char32_t>(u), out);
    }
}

} // namespace frost::text::detail
