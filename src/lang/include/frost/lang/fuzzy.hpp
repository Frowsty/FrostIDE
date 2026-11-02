// SPDX-License-Identifier: Apache-2.0
//
// The fuzzy matcher of completion, file structure and go to symbol (W2, ADR 0041): Rider-like
// "camel hump" matching of a typed pattern against a name, with a score and the matched bytes
// for highlighting. FrostIDE's own algorithm (no matcher source was consulted).
/*
 * Matching. The pattern's characters must appear in the name in order, compared ASCII
 * case-insensitively (other bytes exactly), and the first one must fall on a word start: the
 * name's first byte, a letter after a non-letter or non-digit ('_', ':', '.', ...), an upper-case
 * letter after a lower-case one ("getFileName": g, F, N), or a digit after a letter. So "gfn"
 * and "gFN" match getFileName and "fn" matches file_name, but "etf" matches nothing.
 *
 * Score (higher is better): a dynamic programme over (pattern, name) positions picks the
 * alignment with the best sum of: +1 per matched character, +8 when it lands on a word start,
 * +5 when it continues the previous match, +1 when the case matches exactly, and -1 per name
 * byte skipped between matches (at most -3 per gap). Ties prefer the earliest alignment.
 *
 * Kind (the first sort key of completion, so prefix matches always come first):
 *   Exact (case-sensitive equality), Prefix (case-sensitive), PrefixIgnoreCase, WordStarts
 *   (every matched character is a word start or continues a run that began on one), Fuzzy.
 *
 * Cost: O(pattern x name) time and memory; names over 512 bytes are matched on their first 512.
 * Pure and thread-safe.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace frost::lang {

enum class MatchKind : std::uint8_t { Exact, Prefix, PrefixIgnoreCase, WordStarts, Fuzzy };

struct FuzzyMatch {
    MatchKind kind = MatchKind::Fuzzy;
    int score = 0;
    std::vector<std::uint32_t> positions; // Byte offsets in the name, one per pattern byte, ascending.
};

// The match of `pattern` in `name`, or nullopt. An empty pattern matches everything (Prefix,
// score 0, no positions).
[[nodiscard]] std::optional<FuzzyMatch> fuzzyMatch(std::string_view pattern, std::string_view name);

// True when byte `index` of `name` starts a word (see the header comment).
[[nodiscard]] bool isWordStart(std::string_view name, std::size_t index) noexcept;

} // namespace frost::lang
