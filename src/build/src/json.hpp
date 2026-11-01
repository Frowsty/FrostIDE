// SPDX-License-Identifier: Apache-2.0
//
// A minimal RFC 8259 JSON reader (private to frost::build) for vswhere's -format json output: a
// value tree with strings decoded (\uXXXX escapes and surrogate pairs included), numbers kept as
// their text, and a depth limit. Not a general-purpose parser: objects keep their members in
// order, duplicates included, and lookups take the first.
#pragma once

#include "frost/core/error.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace frost::build::detail {

struct JsonValue {
    enum class Kind : unsigned char { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text{}; // A String's decoded UTF-8, or a Number's literal text.
    std::vector<JsonValue> items{};
    // An Object's members in document order, duplicates included: memberNames[i] names
    // memberValues[i]. Two vectors, because libstdc++ refuses a vector of pairs (or of a member
    // struct) whose element holds the still-incomplete JsonValue.
    std::vector<std::string> memberNames{};
    std::vector<JsonValue> memberValues{};

    // The first member named `name` of an Object, or nullptr (also for other kinds).
    [[nodiscard]] const JsonValue* find(std::string_view name) const noexcept;
    // A String member's text, or "" when the member is missing or not a string.
    [[nodiscard]] std::string stringMember(std::string_view name) const;
    // A Bool member's value, or `fallback`.
    [[nodiscard]] bool boolMember(std::string_view name, bool fallback = false) const noexcept;
};

// Parses one JSON document (surrounding whitespace allowed; a UTF-8 BOM is skipped). Nesting
// deeper than 64 levels is refused. Errors: Corrupt (not well-formed JSON, with the byte offset).
[[nodiscard]] Result<JsonValue> parseJson(std::string_view text);

} // namespace frost::build::detail
