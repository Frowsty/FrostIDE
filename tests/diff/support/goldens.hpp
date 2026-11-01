// SPDX-License-Identifier: Apache-2.0
//
// Readers of the frost::diff golden files (ADR 0019, tests section): the histogram goldens
// (goldens/histogram/*.hunks), the dialect canaries (goldens/histogram/dialect/*.hunks) and the
// hunk-tuple helpers the tests compare with. No gtest.
/*
 * A .hunks file holds A on line 1 and B on line 2, each either a digit string (digit d is the line
 * "L<d>\n"; "-" is the empty sequence) or a double-quoted C-escaped byte string (\n \r \t \\ \"
 * and \xHH). Histogram goldens follow with one "aBegin aEnd bBegin bEnd" line per hunk (0-based,
 * half-open). Dialect goldens follow with a "classic:" line, its hunk lines, a "new:" line and its
 * hunk lines. Blank lines and lines starting with '#' are ignored after line 2.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/line_diff.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frost::diff::test {

// (aBegin, aEnd, bBegin, bEnd).
using HunkTuple = std::array<std::uint32_t, 4>;
using HunkList = std::vector<HunkTuple>;

[[nodiscard]] HunkList hunkTuples(const LineDiff& diff);

// "[(1,2,1,4),(5,5,6,7)]", the notation of ADR 0019.
[[nodiscard]] std::string formatHunks(const HunkList& hunks);

// A digit string or a quoted C-escaped string (see the header comment) as bytes.
[[nodiscard]] Result<std::string> decodeSequenceField(std::string_view field);

// The digit-string text of `digits` ("L<d>\n" per digit).
[[nodiscard]] std::string digitLines(std::string_view digits);

struct HistogramGolden {
    std::string name;
    std::string a;
    std::string b;
    HunkList hunks;
};

struct DialectGolden {
    std::string name;
    std::string a;
    std::string b;
    HunkList classic;
    HunkList fresh; // The "new:" list.
};

[[nodiscard]] Result<HistogramGolden> parseHistogramGolden(std::string name, std::string_view text);
[[nodiscard]] Result<DialectGolden> parseDialectGolden(std::string name, std::string_view text);

// Every *.hunks file directly in `directory`, sorted by name. Errors: those of reading and parsing,
// with the file name.
[[nodiscard]] Result<std::vector<HistogramGolden>> loadHistogramGoldens(std::string_view directory);
[[nodiscard]] Result<std::vector<DialectGolden>> loadDialectGoldens(std::string_view directory);

// tests/diff/<relative> in the source tree (FROST_DIFF_TEST_DATA).
[[nodiscard]] std::string testDataPath(std::string_view relative);

// Whether FROST_DIFF_UPDATE_GOLDENS=1 asks the golden tests to rewrite their files (and fail).
[[nodiscard]] bool updateGoldens();

} // namespace frost::diff::test
