// SPDX-License-Identifier: Apache-2.0
//
// frost::settings internals: JSON parsing, decoding and encoding over yyjson (ADR 0021) and the
// span scanner SettingsDocument's byte-exact edits rely on. Only src/settings/src includes this, so
// yyjson.h stays out of every public header.
#pragma once

#include "frost/core/error.hpp"
#include "frost/settings/value.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::settings::detail {

// Where one top-level member lies in the text (offsets into the whole text, BOM included).
struct MemberSpan {
    std::size_t keyBegin = 0;
    std::size_t keyEnd = 0;
    std::size_t valueBegin = 0;
    std::size_t valueEnd = 0;
    std::size_t commaEnd = 0; // 0: no comma after the value.
};

struct ParsedObject {
    bool hasObject = false; // false: only a BOM, whitespace and comments.
    std::size_t open = 0;
    std::size_t close = 0;
    std::vector<MemberSpan> spans;
    std::vector<std::string> keys; // Unescaped, parallel to spans.
};

/*
 * Parses a scope file's text: an optional UTF-8 BOM, then one JSON object (comments and trailing
 * commas allowed), or nothing but whitespace and comments. Errors: Corrupt, with "line L, column C:
 * reason". O(n).
 */
[[nodiscard]] Result<ParsedObject> parseObject(std::string_view text);

// Checks that `json` is exactly one JSON value (comments allowed). Errors: InvalidArgument.
[[nodiscard]] Result<void> checkSingleValue(std::string_view json);

// Decodes one JSON value as `schema.type` (no range checks). Errors: InvalidArgument naming the
// expected type ("must be an integer").
[[nodiscard]] Result<Value> decodeValue(std::string_view json, const Schema& schema);

// Writes `value` as compact JSON. Errors: InvalidArgument (NaN, infinities, invalid UTF-8).
[[nodiscard]] Result<std::string> encodeValue(const Value& value);

// A JSON string literal for `text` (quotes included). Errors: InvalidArgument (invalid UTF-8).
[[nodiscard]] Result<std::string> encodeString(std::string_view text);

// The 1-based line and column (in code points) of byte `offset`. O(offset).
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> lineColumn(std::string_view text, std::size_t offset) noexcept;

} // namespace frost::settings::detail
