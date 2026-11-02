// SPDX-License-Identifier: Apache-2.0
//
// FileMask: the file-name filter of Find and Replace in Files (ADR 0045), Rider's "File mask"
// field: a list of glob patterns such as "*.cpp, *.h, !*.generated.h".
/*
 * Syntax. Patterns are separated by commas or semicolons; spaces around them are dropped. A
 * pattern with a leading '!' excludes. '*' matches any run of characters (also none), '?' exactly
 * one character (a whole UTF-8 sequence); every other character matches itself with ASCII case
 * ignored on every platform (Rider's masks are case-insensitive). A pattern matches the file name
 * (the last path component) only.
 *
 * A file passes when it matches an inclusion, or there is no inclusion, and matches no exclusion;
 * so the empty mask and a mask of exclusions alone include every other file. A plain value; every
 * member is const and thread-safe.
 */
#pragma once

#include "frost/core/error.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace frost::search {

class FileMask {
public:
    // The mask that passes every file.
    FileMask() = default;

    // Parses `text` (header comment). Errors: InvalidArgument for a '!' with no pattern after it.
    // O(n).
    [[nodiscard]] static Result<FileMask> parse(std::string_view text);

    // Whether `fileName` (one path component) passes (header comment). O(patterns x name) for
    // typical patterns.
    [[nodiscard]] bool matches(std::string_view fileName) const noexcept;
    // True when every file passes (no pattern at all).
    [[nodiscard]] bool matchesAll() const noexcept { return m_include.empty() && m_exclude.empty(); }

    [[nodiscard]] const std::vector<std::string>& inclusions() const noexcept { return m_include; }
    [[nodiscard]] const std::vector<std::string>& exclusions() const noexcept { return m_exclude; }

private:
    std::vector<std::string> m_include;
    std::vector<std::string> m_exclude;
};

// Whether `name` matches the glob `pattern` ('*', '?', ASCII case ignored; header comment). O(n m)
// in the worst case, linear for patterns with one '*'.
[[nodiscard]] bool globMatch(std::string_view pattern, std::string_view name) noexcept;

} // namespace frost::search
