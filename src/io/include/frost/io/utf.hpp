// SPDX-License-Identifier: Apache-2.0
//
// Strict UTF-8 <-> UTF-16 conversion for the OS boundary (frost::io).
//
/*
 * FrostIDE stores and passes text as UTF-8 everywhere above io; Win32 wants
 * UTF-16. These functions convert between the two with a strict policy:
 *
 *   - UTF-8 input must be well formed as defined by the Unicode Standard,
 *     chapter 3, table 3-7 ("Well-Formed UTF-8 Byte Sequences"): no overlong
 *     forms, no encoded surrogates (U+D800..U+DFFF), nothing above U+10FFFF,
 *     no truncated sequences and no stray continuation bytes.
 *   - UTF-16 input must not contain unpaired surrogates.
 *   - Invalid input is never replaced with U+FFFD and never passed through
 *     (no WTF-8): the call fails with ErrorCode::InvalidArgument and a message
 *     giving the offset of the first offending code unit. Silent replacement
 *     would let two different file names map to the same string.
 *   - U+0000 is a valid code point and is converted like any other; callers
 *     that hand strings to NUL-terminated OS APIs reject it themselves
 *     (frost/io/path.hpp does for paths).
 *
 * The codec is a straightforward implementation of table 3-7 (no third-party
 * code). All functions are pure and thread-safe. Cost is O(n) with an ASCII
 * fast path; the output is allocated once at its final size.
 */
#pragma once

#include "frost/core/error.hpp"

#include <string>
#include <string_view>

namespace frost::io {

// True when `utf8` is well-formed UTF-8 (policy above). O(n), never allocates.
[[nodiscard]] bool isValidUtf8(std::string_view utf8) noexcept;

// Converts UTF-8 to UTF-16. Errors: InvalidArgument for ill-formed input.
[[nodiscard]] Result<std::u16string> utf8ToUtf16(std::string_view utf8);

// Converts UTF-16 to UTF-8. Errors: InvalidArgument for unpaired surrogates.
[[nodiscard]] Result<std::string> utf16ToUtf8(std::u16string_view utf16);

#ifdef _WIN32
// The same conversions for Win32's wchar_t strings (UTF-16 on Windows), so call
// sites need no casts. Same policy and errors as utf8ToUtf16/utf16ToUtf8.
[[nodiscard]] Result<std::wstring> toWide(std::string_view utf8);
[[nodiscard]] Result<std::string> fromWide(std::wstring_view wide);
#endif

} // namespace frost::io
