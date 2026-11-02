// SPDX-License-Identifier: Apache-2.0
//
// frost::diff::renderUnified: a LineDiff as a unified diff (POSIX.1-2024 `diff -u`), byte-equal to
// git's -U<k> bodies for the same hunks (ADR 0019, D9).
/*
 * Rules, confirmed against `git diff -U0/-U1/-U3 --inter-hunk-context=0` (git 2.43 and 2.56):
 *   - hunks k and k+1 join into one group when hunk[k+1].a.begin - hunk[k].a.end <= 2 * context;
 *   - a group spans a0 = max(0, first.a.begin - context) to a1 = min(|A|, last.a.end + context),
 *     with b0 = first.b.begin - (first.a.begin - a0) and b1 = last.b.end + (a1 - last.a.end);
 *   - its header is "@@ -R(a0,a1) +R(b0,b1) @@\n", where R(s,e) is "s+1" if e - s == 1, "s,0" if
 *     e == s, else "s+1,e-s";
 *   - lines are written as " ", "-" or "+" plus the line's bytes; a line without a break (only a
 *     side's last line) is followed by "\n\\ No newline at end of file\n".
 * There is no function context: git's funcname text after the second "@@" is TODO(M5).
 *
 * Under LineSplit::Lsp a line may end in a lone CR, which LF-based patch tools do not see as a
 * line end, so they cannot parse such output; patch producers use LineSplit::Lf.
 */
#pragma once

#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"

#include <cstdint>
#include <string>

namespace frost::diff {

struct UnifiedOptions {
    std::uint32_t context = 3; // Unchanged lines around each hunk (git's -U<k>).
    // When both are non-empty and there is at least one hunk, the output starts with
    // "--- <oldHeader>\n+++ <newHeader>\n". CR, LF and backslash are written as \r, \n and \\, so a
    // header always occupies one line. TODO(M2): git's full path quoting (core.quotePath).
    std::string oldHeader;
    std::string newHeader;
};

/*
 * Renders `diff` (a diff of `a` and `b`; precondition: it passes checkDiff's H1 and H2). Empty
 * when the diff has no hunks. O(output). Pure; safe on any thread.
 */
[[nodiscard]] std::string renderUnified(const LineSequence& a, const LineSequence& b, const LineDiff& diff,
                                        const UnifiedOptions& options = {}) noexcept;

} // namespace frost::diff
