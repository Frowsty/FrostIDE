// SPDX-License-Identifier: Apache-2.0
//
// FileMask parsing and glob matching (file_mask.hpp). The matcher is the classic iterative
// wildcard walk: on a mismatch after a '*', the star absorbs one more character and the walk
// resumes behind it, so no recursion and no backtracking stack.

#include "frost/search/file_mask.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <utility>

namespace frost::search {
namespace {

[[nodiscard]] constexpr char foldAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// The length of the UTF-8 sequence that starts with `lead` (1 for ASCII and stray bytes).
[[nodiscard]] constexpr std::size_t sequenceLength(unsigned char lead) noexcept {
    if (lead >= 0xF0 && lead <= 0xF4) {
        return 4;
    }
    if (lead >= 0xE0) {
        return lead <= 0xEF ? 3 : 1;
    }
    if (lead >= 0xC2) {
        return 2;
    }
    return 1;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

bool globMatch(std::string_view pattern, std::string_view name) noexcept {
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t starP = std::string_view::npos; // The pattern position after the last '*'.
    std::size_t starN = 0;                      // The name position that star resumes from.
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = ++p;
            starN = n;
            continue;
        }
        if (p < pattern.size() && pattern[p] == '?') {
            ++p;
            n += std::min(sequenceLength(static_cast<unsigned char>(name[n])), name.size() - n);
            continue;
        }
        if (p < pattern.size() && foldAscii(pattern[p]) == foldAscii(name[n])) {
            ++p;
            ++n;
            continue;
        }
        if (starP == std::string_view::npos) {
            return false;
        }
        // The last star takes one more character of the name.
        p = starP;
        n = ++starN;
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

Result<FileMask> FileMask::parse(std::string_view text) {
    FileMask mask;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find_first_of(",;", start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view part = trim(text.substr(start, end - start));
        start = end + 1;
        if (part.empty()) {
            continue;
        }
        if (part.front() == '!') {
            part = trim(part.substr(1));
            if (part.empty()) {
                return fail(ErrorCode::InvalidArgument, std::format("file mask '{}': '!' needs a pattern", text));
            }
            mask.m_exclude.emplace_back(part);
        } else {
            mask.m_include.emplace_back(part);
        }
    }
    return mask;
}

bool FileMask::matches(std::string_view fileName) const noexcept {
    bool included = m_include.empty();
    for (const std::string& pattern : m_include) {
        if (globMatch(pattern, fileName)) {
            included = true;
            break;
        }
    }
    if (!included) {
        return false;
    }
    for (const std::string& pattern : m_exclude) {
        if (globMatch(pattern, fileName)) {
            return false;
        }
    }
    return true;
}

} // namespace frost::search
