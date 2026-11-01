// SPDX-License-Identifier: Apache-2.0
//
// Property checks of frost::diff results shared by the unit tests, the stress tests and the fuzz
// target (ADR 0019, D16), as Result<void> so the libFuzzer harness can use them without gtest: the
// three-applier apply property of a line diff, the apply property of a refinement, and the
// round trip of diff3 conflict markers.
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/diff/merge.hpp"
#include "frost/diff/refine.hpp"

#include <string>
#include <string_view>

namespace frost::diff::test {

// Whether `text` has a CR not followed by LF (a line end under Lsp that LF patch tools do not see).
[[nodiscard]] bool hasLoneCr(std::string_view text) noexcept;

// Whether `text` is well-formed UTF-8 (TextBuffer accepts only that).
[[nodiscard]] bool validUtf8(std::string_view text);

// A printable form of `bytes` for failure messages (C escapes, at most `limit` bytes).
[[nodiscard]] std::string printable(std::string_view bytes, std::size_t limit = 400);

/*
 * The apply property: checkDiff (H1-H4) passes, and three independent appliers turn A into B byte
 * for byte: toEdits through TextBuffer::applyEdits (valid UTF-8 only), the support byte patcher
 * over the hunks, and renderUnified at -U0, -U1 and -U3 through the POSIX unified applier (unless
 * LineSplit::Lsp meets a lone CR, which LF patch tools cannot represent). Errors: Internal naming
 * the applier that disagreed.
 */
[[nodiscard]] Result<void> checkApplies(const LineSequence& a, const LineSequence& b, const LineDiff& diff,
                                        const DiffOptions& options = {});

// The refined pairs of `hunk` are sorted, disjoint, inside the hunk, separated by equal unchanged
// bytes, and turn A's hunk bytes into B's. Errors: Internal.
[[nodiscard]] Result<void> checkRefinement(const LineSequence& a, const LineSequence& b, const Hunk& hunk,
                                           const Refinement& refinement);

// refineHunk's windowed scan (with its edge-only coarse path) gives exactly the result of the full
// scan, detail::RefineScan::FullScan (a white-box check through the library's private engine.hpp).
// Errors: Internal naming both results; the refinement's own errors (Cancelled) are passed on.
[[nodiscard]] Result<void> checkRefineScansAgree(const LineSequence& a, const LineSequence& b, const Hunk& hunk,
                                                 Granularity granularity, const RefineLimits& limits);

/*
 * renderWithMarkers(labels) parses back (parseMarkers at the computed marker size, splitting lines
 * like the model's sequences) into the plain text between conflicts and each Unresolved conflict's
 * three sides, the one documented exception being a side's last line without a break, which
 * comes back with eol() appended. Precondition: under Lsp, no sequence has a lone CR (the
 * documented CR + LF fusion would merge two lines). Errors: Internal.
 */
[[nodiscard]] Result<void> checkMarkersRoundTrip(const MergeModel& model, const MarkerOptions& labels);

} // namespace frost::diff::test
