// SPDX-License-Identifier: Apache-2.0
//
// SettingsDocument: one JSON scope file, kept as its exact bytes, with edits that touch only the
// member they change (PLAN.md section 3.6: diffable JSON, unknown keys survive round trips; ADR 0021).
/*
 * Format. A UTF-8 file holding one JSON object whose members are flat dotted keys
 * ("editor.tabSize": 4) plus "schemaVersion". An optional UTF-8 BOM, // and block comments, and
 * trailing commas are accepted (users edit these files by hand). Whitespace or comments alone, or
 * an empty file, is an empty document. Parsing and value decoding use yyjson; a small scanner over
 * the validated text records where each top-level member's key and value lie.
 *
 * Byte-exact edits. set() replaces only the value's bytes; a new member is appended after the last
 * one, with the indentation and line break (LF or CRLF) the file already uses; remove() deletes the
 * member with its separator; rename() rewrites only the key. Every other byte, comments and unknown
 * members included, is kept. After each edit the whole text is parsed again, so an edit can never
 * leave invalid JSON behind (O(file size) per edit; scope files are small).
 *
 * Duplicate keys: the last occurrence wins (find(), value(), set()); remove() removes every one.
 * A plain value: copyable, movable, thread-compatible.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/settings/value.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::settings {

// The member that stamps a scope file's format version (migration.hpp).
inline constexpr std::string_view kSchemaVersionKey = "schemaVersion";

class SettingsDocument {
public:
    // One top-level member; offsets index bytes().
    struct Member {
        std::string key;            // Unescaped.
        std::size_t keyBegin = 0;   // The opening quote of the key.
        std::size_t keyEnd = 0;     // One past the closing quote.
        std::size_t valueBegin = 0; // The value's first byte.
        std::size_t valueEnd = 0;   // One past its last byte.
        std::size_t commaEnd = 0;   // One past the ',' after the value; 0 when there is none.
        std::uint32_t line = 1;     // 1-based line of the key.
        std::uint32_t column = 1;   // 1-based column (in code points) of the key.
    };

    // An empty document ("" bytes).
    SettingsDocument() = default;

    /*
     * Parses `bytes` (see the file comment). Errors: Corrupt with "line L, column C: reason" for
     * invalid JSON, a top-level value that is not an object, or text that is not UTF-8. O(n).
     */
    [[nodiscard]] static Result<SettingsDocument> parse(std::string bytes);

    // A new file's text: "{\n  \"schemaVersion\": <version>\n}\n".
    [[nodiscard]] static SettingsDocument create(std::uint32_t schemaVersion);

    [[nodiscard]] const std::string& bytes() const noexcept { return m_bytes; }
    // True when the text holds no object (empty, whitespace or comments only).
    [[nodiscard]] bool isEmpty() const noexcept { return !m_hasObject; }
    [[nodiscard]] std::span<const Member> members() const noexcept { return m_members; }
    // The last member named `key`, or nullptr. O(members).
    [[nodiscard]] const Member* find(std::string_view key) const noexcept;
    // The JSON text of a member's value. Precondition: `member` belongs to this document.
    [[nodiscard]] std::string_view rawValue(const Member& member) const noexcept;

    // Decodes `key`'s value with `schema`'s type and checks it (validateValue). Errors: NotFound
    // (absent), InvalidArgument (wrong JSON type or failed check, with the reason). O(value size).
    [[nodiscard]] Result<Value> value(std::string_view key, const Schema& schema) const;

    // The schemaVersion member when it is an integer in [0, 2^32); nullopt when absent or invalid.
    [[nodiscard]] std::optional<std::uint32_t> schemaVersion() const;

    /*
     * Sets `key` to `value` (replacing the last occurrence's value, or appending a member). An
     * empty document becomes an object. Errors: InvalidArgument (a value that cannot be written as
     * JSON, such as NaN or a string that is not UTF-8); Internal if the edit did not reparse.
     */
    [[nodiscard]] Result<void> set(std::string_view key, const Value& value);
    // As set(), with `json` written verbatim. Errors: InvalidArgument when `json` is not one JSON value.
    [[nodiscard]] Result<void> setRaw(std::string_view key, std::string_view json);
    // Writes schemaVersion: replaces its value, or inserts it as the first member.
    [[nodiscard]] Result<void> setSchemaVersion(std::uint32_t version);
    // Removes every member named `key`; true when one existed. Errors: Internal (reparse).
    [[nodiscard]] Result<bool> remove(std::string_view key);
    /*
     * Renames member `from` to `to`, keeping its value bytes and position. When `to` already
     * exists, `from` is removed instead (the newer key wins). True when `from` existed.
     * Errors: Internal (reparse).
     */
    [[nodiscard]] Result<bool> rename(std::string_view from, std::string_view to);

private:
    [[nodiscard]] Result<void> replaceBytes(std::string bytes);
    [[nodiscard]] Result<void> insertMember(std::string_view key, std::string_view json, bool atFront);

    std::string m_bytes;
    bool m_hasObject = false;
    std::size_t m_open = 0;  // The '{'.
    std::size_t m_close = 0; // The '}'.
    std::vector<Member> m_members;
};

} // namespace frost::settings
