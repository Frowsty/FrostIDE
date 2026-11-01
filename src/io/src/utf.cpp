// SPDX-License-Identifier: Apache-2.0
//
// Strict UTF-8 <-> UTF-16 codec (see frost/io/utf.hpp). The validation follows
// the Unicode Standard, chapter 3, table 3-7; the encoder is the standard bit
// layout from the same chapter.

#include "frost/io/utf.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>

namespace frost::io {
namespace {

// Decodes one well-formed UTF-8 sequence starting at `in[i]`. On success stores
// the code point and returns the sequence length (1-4); returns 0 if ill-formed.
std::size_t decodeUtf8(std::string_view in, std::size_t i, char32_t& out) noexcept {
    const auto byte = [&](std::size_t k) { return static_cast<std::uint8_t>(in[k]); };
    const std::uint8_t b0 = byte(i);
    if (b0 < 0x80) {
        out = b0;
        return 1;
    }
    const std::size_t left = in.size() - i;
    // Table 3-7: the second byte's range depends on the lead byte; later bytes are 80..BF.
    std::uint8_t lo = 0x80;
    std::uint8_t hi = 0xBF;
    std::size_t length = 0;
    char32_t cp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        length = 2;
        cp = b0 & 0x1Fu;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        length = 3;
        cp = b0 & 0x0Fu;
        if (b0 == 0xE0) {
            lo = 0xA0; // Rejects overlong 3-byte forms.
        } else if (b0 == 0xED) {
            hi = 0x9F; // Rejects encoded surrogates U+D800..U+DFFF.
        }
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        length = 4;
        cp = b0 & 0x07u;
        if (b0 == 0xF0) {
            lo = 0x90; // Rejects overlong 4-byte forms.
        } else if (b0 == 0xF4) {
            hi = 0x8F; // Rejects code points above U+10FFFF.
        }
    } else {
        return 0; // 80..C1 (continuation or overlong 2-byte lead) and F5..FF never start a sequence.
    }
    if (left < length) {
        return 0;
    }
    const std::uint8_t b1 = byte(i + 1);
    if (b1 < lo || b1 > hi) {
        return 0;
    }
    cp = (cp << 6) | (b1 & 0x3Fu);
    for (std::size_t k = 2; k < length; ++k) {
        const std::uint8_t b = byte(i + k);
        if (b < 0x80 || b > 0xBF) {
            return 0;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    out = cp;
    return length;
}

// Converts UTF-8 to a 16-bit string type (std::u16string or, on Windows, std::wstring).
template<class String>
Result<String> toUtf16(std::string_view in) {
    // First pass validates and counts UTF-16 units, so the output is allocated once.
    std::size_t units = 0;
    for (std::size_t i = 0; i < in.size();) {
        char32_t cp = 0;
        const std::size_t n = decodeUtf8(in, i, cp);
        if (n == 0) {
            return fail(ErrorCode::InvalidArgument, std::format("invalid UTF-8 at byte offset {}", i));
        }
        units += cp >= 0x10000 ? 2 : 1;
        i += n;
    }
    String out;
    out.resize(units);
    std::size_t o = 0;
    for (std::size_t i = 0; i < in.size();) {
        char32_t cp = 0;
        i += decodeUtf8(in, i, cp);
        using Unit = typename String::value_type;
        if (cp >= 0x10000) {
            const char32_t v = cp - 0x10000;
            out[o++] = static_cast<Unit>(0xD800 + (v >> 10));
            out[o++] = static_cast<Unit>(0xDC00 + (v & 0x3FF));
        } else {
            out[o++] = static_cast<Unit>(cp);
        }
    }
    return out;
}

// Converts a 16-bit string view (char16_t or, on Windows, wchar_t) to UTF-8.
template<class Unit>
Result<std::string> fromUtf16(std::basic_string_view<Unit> in) {
    const auto unit = [&](std::size_t k) { return static_cast<char32_t>(static_cast<std::uint16_t>(in[k])); };
    // First pass validates surrogate pairing and counts bytes.
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char32_t u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= in.size() || unit(i + 1) < 0xDC00 || unit(i + 1) > 0xDFFF) {
                return fail(ErrorCode::InvalidArgument, std::format("unpaired high surrogate at UTF-16 offset {}", i));
            }
            bytes += 4;
            ++i;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return fail(ErrorCode::InvalidArgument, std::format("unpaired low surrogate at UTF-16 offset {}", i));
        } else {
            bytes += u < 0x80 ? 1 : (u < 0x800 ? 2 : 3);
        }
    }
    std::string out;
    out.resize(bytes);
    std::size_t o = 0;
    const auto put = [&](char32_t b) { out[o++] = static_cast<char>(static_cast<std::uint8_t>(b)); };
    for (std::size_t i = 0; i < in.size(); ++i) {
        char32_t cp = unit(i);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (unit(i + 1) - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            put(cp);
        } else if (cp < 0x800) {
            put(0xC0 | (cp >> 6));
            put(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            put(0xE0 | (cp >> 12));
            put(0x80 | ((cp >> 6) & 0x3F));
            put(0x80 | (cp & 0x3F));
        } else {
            put(0xF0 | (cp >> 18));
            put(0x80 | ((cp >> 12) & 0x3F));
            put(0x80 | ((cp >> 6) & 0x3F));
            put(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

} // namespace

std::size_t validUtf8Prefix(std::string_view utf8) noexcept {
    const std::size_t size = utf8.size();
    std::size_t i = 0;
    while (i < size) {
        // ASCII fast path: skip 8 bytes at a time while none has its high bit set. memcpy
        // keeps the load free of alignment and aliasing assumptions.
        while (size - i >= 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, utf8.data() + i, sizeof word);
            if ((word & 0x8080'8080'8080'8080ULL) != 0) {
                break;
            }
            i += 8;
        }
        if (i >= size) {
            break;
        }
        char32_t cp = 0;
        const std::size_t n = decodeUtf8(utf8, i, cp);
        if (n == 0) {
            return i;
        }
        i += n;
    }
    return size;
}

bool isValidUtf8(std::string_view utf8) noexcept { return validUtf8Prefix(utf8) == utf8.size(); }

Result<std::u16string> utf8ToUtf16(std::string_view utf8) { return toUtf16<std::u16string>(utf8); }

Result<std::string> utf16ToUtf8(std::u16string_view utf16) { return fromUtf16(utf16); }

#ifdef _WIN32
static_assert(sizeof(wchar_t) == sizeof(char16_t), "Win32 wchar_t is a UTF-16 code unit");

Result<std::wstring> toWide(std::string_view utf8) { return toUtf16<std::wstring>(utf8); }

Result<std::string> fromWide(std::wstring_view wide) { return fromUtf16(wide); }
#endif

} // namespace frost::io
