// SPDX-License-Identifier: Apache-2.0
//
// Performance tests of frost::diff (label `perf`; ADR 0019, D15 and Measurements). The perf label
// runs in every -Quick, so Debug and sanitizer builds use a tenth of the scale and only report;
// optimised unsanitised builds run the full scale and assert the derived sub-budgets.
/*
 * DiffPerf.Typical: a 10k-line code-like file with 1-100 random edits, 200 samples. Timed: two
 * LineSequence builds, diffLines and renderUnified. p99 <= 5 ms (1/20 of B18's 100 ms).
 * MergePerf.Typical: the same size, three sides. Timed: three LineSequence builds and
 * MergeModel::create. p99 <= 10 ms.
 * DiffPerf.LargeNotDegraded: a 100k-line code-like file with 1000 edits. Timed: diffLines. Must not
 * be degraded and must take <= 50 ms (B21).
 * DiffPerf.Pathological: P1-P7 (100k lines; P6 and P7 one 8 MiB line) with default limits. Timed:
 * diffLines, plus refineHunk for P6 (precise) and P7 (coarse). Each <= 50 ms; degraded results are
 * allowed (expected for P1, P2 and P5).
 * MergePerf.Pathological: merge3 over P2 and P5 triples <= 50 ms (the halved per-diff budgets).
 * DiffPerf.WorkUnitCost: ns per work unit per phase (interning, histogram, Myers, compaction) in
 * each P case.
 * DiffPerf.WorkCapSweep: the tuning rule of ADR 0019, D15, measured: the slowest P case at caps
 * 2^18 .. 2^25, LargeNotDegraded's work, and the largest cap meeting both conditions.
 * DiffPerf.PeakMemory: peak bytes allocated by one diffLines call at kDefaultMaxLines lines per
 * side (counted by replacing the global operator new; unsanitised builds only), and per phase.
 * The call and its interning phase stay within 60 bytes per line the budget can pay for.
 * DiffPerf.MaxLinesBounded: kDefaultMaxLines unique lines per side, every 1000th edited, under the
 * default cap: interning is charged kInternUnitsPerLine per line, so the cap bounds this call
 * too (<= 50 ms; degraded).
 * DiffPerf.EvenlySpaced and MergePerf.EvenlySpaced: edits at evenly spaced lines, the plain
 * histogram's O(N * D) shape, which its reuse across regions makes cheap (ADR 0019, D3, D15 and
 * Q4). Each call <= 50 ms under the default cap; the work they would need and whether they
 * degrade are printed, and the uncapped merge must be clean.
 * DiffPerf.CapScale and MergePerf.CapScale: the share of the cap that interning takes from large
 * files, and what their results look like under Q4's caps (full scale only, no timing; ADR 0019,
 * D7 and Q4).
 * RefinePerf.EdgesOnly: one 15 MiB line differing in its first and last bytes; refineHunk only
 * tokenises the middle's edges (ADR 0019, D8), <= 50 ms per granularity.
 * RefinePerf.ContinuationRuns: Char refinement of lines whose common prefix and suffix are 7 MiB
 * runs of UTF-8 continuation bytes (one token per byte), precise and coarse; the window finds its
 * edges without tokenising the runs (ADR 0019, D8), <= 50 ms each.
 *
 * Statistic: the timed cases assert on the best of their runs (3 or 5), the time the code takes
 * on REF-W1 when nothing else competes for the core; the worst run is printed beside it. Several
 * builds share this machine during verification, and a scheduler stall in one run says nothing
 * about the code (ADR 0019, D15). The Typical cases assert a p99 over their samples instead.
 * Budget: B18. Budget: B21.
 */

#include "frost/diff/merge.hpp"
#include "frost/diff/refine.hpp"
#include "frost/diff/unified.hpp"

#include "compaction.hpp"
#include "diff/diff_test_util.hpp"
#include "diff/support/generators.hpp"
#include "histogram.hpp"
#include "interner.hpp"
#include "myers.hpp"
#include "work_budget.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <new>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FROST_DIFF_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define FROST_DIFF_SANITIZED 1
#endif
#endif

namespace {

// The allocation counter behind DiffPerf.PeakMemory: live and peak bytes while enabled.
std::atomic<bool> gCounting{false};
std::atomic<std::int64_t> gLive{0};
std::atomic<std::int64_t> gPeak{0};

} // namespace

#ifndef FROST_DIFF_SANITIZED
// A size header in front of each block lets delete subtract what new added. 16 bytes keeps the
// default new alignment.
void* operator new(std::size_t size) {
    void* block = std::malloc(size + 16);
    if (block == nullptr) {
        std::abort(); // FrostIDE terminates on allocation failure.
    }
    *static_cast<std::size_t*>(block) = size;
    if (gCounting.load(std::memory_order_relaxed)) {
        const std::int64_t live = gLive.fetch_add(static_cast<std::int64_t>(size)) + static_cast<std::int64_t>(size);
        std::int64_t peak = gPeak.load();
        while (live > peak && !gPeak.compare_exchange_weak(peak, live)) {
        }
    }
    return static_cast<char*>(block) + 16;
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }
    void* block = static_cast<char*>(pointer) - 16;
    if (gCounting.load(std::memory_order_relaxed)) {
        gLive.fetch_sub(static_cast<std::int64_t>(*static_cast<std::size_t*>(block)));
    }
    std::free(block);
}
void operator delete[](void* pointer) noexcept { operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { operator delete(pointer); }
#endif

namespace frost::diff::test {
namespace {

using Clock = std::chrono::steady_clock;
using frost::test::kAssertBudgets;

constexpr bool kFullScale = kAssertBudgets;
constexpr std::uint32_t kTypicalLines = kFullScale ? 10'000 : 1'000;
constexpr std::uint32_t kLargeLines = kFullScale ? 100'000 : 10'000;
constexpr std::uint32_t kPathologicalLines = kFullScale ? 100'000 : 10'000;
constexpr std::uint32_t kLongLineKiB = kFullScale ? 8192 : 819;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double percentile(std::vector<double> samples, double q) {
    std::sort(samples.begin(), samples.end());
    return samples[static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1))];
}

const char* mode() { return kAssertBudgets ? "budgeted" : "report-only"; }

// The best and worst of a few timed runs (see the header comment's statistic).
struct Runs {
    double best = std::numeric_limits<double>::max();
    double worst = 0;

    void add(double ms) noexcept {
        best = std::min(best, ms);
        worst = std::max(worst, ms);
    }
};

// `lines` unique lines "line <i>", and a copy in which every `every`-th line (from line every / 2)
// reads "edit <i>" instead: evenly spaced replaces.
TextPair evenlySpaced(std::uint32_t lines, std::uint32_t every) {
    TextPair pair;
    pair.a.reserve(static_cast<std::size_t>(lines) * 14);
    pair.b.reserve(static_cast<std::size_t>(lines) * 14);
    for (std::uint32_t i = 0; i < lines; ++i) {
        const std::string line = std::format("line {}\n", i);
        pair.a += line;
        pair.b += i % every == every / 2 ? std::format("edit {}\n", i) : line;
    }
    return pair;
}

TEST(DiffPerf, Typical) {
    SplitMix64 rng(0xD1FF0060);
    std::vector<double> samples;
    const std::uint32_t count = kFullScale ? 200 : 40;
    for (std::uint32_t s = 0; s < count; ++s) {
        TextPair pair = codeLikePair(rng, kTypicalLines, static_cast<std::uint32_t>(1 + rng.below(100)));
        const auto start = Clock::now();
        auto a = LineSequence::fromBytes(std::move(pair.a));
        auto b = LineSequence::fromBytes(std::move(pair.b));
        ASSERT_TRUE(a && b);
        auto diff = diffLines(*a, *b);
        ASSERT_TRUE(diff.has_value());
        const std::string unified = renderUnified(*a, *b, *diff);
        samples.push_back(msSince(start));
        ASSERT_FALSE(diff->degraded);
        ASSERT_FALSE(unified.empty() && !diff->hunks.empty());
    }
    const double p50 = percentile(samples, 0.5);
    const double p99 = percentile(samples, 0.99);
    std::printf("[ perf ] DiffPerf.Typical (%s): %u lines, p50 %.3f ms, p99 %.3f ms (budget p99 5 ms)\n", mode(),
                kTypicalLines, p50, p99);
    if (kAssertBudgets) {
        EXPECT_LE(p99, 5.0);
    }
}

TEST(MergePerf, Typical) {
    SplitMix64 rng(0xD1FF0061);
    std::vector<double> samples;
    const std::uint32_t count = kFullScale ? 200 : 40;
    for (std::uint32_t s = 0; s < count; ++s) {
        TextPair first = codeLikePair(rng, kTypicalLines, static_cast<std::uint32_t>(1 + rng.below(100)));
        // Theirs edits the same base independently: inserted lines after random lines.
        std::string theirs = first.a;
        for (std::uint64_t e = 1 + rng.below(50); e > 0; --e) {
            const std::size_t at = theirs.find('\n', rng.below(theirs.size()));
            if (at != std::string::npos) {
                theirs.insert(at + 1, std::format("    theirs {};\n", e));
            }
        }
        const auto start = Clock::now();
        auto base = LineSequence::fromBytes(std::move(first.a));
        auto ours = LineSequence::fromBytes(std::move(first.b));
        auto mine = LineSequence::fromBytes(std::move(theirs));
        ASSERT_TRUE(base && ours && mine);
        auto model = MergeModel::create(std::move(*base), std::move(*ours), std::move(*mine));
        samples.push_back(msSince(start));
        ASSERT_TRUE(model.has_value());
        ASSERT_FALSE(model->merge().degraded);
    }
    const double p50 = percentile(samples, 0.5);
    const double p99 = percentile(samples, 0.99);
    std::printf("[ perf ] MergePerf.Typical (%s): %u lines, p50 %.3f ms, p99 %.3f ms (budget p99 10 ms)\n", mode(),
                kTypicalLines, p50, p99);
    if (kAssertBudgets) {
        EXPECT_LE(p99, 10.0);
    }
}

TEST(DiffPerf, LargeNotDegraded) {
    SplitMix64 rng(0xD1FF0062);
    TextPair pair = codeLikePair(rng, kLargeLines, kLargeLines / 100);
    const LineSequence a = makeSequence(std::move(pair.a));
    const LineSequence b = makeSequence(std::move(pair.b));
    Runs runs;
    LineDiff last;
    for (int run = 0; run < 5; ++run) {
        const auto start = Clock::now();
        auto diff = diffLines(a, b);
        runs.add(msSince(start));
        ASSERT_TRUE(diff.has_value());
        last = std::move(*diff);
    }
    std::printf("[ perf ] DiffPerf.LargeNotDegraded (%s): %u lines, %zu hunks, work %llu of %llu (headroom %.2fx), "
                "%s, best %.3f ms, worst %.3f ms (budget 50 ms)\n",
                mode(), kLargeLines, last.hunks.size(), static_cast<unsigned long long>(last.work),
                static_cast<unsigned long long>(kDefaultMaxWork),
                static_cast<double>(kDefaultMaxWork) / static_cast<double>(std::max<std::uint64_t>(last.work, 1)),
                last.degraded ? "DEGRADED" : "not degraded", runs.best, runs.worst);
    const double best = runs.best;
    EXPECT_FALSE(last.degraded);
    if (kAssertBudgets) {
        EXPECT_LE(best, 50.0);
    }
}

std::uint32_t sizeFor(Pathology kind) {
    return kind == Pathology::P6 || kind == Pathology::P7 ? kLongLineKiB : kPathologicalLines;
}

TEST(DiffPerf, Pathological) {
    SplitMix64 rng(0xD1FF0063);
    for (const Pathology kind : kAllPathologies) {
        TextPair pair = pathologicalPair(kind, rng, sizeFor(kind));
        const LineSequence a = makeSequence(std::move(pair.a));
        const LineSequence b = makeSequence(std::move(pair.b));
        Runs runs;
        LineDiff diff;
        Refinement refinement;
        for (int run = 0; run < 3; ++run) {
            const auto start = Clock::now();
            auto result = diffLines(a, b);
            ASSERT_TRUE(result.has_value());
            if (kind == Pathology::P6 || kind == Pathology::P7) {
                ASSERT_EQ(result->hunks.size(), 1u);
                auto refined = refineHunk(a, b, result->hunks.front(), Granularity::Word);
                ASSERT_TRUE(refined.has_value());
                refinement = std::move(*refined);
            }
            runs.add(msSince(start));
            diff = std::move(*result);
        }
        const double best = runs.best;
        std::printf("[ perf ] DiffPerf.Pathological %s (%s): %u, %zu hunks, work %llu, %s%s, best %.3f ms, worst "
                    "%.3f ms (budget 50 ms)\n",
                    std::string(pathologyName(kind)).c_str(), mode(), sizeFor(kind), diff.hunks.size(),
                    static_cast<unsigned long long>(diff.work), diff.degraded ? "degraded" : "exact",
                    kind == Pathology::P6 || kind == Pathology::P7
                        ? (refinement.coarse ? ", refine coarse" : ", refine precise")
                        : "",
                    best, runs.worst);
        EXPECT_TRUE(appliesCleanly(a, b, diff));
        if (kind == Pathology::P6) {
            EXPECT_FALSE(refinement.coarse);
        }
        if (kind == Pathology::P7) {
            EXPECT_TRUE(refinement.coarse);
        }
        if (kAssertBudgets) {
            EXPECT_LE(best, 50.0) << pathologyName(kind);
        }
    }
}

TEST(MergePerf, Pathological) {
    SplitMix64 rng(0xD1FF0064);
    for (const Pathology kind : {Pathology::P2, Pathology::P5}) {
        TextPair first = pathologicalPair(kind, rng, kPathologicalLines);
        TextPair second = pathologicalPair(kind, rng, kPathologicalLines);
        const LineSequence base = makeSequence(std::move(first.a));
        const LineSequence ours = makeSequence(std::move(first.b));
        const LineSequence theirs = makeSequence(std::move(second.b));
        Runs runs;
        Merge3 merge;
        for (int run = 0; run < 3; ++run) {
            const auto start = Clock::now();
            auto result = merge3(base, ours, theirs);
            runs.add(msSince(start));
            ASSERT_TRUE(result.has_value());
            merge = std::move(*result);
        }
        const double best = runs.best;
        std::printf("[ perf ] MergePerf.Pathological %s (%s): %zu chunks, %u conflicts, work %llu, %s, best %.3f ms, "
                    "worst %.3f ms (budget 50 ms)\n",
                    std::string(pathologyName(kind)).c_str(), mode(), merge.chunks.size(), merge.conflicts,
                    static_cast<unsigned long long>(merge.work), merge.degraded ? "degraded" : "exact", best,
                    runs.worst);
        if (kAssertBudgets) {
            EXPECT_LE(best, 50.0) << pathologyName(kind);
        }
    }
}

TEST(DiffPerf, MaxLinesBounded) {
    const std::uint32_t lines = kFullScale ? kDefaultMaxLines : kDefaultMaxLines / 16;
    TextPair pair = evenlySpaced(lines, 1000);
    const LineSequence a = makeSequence(std::move(pair.a));
    const LineSequence b = makeSequence(std::move(pair.b));
    ASSERT_EQ(a.size(), lines);
    Runs runs;
    LineDiff last;
    for (int run = 0; run < 3; ++run) {
        const auto start = Clock::now();
        auto diff = diffLines(a, b);
        runs.add(msSince(start));
        ASSERT_TRUE(diff.has_value());
        last = std::move(*diff);
    }
    std::printf("[ perf ] DiffPerf.MaxLinesBounded (%s): %u lines per side, %zu hunks, work %llu, %s, best %.3f ms, "
                "worst %.3f ms (budget 50 ms)\n",
                mode(), lines, last.hunks.size(), static_cast<unsigned long long>(last.work),
                last.degraded ? "degraded" : "exact", runs.best, runs.worst);
    EXPECT_TRUE(appliesCleanly(a, b, last));
    EXPECT_LE(last.work, kDefaultMaxWork);
    if (kFullScale) {
        // 2 * 2^21 lines at kInternUnitsPerLine units each are far beyond the cap.
        EXPECT_TRUE(last.degraded);
    }
    if (kAssertBudgets) {
        EXPECT_LE(runs.best, 50.0);
    }
}

TEST(DiffPerf, EvenlySpaced) {
    struct Shape {
        std::uint32_t lines;
        std::uint32_t every;
    };
    // Full scale; Debug and sanitizer builds use a tenth of the lines.
    constexpr Shape kShapes[] = {{10'000, 10}, {100'000, 1000}, {100'000, 2000}, {50'000, 500}};
    for (const Shape shape : kShapes) {
        const std::uint32_t lines = kFullScale ? shape.lines : shape.lines / 10;
        TextPair pair = evenlySpaced(lines, shape.every);
        const LineSequence a = makeSequence(std::move(pair.a));
        const LineSequence b = makeSequence(std::move(pair.b));
        DiffOptions unlimited;
        unlimited.limits.maxWork = std::numeric_limits<std::uint64_t>::max();
        auto needed = diffLines(a, b, unlimited);
        ASSERT_TRUE(needed.has_value());
        ASSERT_FALSE(needed->degraded);
        EXPECT_EQ(needed->hunks.size(), (lines + shape.every / 2) / shape.every);
        Runs runs;
        LineDiff last;
        for (int run = 0; run < 3; ++run) {
            const auto start = Clock::now();
            auto diff = diffLines(a, b);
            runs.add(msSince(start));
            ASSERT_TRUE(diff.has_value());
            last = std::move(*diff);
        }
        std::printf("[ perf ] DiffPerf.EvenlySpaced (%s): %u lines, every %uth edited: needs %llu units (%.2fx the "
                    "cap); under the cap %zu of %zu hunks, %s, best %.3f ms, worst %.3f ms (budget 50 ms)\n",
                    mode(), lines, shape.every, static_cast<unsigned long long>(needed->work),
                    static_cast<double>(needed->work) / static_cast<double>(kDefaultMaxWork), last.hunks.size(),
                    needed->hunks.size(), last.degraded ? "DEGRADED" : "exact", runs.best, runs.worst);
        EXPECT_TRUE(appliesCleanly(a, b, last));
        if (!last.degraded) {
            EXPECT_EQ(last.hunks, needed->hunks);
        }
        if (kAssertBudgets) {
            EXPECT_LE(runs.best, 50.0) << lines << " lines, every " << shape.every;
        }
    }
}

TEST(MergePerf, EvenlySpaced) {
    // Base: unique lines. Ours replaces every 40th line from line 0, theirs every 40th from line
    // 20: disjoint edits, so the true merge is clean.
    const std::uint32_t lines = kFullScale ? 10'000 : 1'000;
    std::string base;
    std::string ours;
    std::string theirs;
    for (std::uint32_t i = 0; i < lines; ++i) {
        const std::string line = std::format("line {}\n", i);
        base += line;
        ours += i % 40 == 0 ? std::format("ours {}\n", i) : line;
        theirs += i % 40 == 20 ? std::format("theirs {}\n", i) : line;
    }
    const LineSequence sb = makeSequence(std::move(base));
    const LineSequence so = makeSequence(std::move(ours));
    const LineSequence st = makeSequence(std::move(theirs));
    MergeOptions unlimited;
    unlimited.diff.limits.maxWork = std::numeric_limits<std::uint64_t>::max();
    auto needed = merge3(sb, so, st, unlimited);
    ASSERT_TRUE(needed.has_value());
    ASSERT_FALSE(needed->degraded);
    EXPECT_EQ(needed->conflicts, 0u);
    Runs runs;
    Merge3 last;
    for (int run = 0; run < 3; ++run) {
        const auto start = Clock::now();
        auto merge = merge3(sb, so, st);
        runs.add(msSince(start));
        ASSERT_TRUE(merge.has_value());
        last = std::move(*merge);
    }
    std::printf("[ perf ] MergePerf.EvenlySpaced (%s): %u lines, %u disjoint edits per side: needs %llu units (%.2fx "
                "the cap); under the cap %u conflicts, %s, best %.3f ms, worst %.3f ms (budget 50 ms)\n",
                mode(), lines, lines / 40, static_cast<unsigned long long>(needed->work),
                static_cast<double>(needed->work) / static_cast<double>(kDefaultMaxWork), last.conflicts,
                last.degraded ? "DEGRADED" : "exact", runs.best, runs.worst);
    EXPECT_TRUE(checkMerge(sb, so, st, last).has_value());
    if (!last.degraded) {
        EXPECT_EQ(last.conflicts, 0u);
    }
    if (kAssertBudgets) {
        EXPECT_LE(runs.best, 50.0);
    }
}

// Unique statement lines (the shape of the second review's probe), with the line replaced by
// "    // <tag> <i>" wherever period != 0 and i % period == phase.
std::string statements(std::uint32_t lines, std::uint32_t period, std::uint32_t phase, std::string_view tag) {
    std::string text;
    text.reserve(static_cast<std::size_t>(lines) * 40);
    for (std::uint32_t i = 0; i < lines; ++i) {
        text += period != 0 && i % period == phase
                    ? std::format("    // {} {}\n", tag, i)
                    : std::format("    int value{} = compute({}, {});\n", i, i * 7 % 1000, i % 13);
    }
    return text;
}

// The units interning `sequences` takes (uncapped), with or without the partner path.
std::uint64_t internWork(std::initializer_list<const LineSequence*> sequences,
                         detail::PartnerPath partners = detail::PartnerPath::On) {
    const std::vector<const LineSequence*> list(sequences);
    std::vector<std::vector<std::uint32_t>> ids(list.size());
    detail::WorkBudget budget(std::numeric_limits<std::uint64_t>::max(), {});
    (void)detail::internSequences(list, ids, budget, detail::LineHashes::Fnv, nullptr, partners);
    return budget.used();
}

// The caps of Q4: the default 2^22 (option a), 2^23 (option c) and 2^25 (option b's second cap).
constexpr std::uint64_t kQ4Caps[] = {kDefaultMaxWork, kDefaultMaxWork * 2, kDefaultMaxWork * 8};

std::string capName(std::uint64_t cap) { return std::format("2^{}", std::countr_zero(cap)); }

bool wholeFile(const Hunk& hunk, std::uint32_t linesA, std::uint32_t linesB) {
    return hunk.a == LineRange{LineIndex{0}, LineIndex{linesA}} && hunk.b == LineRange{LineIndex{0}, LineIndex{linesB}};
}

/*
 * What the cap leaves for large files (ADR 0019, D7 and Q4). Interning comes first: about 9 units
 * per line pair of a lightly edited diff and 10 per line triple of a merge with the partner path
 * (16 and 24 without it). Unique statement lines with 10 replaced lines (DiffPerf.CapScale) or 5
 * disjoint replaced lines per side (MergePerf.CapScale), from 100k (50k) lines per side up to the
 * line limit, under each of Q4's caps: the work each size needs, interning's share, and what the
 * result looks like (exact; k of the 10 hunks; one whole-file hunk or conflict). Full scale only,
 * no timing. Asserted: a result degrades exactly when the work it needs exceeds the cap, interning
 * beyond the cap gives one whole-file hunk or conflict, and the sizes DefaultCap.MidSizeFilesStayExact
 * checks in every build (200k-line diffs, 150k-line merges) stay exact under the default cap.
 */
TEST(DiffPerf, CapScale) {
    if (!kFullScale) {
        std::printf("[ perf ] DiffPerf.CapScale: full scale only (optimised, unsanitised builds)\n");
        return;
    }
    constexpr std::uint32_t kSizes[] = {100'000, 150'000, 200'000,   250'000,   262'144,
                                        300'000, 500'000, 1'000'000, 1'500'000, kDefaultMaxLines};
    for (const std::uint32_t lines : kSizes) {
        const LineSequence a = makeSequence(statements(lines, 0, 0, ""));
        const LineSequence b = makeSequence(statements(lines, lines / 10, lines / 23, "edited"));
        DiffOptions unlimited;
        unlimited.limits.maxWork = std::numeric_limits<std::uint64_t>::max();
        auto needed = diffLines(a, b, unlimited);
        ASSERT_TRUE(needed.has_value());
        ASSERT_EQ(needed->hunks.size(), 10u);
        const std::uint64_t interning = internWork({&a, &b});
        const std::uint64_t tableOnly = internWork({&a, &b}, detail::PartnerPath::Off);
        std::string shapes;
        for (const std::uint64_t cap : kQ4Caps) {
            DiffOptions options;
            options.limits.maxWork = cap;
            auto diff = diffLines(a, b, options);
            ASSERT_TRUE(diff.has_value());
            EXPECT_TRUE(appliesCleanly(a, b, *diff, options));
            EXPECT_EQ(diff->degraded, needed->work > cap) << lines << " lines per side, cap " << cap;
            std::string shape = "exact";
            if (diff->degraded) {
                shape = diff->hunks.size() == 1 && wholeFile(diff->hunks[0], lines, lines)
                            ? std::string("one whole-file hunk")
                            : std::format("{} of 10 hunks", diff->hunks.size());
            }
            shapes += std::format("; {} {}", capName(cap), shape);
            if (cap != kDefaultMaxWork) {
                continue;
            }
            // ADR 0019, D7 and Q4.
            if (lines <= 200'000) {
                EXPECT_FALSE(diff->degraded) << lines << " lines per side";
            }
            if (interning > cap) {
                ASSERT_EQ(diff->hunks.size(), 1u) << lines << " lines per side";
                EXPECT_TRUE(wholeFile(diff->hunks[0], lines, lines)) << lines << " lines per side";
            }
        }
        std::printf("[ perf ] DiffPerf.CapScale: %u lines per side, 10 edits: needs %llu units (%.2fx 2^22), "
                    "interning %llu (%.0f%%; %llu without the partner path)%s\n",
                    lines, static_cast<unsigned long long>(needed->work),
                    static_cast<double>(needed->work) / static_cast<double>(kDefaultMaxWork),
                    static_cast<unsigned long long>(interning),
                    100.0 * static_cast<double>(interning) / static_cast<double>(needed->work),
                    static_cast<unsigned long long>(tableOnly), shapes.c_str());
    }
}

TEST(MergePerf, CapScale) {
    if (!kFullScale) {
        std::printf("[ perf ] MergePerf.CapScale: full scale only (optimised, unsanitised builds)\n");
        return;
    }
    constexpr std::uint32_t kSizes[] = {50'000,  100'000, 120'000, 150'000, 175'000,
                                        250'000, 300'000, 500'000, 750'000, 1'000'000};
    for (const std::uint32_t lines : kSizes) {
        const LineSequence base = makeSequence(statements(lines, 0, 0, ""));
        const LineSequence ours = makeSequence(statements(lines, lines / 5, lines / 23, "ours"));
        const LineSequence theirs = makeSequence(statements(lines, lines / 5, lines / 11, "theirs"));
        MergeOptions unlimited;
        unlimited.diff.limits.maxWork = std::numeric_limits<std::uint64_t>::max();
        auto needed = merge3(base, ours, theirs, unlimited);
        ASSERT_TRUE(needed.has_value());
        ASSERT_EQ(needed->conflicts, 0u);
        const std::uint64_t interning = internWork({&base, &ours, &theirs});
        // Each diff gets floor(R / 2) of what interning leaves (D7), so the costlier one decides.
        std::uint64_t diffWork = 0;
        for (const LineSequence* side : {&ours, &theirs}) {
            auto diff = diffLines(base, *side, unlimited.diff);
            ASSERT_TRUE(diff.has_value());
            diffWork = std::max(diffWork, diff->work - internWork({&base, side}));
        }
        const std::uint64_t tableOnly = internWork({&base, &ours, &theirs}, detail::PartnerPath::Off);
        const auto cleanUnder = [&](std::uint64_t internUnits, std::uint64_t cap) {
            return internUnits <= cap && diffWork <= (cap - internUnits) / 2;
        };
        std::string shapes;
        for (const std::uint64_t cap : kQ4Caps) {
            MergeOptions options;
            options.diff.limits.maxWork = cap;
            auto merge = merge3(base, ours, theirs, options);
            ASSERT_TRUE(merge.has_value());
            EXPECT_TRUE(checkMerge(base, ours, theirs, *merge).has_value());
            const bool whole = merge->chunks.size() == 1 && merge->chunks[0].kind == ChunkKind::Conflict &&
                               merge->chunks[0].base == LineRange{LineIndex{0}, LineIndex{lines}};
            std::string shape = "clean";
            if (merge->degraded) {
                shape =
                    whole ? std::string("one whole-file conflict")
                          : std::format("degraded, {} conflicts in {} chunks", merge->conflicts, merge->chunks.size());
            }
            shapes += std::format("; {} {}", capName(cap), shape);
            EXPECT_EQ(!merge->degraded, cleanUnder(interning, cap)) << lines << " lines per side, cap " << cap;
            if (cap != kDefaultMaxWork) {
                continue;
            }
            // ADR 0019, D7 and Q4.
            if (lines <= 150'000) {
                EXPECT_FALSE(merge->degraded) << lines << " lines per side";
                EXPECT_EQ(merge->conflicts, 0u) << lines << " lines per side";
            }
            if (interning > cap) {
                EXPECT_TRUE(whole) << lines << " lines per side";
            }
        }
        std::printf("[ perf ] MergePerf.CapScale: %u lines per side, 5 disjoint edits per side: interning %llu "
                    "units (%.2fx 2^22; %llu without the partner path), each diff up to %llu more%s\n",
                    lines, static_cast<unsigned long long>(interning),
                    static_cast<double>(interning) / static_cast<double>(kDefaultMaxWork),
                    static_cast<unsigned long long>(tableOnly), static_cast<unsigned long long>(diffWork),
                    shapes.c_str());
    }
}

TEST(RefinePerf, EdgesOnly) {
    // 15 MiB (1.5 MiB unoptimised) of words, below maxScanBytes, with different first and last
    // bytes: the byte middle is the whole line, far above maxBytes.
    const std::size_t target = kFullScale ? std::size_t{15} << 20 : std::size_t{3} << 19;
    SplitMix64 rng(0xD1FF0068);
    std::string filler;
    filler.reserve(target + 16);
    while (filler.size() < target) {
        filler += " w";
        filler += static_cast<char>('a' + rng.below(26));
        filler += static_cast<char>('a' + rng.below(26));
    }
    const LineSequence a = makeSequence("A" + filler + "A\n");
    const LineSequence b = makeSequence("B" + filler + "B\n");
    const Hunk hunk{LineRange{LineIndex{0}, LineIndex{1}}, LineRange{LineIndex{0}, LineIndex{1}}};
    for (const Granularity granularity : {Granularity::Word, Granularity::Char}) {
        Runs runs;
        Refinement last;
        for (int run = 0; run < 3; ++run) {
            const auto start = Clock::now();
            auto refined = refineHunk(a, b, hunk, granularity);
            runs.add(msSince(start));
            ASSERT_TRUE(refined.has_value());
            last = std::move(*refined);
        }
        std::printf("[ perf ] RefinePerf.EdgesOnly %s (%s): %zu bytes, %s, best %.3f ms, worst %.3f ms (budget "
                    "50 ms)\n",
                    granularity == Granularity::Word ? "Word" : "Char", mode(), a.bytes().size(),
                    last.coarse ? "coarse" : "precise", runs.best, runs.worst);
        EXPECT_TRUE(last.coarse);
        ASSERT_EQ(last.changes.size(), 1u);
        EXPECT_EQ(last.changes[0].a.begin.value(), 0u);
        EXPECT_EQ(last.changes[0].a.end.value(), a.bytes().size() - 1);
        if (kAssertBudgets) {
            EXPECT_LE(runs.best, 50.0);
        }
    }
}

TEST(RefinePerf, ContinuationRuns) {
    /*
     * Char granularity over lines whose common prefix and suffix are 7 MiB (0.7 MiB unoptimised)
     * runs of UTF-8 continuation bytes with no lead byte (binary data, or Latin-1 text in
     * 0x80-0xBF). Each such byte is a token of its own, so the window must find its edges by the
     * three-byte rule of certainStart (ADR 0019, D8) instead of walking to the line's ends and
     * tokenising every byte. Precise: one byte changed between the runs. Coarse: a 128 KiB middle,
     * above maxBytes, differing at both of its ends.
     */
    const std::size_t runBytes = kFullScale ? std::size_t{7} << 20 : std::size_t{7} << 17;
    SplitMix64 rng(0xD1FF006A);
    std::string run;
    run.reserve(runBytes);
    while (run.size() < runBytes) {
        run += static_cast<char>(0x80 + rng.below(0x40));
    }
    std::string middleA(std::size_t{128} << 10, 'm');
    std::string middleB = middleA;
    middleA.front() = 'a';
    middleA.back() = 'a';
    middleB.front() = 'b';
    middleB.back() = 'b';
    struct Case {
        const char* name;
        LineSequence a;
        LineSequence b;
        bool coarse;
        std::size_t middle; // Bytes of the expected change on each side.
    };
    const Case cases[] = {
        {"precise", makeSequence(run + "x" + run + "\n"), makeSequence(run + "y" + run + "\n"), false, 1},
        {"coarse", makeSequence(run + middleA + run + "\n"), makeSequence(run + middleB + run + "\n"), true,
         middleA.size()},
    };
    const Hunk hunk{LineRange{LineIndex{0}, LineIndex{1}}, LineRange{LineIndex{0}, LineIndex{1}}};
    for (const Case& item : cases) {
        Runs runs;
        Refinement last;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto start = Clock::now();
            auto refined = refineHunk(item.a, item.b, hunk, Granularity::Char);
            runs.add(msSince(start));
            ASSERT_TRUE(refined.has_value());
            last = std::move(*refined);
        }
        std::printf("[ perf ] RefinePerf.ContinuationRuns %s (%s): %zu bytes, %s, best %.3f ms, worst %.3f ms "
                    "(budget 50 ms)\n",
                    item.name, mode(), item.a.bytes().size(), last.coarse ? "coarse" : "precise", runs.best,
                    runs.worst);
        EXPECT_EQ(last.coarse, item.coarse);
        ASSERT_EQ(last.changes.size(), 1u);
        EXPECT_EQ(last.changes[0].a.begin.value(), runBytes);
        EXPECT_EQ(last.changes[0].a.size(), item.middle);
        EXPECT_EQ(last.changes[0].b.size(), item.middle);
        if (kAssertBudgets) {
            EXPECT_LE(runs.best, 50.0);
        }
    }
}

struct Phase {
    double ms = 0;
    std::uint64_t units = 0;
};

// ns per unit of one phase, or 0 when it did too little work to measure.
double nsPerUnit(const Phase& phase) {
    return phase.units < 20'000 ? 0.0 : phase.ms * 1e6 / static_cast<double>(phase.units);
}

TEST(DiffPerf, WorkUnitCost) {
    SplitMix64 rng(0xD1FF0065);
    constexpr std::uint64_t kProbeWork = std::uint64_t{1} << 24;
    double worst = 0;
    std::string worstAt;
    for (const Pathology kind : kAllPathologies) {
        TextPair pair = pathologicalPair(kind, rng, sizeFor(kind));
        const LineSequence a = makeSequence(std::move(pair.a));
        const LineSequence b = makeSequence(std::move(pair.b));
        Phase intern;
        Phase histogram;
        Phase myers;
        Phase compaction;
        std::vector<std::uint32_t> ids[2];
        const LineSequence* sequences[] = {&a, &b};
        std::uint32_t idCount = 0;
        {
            detail::WorkBudget budget(std::numeric_limits<std::uint64_t>::max(), {});
            const auto start = Clock::now();
            idCount = detail::internSequences(sequences, ids, budget);
            intern = Phase{msSince(start), budget.used()};
        }
        std::vector<std::uint8_t> changedA(ids[0].size(), 0);
        std::vector<std::uint8_t> changedB(ids[1].size(), 0);
        if (!ids[0].empty() && !ids[1].empty()) {
            detail::WorkBudget budget(kProbeWork, {});
            const auto start = Clock::now();
            (void)detail::histogramDiff(ids[0], ids[1], idCount, changedA, changedB, budget);
            histogram = Phase{msSince(start), budget.used()};
        }
        {
            std::vector<std::uint8_t> flagsA(ids[0].size(), 0);
            std::vector<std::uint8_t> flagsB(ids[1].size(), 0);
            detail::WorkBudget budget(kProbeWork, {});
            const auto start = Clock::now();
            (void)detail::myersDiff(ids[0], ids[1],
                                    detail::Region{0, static_cast<std::uint32_t>(ids[0].size()), 0,
                                                   static_cast<std::uint32_t>(ids[1].size())},
                                    flagsA, flagsB, budget);
            myers = Phase{msSince(start), budget.used()};
        }
        {
            detail::WorkBudget budget(std::numeric_limits<std::uint64_t>::max(), {});
            const auto start = Clock::now();
            (void)detail::compactGitClassic(ids[0], ids[1], changedA, changedB, budget);
            compaction = Phase{msSince(start), budget.used()};
        }
        const Phase* phases[] = {&intern, &histogram, &myers, &compaction};
        const char* names[] = {"intern", "histogram", "myers", "compaction"};
        std::string line;
        for (int p = 0; p < 4; ++p) {
            const double cost = nsPerUnit(*phases[p]);
            line += std::format(" {} {:.2f} ns/unit ({} units, {:.2f} ms);", names[p], cost, phases[p]->units,
                                phases[p]->ms);
            if (cost > worst) {
                worst = cost;
                worstAt = std::format("{} {}", pathologyName(kind), names[p]);
            }
        }
        std::printf("[ perf ] WorkUnitCost %s (%s):%s\n", std::string(pathologyName(kind)).c_str(), mode(),
                    line.c_str());
    }
    std::printf("[ perf ] WorkUnitCost (%s): the most expensive unit is %s at %.2f ns\n", mode(), worstAt.c_str(),
                worst);
}

/*
 * The tuning rule of ADR 0019, D15, measured directly: diffLines over every P case at caps 2^18 ..
 * 2^25 (best of three), and the work LargeNotDegraded needs. The recommended kDefaultMaxWork is the
 * largest power of two at which every P case takes <= 50 ms and LargeNotDegraded is undegraded.
 * Debug and sanitizer builds sweep a smaller range once, at their smaller scale, and only report.
 */
TEST(DiffPerf, WorkCapSweep) {
    SplitMix64 rng(0xD1FF0067);
    std::vector<std::pair<LineSequence, LineSequence>> cases;
    for (const Pathology kind : kAllPathologies) {
        TextPair pair = pathologicalPair(kind, rng, sizeFor(kind));
        cases.emplace_back(makeSequence(std::move(pair.a)), makeSequence(std::move(pair.b)));
    }
    SplitMix64 large(0xD1FF0062);
    TextPair pair = codeLikePair(large, kLargeLines, kLargeLines / 100);
    DiffOptions unlimited;
    unlimited.limits.maxWork = std::numeric_limits<std::uint64_t>::max();
    auto largeDiff = diffLines(makeSequence(std::move(pair.a)), makeSequence(std::move(pair.b)), unlimited);
    ASSERT_TRUE(largeDiff.has_value());
    const std::uint64_t largeWork = largeDiff->work;
    const int lastExponent = kFullScale ? 25 : 22;
    const int runs = kFullScale ? 3 : 1;
    int recommended = 0;
    for (int exponent = 18; exponent <= lastExponent; ++exponent) {
        DiffOptions options;
        options.limits.maxWork = std::uint64_t{1} << exponent;
        double worst = 0;
        std::size_t worstCase = 0;
        for (std::size_t c = 0; c < cases.size(); ++c) {
            double best = std::numeric_limits<double>::max();
            for (int run = 0; run < runs; ++run) {
                const auto start = Clock::now();
                auto diff = diffLines(cases[c].first, cases[c].second, options);
                best = std::min(best, msSince(start));
                ASSERT_TRUE(diff.has_value());
            }
            if (best > worst) {
                worst = best;
                worstCase = c;
            }
        }
        const bool largeExact = largeWork <= options.limits.maxWork;
        if (worst <= 50.0 && largeExact) {
            recommended = exponent;
        }
        std::printf("[ perf ] WorkCapSweep (%s): cap 2^%d: slowest P case %s %.3f ms; LargeNotDegraded %s\n", mode(),
                    exponent, std::string(pathologyName(kAllPathologies[worstCase])).c_str(), worst,
                    largeExact ? "undegraded" : "DEGRADED");
    }
    std::printf("[ perf ] WorkCapSweep (%s): LargeNotDegraded needs %llu units; recommended kDefaultMaxWork 2^%d "
                "(%s); kDefaultMaxWork is 2^%d\n",
                mode(), static_cast<unsigned long long>(largeWork), recommended,
                recommended == 0 ? "none satisfies the rule" : "largest cap meeting the rule",
                std::countr_zero(kDefaultMaxWork));
}

TEST(DiffPerf, PeakMemory) {
#ifdef FROST_DIFF_SANITIZED
    std::printf("[ perf ] DiffPerf.PeakMemory: not measured under a sanitizer\n");
#else
    SplitMix64 rng(0xD1FF0066);
    const std::uint32_t lines = kFullScale ? kDefaultMaxLines : kDefaultMaxLines / 16;
    TextPair pair = codeLikePair(rng, lines, lines / 1000);
    const std::size_t bytesA = pair.a.size();
    const std::size_t bytesB = pair.b.size();
    gLive = 0;
    gPeak = 0;
    gCounting = true;
    // The copies are made inside the counting window, so the sequences' bytes are counted.
    auto a = LineSequence::fromBytes(std::string(pair.a));
    auto b = LineSequence::fromBytes(std::string(pair.b));
    const std::int64_t sequences = gLive.load();
    gPeak = gLive.load();
    auto diff = diffLines(*a, *b);
    const std::int64_t peak = gPeak.load() - sequences;
    ASSERT_TRUE(diff.has_value());
    // The phases one by one with one default budget: each phase's peak above what was live before
    // it, and what it keeps (the id arrays, the flags) for the next.
    struct MemoryPhase {
        std::int64_t peak = 0;
        std::int64_t kept = 0;
    };
    const auto measure = [](auto&& phase) {
        const std::int64_t before = gLive.load();
        gPeak = before;
        phase();
        return MemoryPhase{gPeak.load() - before, gLive.load() - before};
    };
    std::vector<std::uint32_t> ids[2];
    const LineSequence* both[] = {&*a, &*b};
    detail::WorkBudget budget(kDefaultMaxWork, {});
    std::uint32_t idCount = 0;
    const MemoryPhase interning = measure([&] { idCount = detail::internSequences(both, ids, budget); });
    std::vector<std::uint8_t> changedA;
    std::vector<std::uint8_t> changedB;
    const MemoryPhase flags = measure([&] {
        changedA.assign(ids[0].size(), 0);
        changedB.assign(ids[1].size(), 0);
    });
    const MemoryPhase histogram = measure([&] {
        if (idCount != detail::Interner::kExhausted) {
            (void)detail::histogramDiff(ids[0], ids[1], idCount, changedA, changedB, budget);
        }
    });
    const MemoryPhase compaction =
        measure([&] { (void)detail::compactGitClassic(ids[0], ids[1], changedA, changedB, budget); });
    gCounting = false;
    const double perLine = 1.0 / static_cast<double>(a->size() + b->size());
    std::printf("[ perf ] DiffPerf.PeakMemory (%s): %u + %u lines (%zu + %zu bytes): sequences %lld bytes "
                "(%.1f per line beyond the bytes), one diffLines call peaks at %lld bytes (%.1f per line), %s\n",
                mode(), a->size(), b->size(), bytesA, bytesB, static_cast<long long>(sequences),
                (static_cast<double>(sequences) - static_cast<double>(bytesA + bytesB)) * perLine,
                static_cast<long long>(peak), static_cast<double>(peak) * perLine,
                diff->degraded ? "degraded" : "exact");
    const std::string distinct = idCount == detail::Interner::kExhausted
                                     ? std::string("the budget ran out while interning")
                                     : std::format("{} distinct lines", idCount);
    std::printf("[ perf ] DiffPerf.PeakMemory phases, bytes per line: interning peak %.1f (ids kept %.1f; %s), "
                "flags %.1f, histogram with its Myers fallbacks peak %.1f, compaction peak %.1f\n",
                static_cast<double>(interning.peak) * perLine, static_cast<double>(interning.kept) * perLine,
                distinct.c_str(), static_cast<double>(flags.kept) * perLine,
                static_cast<double>(histogram.peak) * perLine, static_cast<double>(compaction.peak) * perLine);
    // Interning is the peak phase, and its table, keys and ids are sized for the lines the budget
    // can pay for at the table's price (ADR 0019, D7): at most four 8-byte slots, one 24-byte key
    // and one 4-byte id per such line (lines taking the cheaper partner path add only their ids).
    // Sized for the whole input instead, a call at the line limit peaked at 184.5 MB.
    const std::uint64_t affordable =
        std::min<std::uint64_t>(a->size() + b->size(), kDefaultMaxWork / detail::kInternUnitsPerLine);
    const auto bound = static_cast<std::int64_t>(60 * affordable);
    EXPECT_LE(interning.peak, bound) << affordable << " affordable lines";
    EXPECT_LE(peak, bound) << affordable << " affordable lines";
#endif
}

} // namespace
} // namespace frost::diff::test
