// SPDX-License-Identifier: Apache-2.0
//
// A read-only JSON tree over yyjson, private to frost::project, for CMakePresets.json and the CMake
// File API reply files (ADR 0046). yyjson (MIT, ADR 0021) is linked PRIVATE, so no layer above
// project sees yyjson.h; frost::lang has its own Json for the LSP, which stays separate on purpose
// (lang sits above project and owns its writer).
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project::detail {

/*
 * One JSON value. Objects keep their members in document order; a duplicate key keeps both
 * entries and find() returns the first. Numbers keep an int64 view when they are integral. A
 * plain value type: copyable, thread-compatible.
 */
struct JsonNode {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    std::optional<std::int64_t> integer{}; // Set for integral numbers.
    double number = 0.0;
    std::string text{};              // A String's UTF-8.
    std::vector<JsonNode> items{};   // Array items or object values, in order.
    std::vector<std::string> keys{}; // Object keys, parallel to items.

    [[nodiscard]] bool isNull() const noexcept { return kind == Kind::Null; }
    [[nodiscard]] bool isBool() const noexcept { return kind == Kind::Bool; }
    [[nodiscard]] bool isString() const noexcept { return kind == Kind::String; }
    [[nodiscard]] bool isArray() const noexcept { return kind == Kind::Array; }
    [[nodiscard]] bool isObject() const noexcept { return kind == Kind::Object; }
    [[nodiscard]] bool isNumber() const noexcept { return kind == Kind::Number; }

    // The first member named `key` of an object, or nullptr (also for other kinds). O(members).
    [[nodiscard]] const JsonNode* find(std::string_view key) const noexcept;
    // A string member's text, or `fallback` when it is missing or not a string.
    [[nodiscard]] std::string stringAt(std::string_view key, std::string_view fallback = {}) const;
    // A bool member, or `fallback`.
    [[nodiscard]] bool boolAt(std::string_view key, bool fallback = false) const noexcept;
    // An integral number member, or nullopt.
    [[nodiscard]] std::optional<std::int64_t> integerAt(std::string_view key) const noexcept;
};

// Parses one JSON text (a UTF-8 BOM is skipped). Nesting deeper than 128 levels is refused.
// Errors: Corrupt with yyjson's reason and the byte position. O(size).
[[nodiscard]] Result<JsonNode> parseJson(std::string_view text);

} // namespace frost::project::detail
