// SPDX-License-Identifier: Apache-2.0
//
// Private UTF-8 decoding shared by frost::input's implementation files (not installed):
// the keymap parser's validation (which needs the offset of the first bad byte, so
// frost::io::isValidUtf8 does not fit, and frost::input does not link frost::io) and Find
// Action's code-point matching.
#pragma once

#include <cstddef>
#include <string_view>

namespace frost::input::detail {

// One decoded UTF-8 sequence: the code point and its length in bytes. An invalid byte
// (a bad lead or continuation byte, a truncated, overlong or surrogate form, or a value
// above U+10FFFF; the well-formed table of the Unicode Standard, section 3.9) decodes as
// valid = false with length 1 and cp = 0x110000 + the byte, so it can never equal a code
// point. Precondition: i < s.size(). O(1), noexcept.
struct Utf8Unit {
    char32_t cp = 0;
    std::size_t length = 1;
    bool valid = true;
};

[[nodiscard]] constexpr Utf8Unit decodeUtf8At(std::string_view s, std::size_t i) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    const Utf8Unit invalid{.cp = char32_t{0x110000} + b0, .length = 1, .valid = false};
    if (b0 < 0x80) {
        return Utf8Unit{.cp = char32_t{b0}, .length = 1, .valid = true};
    }
    std::size_t length = 0;
    char32_t min = 0;
    char32_t c = 0;
    if ((b0 & 0xE0) == 0xC0) {
        length = 2;
        min = 0x80;
        c = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        length = 3;
        min = 0x800;
        c = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        length = 4;
        min = 0x10000;
        c = b0 & 0x07;
    } else {
        return invalid;
    }
    if (i + length > s.size()) {
        return invalid;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            return invalid;
        }
        c = (c << 6) | (b & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        return invalid;
    }
    return Utf8Unit{.cp = c, .length = length, .valid = true};
}

// The byte offset of the first invalid UTF-8 sequence in `s`, or npos. O(n), noexcept.
[[nodiscard]] constexpr std::size_t invalidUtf8At(std::string_view s) noexcept {
    for (std::size_t i = 0; i < s.size();) {
        const Utf8Unit unit = decodeUtf8At(s, i);
        if (!unit.valid) {
            return i;
        }
        i += unit.length;
    }
    return std::string_view::npos;
}

static_assert(invalidUtf8At("a\xC3\xBC\xE2\x82\xAC\xF0\x9F\x98\x80") == std::string_view::npos);
static_assert(invalidUtf8At("ab\xC0\xAF") == 2, "overlong");
static_assert(invalidUtf8At("\xED\xA0\x80") == 0, "surrogate");
static_assert(invalidUtf8At("x\xF4\x90\x80\x80") == 1, "above U+10FFFF");
static_assert(invalidUtf8At("\xE2\x82") == 0, "truncated");
static_assert(decodeUtf8At("\xFF", 0).cp == 0x1100FF);

} // namespace frost::input::detail
