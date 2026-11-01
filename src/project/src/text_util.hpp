// SPDX-License-Identifier: Apache-2.0
//
// Private string helpers of frost::project: ASCII case folding, trimming, splitting, GUID
// normalisation and decoding of text files (BOMs). Pure and thread-safe.
#pragma once

#include "frost/core/error.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace frost::project::detail {

[[nodiscard]] constexpr char toLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr char toUpperAscii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

[[nodiscard]] std::string toLowerAscii(std::string_view text);
[[nodiscard]] std::string toUpperAscii(std::string_view text);

// Equality with ASCII case ignored.
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] bool istartsWith(std::string_view text, std::string_view prefix) noexcept;
[[nodiscard]] bool iendsWith(std::string_view text, std::string_view suffix) noexcept;
// Three-way comparison with ASCII case ignored (then byte order, so the order is total).
[[nodiscard]] int icompare(std::string_view a, std::string_view b) noexcept;

// Removes spaces, tabs, CR and LF at both ends.
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;

// Splits at every `separator`; pieces are trimmed and empty ones dropped.
[[nodiscard]] std::vector<std::string_view> splitTrimmed(std::string_view text, char separator);

// "{abc...}" or "abc..." to "{ABC...}"; other text is trimmed and upper-cased only.
[[nodiscard]] std::string normalizeGuid(std::string_view guid);

// The extension of a file name including the dot, lower-cased ("" when none).
[[nodiscard]] std::string lowerExtension(std::string_view fileName);

// The file name without its last extension.
[[nodiscard]] std::string_view stem(std::string_view fileName) noexcept;

// Text file contents as UTF-8: a UTF-8 BOM is dropped, UTF-16 LE/BE with a BOM is converted.
// Errors: Corrupt for UTF-16 that does not convert (odd length, unpaired surrogates).
[[nodiscard]] Result<std::string> decodeText(std::string_view bytes, std::string_view path);

} // namespace frost::project::detail
