// SPDX-License-Identifier: Apache-2.0
//
// The editor model's small per-language table (ADR 0025): comment tokens for the comment commands,
// looked up by language id or by file name. tree-sitter grammars (m1/syntax) and the settings
// registry will grow this into a real language registry; until then this is the one table.
#pragma once

#include <span>
#include <string_view>

namespace frost::text {

/*
 * One language's editing facts. Empty tokens mean "none": a language without a line comment
 * comments lines with its block tokens, and one with neither has no comment commands. A plain
 * value over string literals; the table entries live for the whole program.
 */
struct LanguageInfo {
    std::string_view id;          // Stable id, e.g. "cpp".
    std::string_view name;        // Display name, e.g. "C++".
    std::string_view lineComment; // e.g. "//".
    std::string_view blockOpen;   // e.g. "/*".
    std::string_view blockClose;  // e.g. "*/".

    [[nodiscard]] constexpr bool hasLineComment() const noexcept { return !lineComment.empty(); }
    [[nodiscard]] constexpr bool hasBlockComment() const noexcept { return !blockOpen.empty() && !blockClose.empty(); }
};

// Every language in the table, plain text first. O(1).
[[nodiscard]] std::span<const LanguageInfo> languages() noexcept;
// Plain text: no comment tokens. O(1).
[[nodiscard]] const LanguageInfo& plainTextLanguage() noexcept;
// The language with `id`, or nullptr. O(languages).
[[nodiscard]] const LanguageInfo* languageById(std::string_view id) noexcept;
// The language of a file by its name (CMakeLists.txt) or extension (case-insensitive; the last
// path component of `path` is used), or plain text. O(extensions).
[[nodiscard]] const LanguageInfo& languageForPath(std::string_view path) noexcept;

} // namespace frost::text
