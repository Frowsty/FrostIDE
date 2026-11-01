// SPDX-License-Identifier: Apache-2.0
//
// frost::diff::diffLines: the line diff of two LineSequences (ADR 0019, D3-D7). The default is
// the histogram algorithm with git's classic slider compaction, which reproduces
// `git diff --histogram --no-indent-heuristic` of git before 2.54 hunk for hunk; Myers (minimal)
// serves as the histogram's fallback and as an option. Also toEdits (a diff as frost::text edits)
// and checkDiff (the invariants, for tests and fuzzing).
/*
 * A diff is a list of hunks: A's lines [hunk.a) are replaced by B's lines [hunk.b). Every line
 * outside the hunks is unchanged and pairs, in order, with an unchanged line of the other side
 * whose bytes are equal.
 *
 * Work cap (ADR 0019, D7). Every phase (interning, histogram, Myers, compaction) charges one
 * budget of DiffLimits::maxWork units. When it runs out the call still succeeds: the regions not
 * yet diffed become whole-region replaces, the result is a valid (coarser) diff, and
 * LineDiff::degraded is set. Results are deterministic for a given input, options and budget.
 * The stop token is checked at entry and every 4096 work units.
 *
 * Git parity (ADR 0019, D6). Sliders::GitClassic reproduces git < 2.54 exactly on inputs that do
 * not reach the histogram's Myers fallback, and in the measured fallback cases. git 2.54 changed
 * the final clean-up of histogram output; matching it is TODO(M5).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/text/text_buffer.hpp"

#include <cstdint>
#include <stop_token>
#include <vector>

namespace frost::diff {

// One change: A's lines [a) are replaced by B's lines [b). At least one side is non-empty.
struct Hunk {
    LineRange a;
    LineRange b;

    friend constexpr bool operator==(const Hunk&, const Hunk&) noexcept = default;
};

enum class Algorithm : std::uint8_t {
    Histogram, // JGit's histogram algorithm, as git runs it (no prefix/suffix trimming); the default.
    Myers,     // A minimal script over the whole input (Myers 1986, linear space); for tests and callers
               // who want minimal results.
};

enum class Sliders : std::uint8_t {
    GitClassic, // git's classic compaction of changed-line groups (one pass per side; ADR 0019, D5).
    None,       // The raw algorithm output, for tests.
};

/*
 * The default work cap: 2^22 units, sized so that one call stays within B21's 50 ms on REF-W1 on
 * any input within the line limits: the slowest pathological case (P1-P7 at 100k lines) and a
 * kDefaultMaxLines-line pair, whose interning alone the cap stops (DiffPerf.MaxLinesBounded).
 * What it leaves for ordinary inputs (DefaultCap.*): a 10k-line file with scattered edits needs a
 * few percent of the cap, and with every 10th line edited 0.42x; a 200k-line file with 10 edits
 * 0.61x, and a 150k-line merge with 5 edits per side 0.60x. Edits at evenly spaced lines cost the
 * histogram about O(N + D^2) and still degrade when very dense (every other line of a 10k-line
 * file). Callers off the UI thread that prefer precision to latency may pass a larger maxWork.
 * Exact numbers: ADR 0019, D15 and Measurements; re-measure with DiffPerf.WorkCapSweep.
 * Budget: B21.
 */
inline constexpr std::uint64_t kDefaultMaxWork = std::uint64_t{1} << 22;

struct DiffLimits {
    // Work units per call (see the header comment). 0 makes every non-trivial diff one whole replace.
    std::uint64_t maxWork = kDefaultMaxWork;
    // Lines per side; larger inputs fail with OutOfRange.
    std::uint32_t maxLines = kDefaultMaxLines;
};

struct DiffOptions {
    Algorithm algorithm = Algorithm::Histogram;
    Sliders sliders = Sliders::GitClassic;
    DiffLimits limits{};
};

// The result of diffLines. Plain value.
struct LineDiff {
    std::vector<Hunk> hunks;     // Sorted and maximal: consecutive hunks are separated by >= 1 unchanged line.
    bool degraded = false;       // The work cap was reached; the hunks are valid but coarser than normal.
    std::uint32_t fallbacks = 0; // Histogram regions handed to Myers (more than 64 repeats of every line).
    std::uint64_t work = 0;      // Work units spent.

    // Lines of A inside hunks. O(hunks).
    [[nodiscard]] std::uint64_t deletedLines() const noexcept;
    // Lines of B inside hunks. O(hunks).
    [[nodiscard]] std::uint64_t insertedLines() const noexcept;
};

/*
 * Diffs `a` against `b`. Precondition: a.split() == b.split().
 *
 * Errors, in this order: OutOfRange if either side has more than options.limits.maxLines lines
 * (O(1)); Cancelled if `stop` has been requested at entry or is requested during the call (polled
 * every 4096 work units). Reaching maxWork is not an error (see the header comment). Byte-equal
 * inputs give no hunks; exactly one empty side gives one hunk; neither costs work.
 *
 * Complexity: O(N + M) to intern (each line and probe step counts as work); the histogram is
 * typically O((N + M) * D) (JGit's analysis); Myers is O((N + M) * D) time and O(N + M) space;
 * compaction is O(N + M + slide steps). Every phase is bounded by maxWork; outside the budget
 * the call does only the O(1) limit checks, a byte comparison of the two texts (memcmp speed) and
 * O(N + M) flag and hunk passes. Pure and reentrant: any number of threads may diff shared
 * sequences concurrently.
 * Budget: B18 (openDiff visible, gated in M5). Budget: B21 (bounded by maxWork; measured by
 * DiffPerf.Pathological and DiffPerf.MaxLinesBounded).
 */
[[nodiscard]] Result<LineDiff> diffLines(const LineSequence& a, const LineSequence& b, const DiffOptions& options = {},
                                         std::stop_token stop = {}) noexcept;

/*
 * The diff as frost::text edits: one Edit per hunk, in a's byte coordinates, whose text is a view
 * into b.bytes(). The edits are sorted and non-adjacent (hunks are separated by unchanged lines),
 * ready for TextBuffer::applyEdits. When a and b are valid UTF-8, every bound is a char boundary
 * and every text is well-formed, because line boundaries follow an ASCII break byte.
 *
 * Lifetime: the views are valid while any copy of `b` lives. Precondition (the caller's duty):
 * apply the edits only to a buffer whose text is still a's bytes; for example build `a` from
 * TextBuffer::snapshot() at version v and apply only while TextBuffer::version() == v.
 * O(hunks). Budget: B18.
 */
[[nodiscard]] std::vector<text::Edit> toEdits(const LineSequence& a, const LineSequence& b,
                                              const LineDiff& diff) noexcept;

/*
 * Checks a diff of `a` and `b` and returns ErrorCode::Internal naming the first violation:
 *   H1 hunks are sorted, each has a non-empty side, and each lies within bounds;
 *   H2 hunk k's a.begin minus the previous a.end equals its b.begin minus the previous b.end, and
 *      is >= 1 for k > 0; the tail runs after the last hunk are equal too;
 *   H3 every unchanged pair of lines is byte-equal;
 *   H4 (a consistency check, not an independent oracle) with Sliders::GitClassic and !degraded,
 *      re-running the side-B slider pass on a copy of the final change flags changes nothing.
 *      The side-A pass is not re-run: it is not a fixed point (ADR 0019, D5).
 * O(N + M + slide steps); safe on any thread. For tests and fuzzing.
 */
[[nodiscard]] Result<void> checkDiff(const LineSequence& a, const LineSequence& b, const LineDiff& diff,
                                     const DiffOptions& options = {}) noexcept;

} // namespace frost::diff
