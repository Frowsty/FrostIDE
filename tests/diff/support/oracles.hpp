// SPDX-License-Identifier: Apache-2.0
//
// Independent oracles for the frost::diff tests (ADR 0019, D16), written without frost::diff's
// code: a naive line splitter, an O(NM) LCS, a byte patcher over hunks, a unified-diff parser and
// applier written from POSIX.1-2024 `diff -u`, and a diff3 conflict-marker parser. No gtest.
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::diff::test {

// The lines of `text` under `split`, each with its break; the empty final line is omitted.
// A character-by-character reading of the rules in line_sequence.hpp.
[[nodiscard]] std::vector<std::string> naiveSplit(std::string_view text, LineSplit split);

// Dense ids for two line lists (equal strings get equal ids).
void naiveIds(const std::vector<std::string>& a, const std::vector<std::string>& b, std::vector<std::uint32_t>& idsA,
              std::vector<std::uint32_t>& idsB);

// The length of a longest common subsequence, by the O(NM) dynamic programme.
[[nodiscard]] std::uint64_t lcsLength(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b);

// The insert/delete distance N + M - 2 * LCS (not Levenshtein): the size of a minimal script.
[[nodiscard]] std::uint64_t editDistance(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b);

// Rebuilds B from A's lines, B's lines and the hunks: A's lines outside hunks, B's inside.
// Errors: InvalidArgument for hunks out of order or out of bounds.
[[nodiscard]] Result<std::string> applyHunks(const std::vector<std::string>& linesA,
                                             const std::vector<std::string>& linesB, std::span<const Hunk> hunks);

/*
 * Applies a unified diff (one file: optional "---"/"+++" headers, then "@@ -l,s +l,s @@" hunks with
 * ' ', '-' and '+' lines and "\ No newline at end of file" markers) to `original`, read as LF lines.
 * Context and deleted lines must match the original exactly. Errors: InvalidArgument naming the
 * problem (a malformed header, a mismatch, overlapping hunks).
 */
[[nodiscard]] Result<std::string> applyUnified(std::string_view original, std::string_view patch);

// One parsed region of a text with diff3 conflict markers.
struct MarkedRegion {
    bool conflict = false;
    std::string text; // Plain text (conflict == false).
    std::string ours; // The three sections of a conflict, each with its lines' breaks.
    std::string base;
    std::string theirs;
    std::string oursLabel; // The text after "<<<<<<< " (without the line break), and so on.
    std::string baseLabel;
    std::string theirsLabel;
};

/*
 * Parses `text` with conflict markers of exactly `size` marker characters (a line starting with
 * size '<' and then a space or a line break opens a conflict; '|', '=' and '>' lines of the same
 * size continue it). Lines end as `split` says: at LF only, or at LF, CR and CRLF (every marker
 * is followed by one). Errors: InvalidArgument for an unterminated or out-of-order conflict.
 */
[[nodiscard]] Result<std::vector<MarkedRegion>> parseMarkers(std::string_view text, std::uint32_t size,
                                                             LineSplit split = LineSplit::Lsp);

} // namespace frost::diff::test
