// SPDX-License-Identifier: Apache-2.0
//
// The camel-hump fuzzy matcher (fuzzy.hpp): an O(pattern x name) dynamic programme.

#include "frost/lang/fuzzy.hpp"

#include <algorithm>
#include <limits>

namespace frost::lang {
namespace {

constexpr std::size_t kMaxName = 512;
constexpr int kNone = std::numeric_limits<int>::min() / 4;
constexpr int kWordStartBonus = 8;
constexpr int kConsecutiveBonus = 5;
constexpr int kCaseBonus = 1;
constexpr int kMaxGapPenalty = 3;

[[nodiscard]] constexpr bool isLower(char c) noexcept { return c >= 'a' && c <= 'z'; }
[[nodiscard]] constexpr bool isUpper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
[[nodiscard]] constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] constexpr bool isLetter(char c) noexcept {
    return isLower(c) || isUpper(c) || static_cast<unsigned char>(c) >= 0x80;
}
[[nodiscard]] constexpr char lower(char c) noexcept { return isUpper(c) ? static_cast<char>(c - 'A' + 'a') : c; }
[[nodiscard]] constexpr bool sameChar(char a, char b) noexcept { return lower(a) == lower(b); }

[[nodiscard]] bool startsWithIgnoreCase(std::string_view name, std::string_view prefix) noexcept {
    return name.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), name.begin(), [](char a, char b) { return sameChar(a, b); });
}

} // namespace

bool isWordStart(std::string_view name, std::size_t index) noexcept {
    if (index >= name.size()) {
        return false;
    }
    if (index == 0) {
        return true;
    }
    const char previous = name[index - 1];
    const char current = name[index];
    if (!isLetter(current) && !isDigit(current)) {
        return false; // Separators themselves are never word starts.
    }
    if (!isLetter(previous) && !isDigit(previous)) {
        return true; // After '_', ':', '.', ' ', ...
    }
    if (isUpper(current) && isLower(previous)) {
        return true; // Camel hump.
    }
    return isDigit(current) && isLetter(previous);
}

std::optional<FuzzyMatch> fuzzyMatch(std::string_view pattern, std::string_view name) {
    if (pattern.empty()) {
        return FuzzyMatch{.kind = MatchKind::Prefix, .score = 0, .positions = {}};
    }
    name = name.substr(0, std::min(name.size(), kMaxName));
    const std::size_t n = pattern.size();
    const std::size_t m = name.size();
    if (n > m) {
        return std::nullopt;
    }
    // best[i * m + j]: the best score with pattern[i] matched at name[j]; parent: where pattern[i - 1] matched.
    std::vector<int> best(n * m, kNone);
    std::vector<int> parent(n * m, -1);
    for (std::size_t j = 0; j < m; ++j) {
        if (sameChar(pattern[0], name[j]) && isWordStart(name, j)) {
            best[j] = 1 + kWordStartBonus + (pattern[0] == name[j] ? kCaseBonus : 0) -
                      static_cast<int>(std::min<std::size_t>(j, kMaxGapPenalty));
        }
    }
    for (std::size_t i = 1; i < n; ++i) {
        const int* previous = &best[(i - 1) * m];
        int farBest = kNone; // max over k <= j - 4 of previous[k] (gap penalty capped)
        int farIndex = -1;
        for (std::size_t j = i; j < m; ++j) {
            if (j >= 4 && previous[j - 4] > farBest) {
                farBest = previous[j - 4];
                farIndex = static_cast<int>(j - 4);
            }
            if (!sameChar(pattern[i], name[j])) {
                continue;
            }
            int candidate = kNone;
            int from = -1;
            const auto consider = [&](int score, int index) {
                if (score > candidate) {
                    candidate = score;
                    from = index;
                }
            };
            if (previous[j - 1] != kNone) {
                consider(previous[j - 1] + kConsecutiveBonus, static_cast<int>(j - 1));
            }
            if (j >= 2 && previous[j - 2] != kNone) {
                consider(previous[j - 2] - 1, static_cast<int>(j - 2));
            }
            if (j >= 3 && previous[j - 3] != kNone) {
                consider(previous[j - 3] - 2, static_cast<int>(j - 3));
            }
            if (farBest != kNone) {
                consider(farBest - kMaxGapPenalty, farIndex);
            }
            if (from < 0) {
                continue;
            }
            const int bonus =
                1 + (isWordStart(name, j) ? kWordStartBonus : 0) + (pattern[i] == name[j] ? kCaseBonus : 0);
            best[i * m + j] = candidate + bonus;
            parent[i * m + j] = from;
        }
    }
    int score = kNone;
    int end = -1;
    for (std::size_t j = 0; j < m; ++j) {
        if (best[(n - 1) * m + j] > score) {
            score = best[(n - 1) * m + j];
            end = static_cast<int>(j);
        }
    }
    if (end < 0) {
        return std::nullopt;
    }
    FuzzyMatch match;
    match.score = score;
    match.positions.resize(n);
    for (std::size_t i = n; i-- > 0;) {
        match.positions[i] = static_cast<std::uint32_t>(end);
        end = parent[i * m + static_cast<std::size_t>(end)];
    }
    if (name == pattern) {
        match.kind = MatchKind::Exact;
    } else if (name.starts_with(pattern)) {
        match.kind = MatchKind::Prefix;
    } else if (startsWithIgnoreCase(name, pattern)) {
        match.kind = MatchKind::PrefixIgnoreCase;
    } else {
        bool wordStarts = true;
        for (std::size_t i = 0; i < n && wordStarts; ++i) {
            const std::uint32_t at = match.positions[i];
            wordStarts = isWordStart(name, at) || (i > 0 && at == match.positions[i - 1] + 1);
        }
        match.kind = wordStarts ? MatchKind::WordStarts : MatchKind::Fuzzy;
    }
    if (match.kind == MatchKind::Exact || match.kind == MatchKind::Prefix ||
        match.kind == MatchKind::PrefixIgnoreCase) {
        for (std::size_t i = 0; i < n; ++i) {
            match.positions[i] = static_cast<std::uint32_t>(i);
        }
    }
    return match;
}

} // namespace frost::lang
