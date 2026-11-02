// SPDX-License-Identifier: Apache-2.0
//
// The fuzzy path matcher of Go to File (PLAN.md section 3.2, ADR 0042): scores a candidate path for a
// typed pattern with word-start (CamelHump) and path-segment matching, ranks candidates, and parses
// the "file:line[:column]" suffix that makes the popup jump to a line.
/*
 * Matching (our own scoring; deterministic integer arithmetic). Text is compared code unit by code
 * unit with ASCII case folded. A pattern word matches a text when its characters occur in order;
 * each matched character earns 1, plus 10 at the start of the text, 8 at a word start (after one
 * of ". _ - / \ space", a lower-to-upper case change or a letter-digit change) and 6 when it
 * directly follows the previous matched character; every skipped character after the first match
 * costs 1, and a whole-word equality or prefix earns 50 or 20 more. So "mcpp" matches "main.cpp"
 * (m, then the word "cpp") and "FiSy" matches "FileSystem.cpp".
 *
 * Paths. A pattern without separators is matched against the file name (score doubled), and failing
 * that against the whole path with separators as word starts. A pattern with '/' or '\' is split
 * into segments: the last must match the file name (an empty last segment matches any name), the
 * others must match directory components of the path in order ("src/ma" finds "src/app/main.cpp").
 * Shorter paths win ties, then byte order.
 *
 * Thread safety: PathMatcher is immutable after construction; every function is pure.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::search {

// A Go to File input split into the pattern and an optional 1-based location.
struct LocationQuery {
    std::string pattern;
    std::optional<int> line;
    std::optional<int> column;
    friend bool operator==(const LocationQuery&, const LocationQuery&) = default;
};

// Parses "name:line", "name:line:column" and "name(line)" suffixes (numbers >= 1; a ':' followed by
// anything but digits stays in the pattern, so "C:\x" is a pattern). Surrounding spaces are trimmed.
// O(n).
[[nodiscard]] LocationQuery parseLocationQuery(std::string_view text);

// The word score described in the header comment, or nullopt when `pattern` (already lower-case,
// non-empty) does not occur in `text` in order. O(|pattern| x |text|).
[[nodiscard]] std::optional<int> scoreWord(std::string_view pattern, std::string_view text);

class PathMatcher {
public:
    // `pattern` as typed (case and separators as given); an all-space pattern matches everything
    // with score 0.
    explicit PathMatcher(std::string_view pattern);

    [[nodiscard]] bool empty() const noexcept { return m_segments.empty(); }
    // The score of `path` (relative or absolute, '/' or '\' separated), or nullopt when it does not
    // match. O(|pattern| x |path|).
    [[nodiscard]] std::optional<int> score(std::string_view path) const;

private:
    std::vector<std::string> m_segments; // Lower-case; more than one when the pattern had separators.
};

// One ranked candidate: its index in the input and its score.
struct RankedPath {
    std::size_t index = 0;
    int score = 0;
    friend bool operator==(const RankedPath&, const RankedPath&) = default;
};

// The candidates matching `pattern`, best first (score desc, shorter path, byte order), at most
// `limit` of them (0: all). An empty pattern keeps the input order. O(N x |pattern| x |path| + N log N).
[[nodiscard]] std::vector<RankedPath> rankPaths(std::string_view pattern, std::span<const std::string> paths,
                                                std::size_t limit = 0);

} // namespace frost::search
