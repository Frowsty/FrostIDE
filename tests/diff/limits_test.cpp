// SPDX-License-Identifier: Apache-2.0
//
/*
 * Limits of diffLines (ADR 0019, D2 and D7): tiny work caps give valid degraded results, the line
 * limit, cancellation before the start and midway, an interner flood of colliding hashes that the
 * cap bounds, and the interner's table, keys and id arrays sized by the lines the budget can pay
 * for. Evenly spaced edits (the histogram's O(N * D) shape) and the memory of a call at the line
 * limit are measured in perf_test.cpp (DiffPerf.EvenlySpaced, DiffPerf.PeakMemory).
 */

#include "diff/diff_test_util.hpp"
#include "diff/support/generators.hpp"
#include "engine.hpp"
#include "interner.hpp"
#include "work_budget.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <limits>
#include <stop_token>
#include <thread>

namespace frost::diff::test {
namespace {

std::vector<TextPair> tinyCapInputs(SplitMix64& rng) {
    std::vector<TextPair> inputs;
    const std::uint32_t perClass = scaled(150, 60, 30);
    for (const PairClass kind : kAllPairClasses) {
        for (std::uint32_t c = 0; c < perClass; ++c) {
            inputs.push_back(generatePair(kind, rng));
        }
    }
    for (const Pathology kind : kAllPathologies) {
        inputs.push_back(pathologicalPair(kind, rng, kind == Pathology::P6 || kind == Pathology::P7 ? 16 : 2000));
    }
    return inputs;
}

TEST(Limits, TinyWorkStillValid) {
    SplitMix64 rng(0xD1FF0020);
    const std::vector<TextPair> inputs = tinyCapInputs(rng);
    std::uint32_t degraded = 0;
    for (const TextPair& pair : inputs) {
        const LineSequence a = makeSequence(pair.a, LineSplit::Lf);
        const LineSequence b = makeSequence(pair.b, LineSplit::Lf);
        auto full = diffLines(a, b);
        ASSERT_TRUE(full.has_value());
        for (const std::uint64_t cap : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{16}, std::uint64_t{1000}}) {
            DiffOptions options;
            options.limits.maxWork = cap;
            auto diff = diffLines(a, b, options);
            ASSERT_TRUE(diff.has_value());
            ASSERT_TRUE(appliesCleanly(a, b, *diff, options)) << "cap " << cap << ": " << printable(pair.a, 80);
            EXPECT_LE(diff->work, cap);
            if (diff->degraded) {
                ++degraded;
            } else {
                // A run that did not run out computed exactly what the uncapped run computes.
                EXPECT_EQ(formatHunks(hunkTuples(*diff)), formatHunks(hunkTuples(*full))) << "cap " << cap;
            }
            if (cap == 0 && a.bytes() != b.bytes()) {
                // Nothing may be compared at all: the whole input is one hunk.
                ASSERT_EQ(diff->hunks.size(), 1u);
                EXPECT_EQ(diff->hunks[0].a, (LineRange{LineIndex{0}, LineIndex{a.size()}}));
                EXPECT_EQ(diff->hunks[0].b, (LineRange{LineIndex{0}, LineIndex{b.size()}}));
                EXPECT_EQ(diff->degraded, a.size() != 0 && b.size() != 0);
            }
        }
    }
    EXPECT_GT(degraded, 0u);
}

TEST(Limits, MaxLines) {
    const LineSequence a = makeSequence("1\n2\n3\n4\n5\n");
    const LineSequence b = makeSequence("1\n2\n");
    DiffOptions options;
    options.limits.maxLines = 4;
    auto tooMany = diffLines(a, b, options);
    ASSERT_FALSE(tooMany.has_value());
    EXPECT_EQ(tooMany.error().code(), ErrorCode::OutOfRange);
    auto other = diffLines(b, a, options);
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().code(), ErrorCode::OutOfRange);
    options.limits.maxLines = 5;
    EXPECT_TRUE(diffLines(a, b, options).has_value());
}

TEST(Limits, CancelledBeforeStart) {
    std::stop_source source;
    source.request_stop();
    const LineSequence a = makeSequence("a\nb\n");
    const LineSequence b = makeSequence("a\nc\n");
    auto diff = diffLines(a, b, {}, source.get_token());
    ASSERT_FALSE(diff.has_value());
    EXPECT_EQ(diff.error().code(), ErrorCode::Cancelled);
    // Even equal inputs observe the stop at entry.
    auto same = diffLines(a, a, {}, source.get_token());
    ASSERT_FALSE(same.has_value());
    EXPECT_EQ(same.error().code(), ErrorCode::Cancelled);
}

TEST(Limits, CancelledMidway) {
    // Two independent random two-symbol texts of 100k lines: every line repeats far more than 64
    // times, so the uncapped diff is one Myers run with D in the tens of thousands, which takes
    // seconds; building the input is linear (the P2 generator's 5000 vector mutations took 8 s in
    // Debug).
    SplitMix64 rng(0xD1FF0021);
    std::string textA;
    std::string textB;
    for (std::uint32_t i = 0; i < 100000; ++i) {
        textA += rng.below(2) == 0 ? "a\n" : "b\n";
        textB += rng.below(2) == 0 ? "a\n" : "b\n";
    }
    const LineSequence a = makeSequence(std::move(textA));
    const LineSequence b = makeSequence(std::move(textB));
    DiffOptions options;
    options.limits.maxWork = std::numeric_limits<std::uint64_t>::max();
    std::stop_source source;
    std::chrono::steady_clock::time_point requested;
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        requested = std::chrono::steady_clock::now();
        source.request_stop();
    });
    auto diff = diffLines(a, b, options, source.get_token());
    const auto returned = std::chrono::steady_clock::now();
    stopper.join();
    ASSERT_FALSE(diff.has_value()) << "the uncapped P2 diff finished within 20 ms";
    EXPECT_EQ(diff.error().code(), ErrorCode::Cancelled);
    const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(returned - requested);
    std::printf("[ diff ] cancellation returned %lld us after the stop request (%s)\n",
                static_cast<long long>(latency.count()), frost::test::kAssertBudgets ? "budgeted" : "report-only");
    if (frost::test::kAssertBudgets) {
        EXPECT_LE(latency.count(), 100000);
    }
}

TEST(Limits, InternerFlooding) {
    // Every line hashes alike (the test-only constant hash), so each intern probes every earlier
    // key: quadratic without a cap. The cap bounds it and the result stays valid.
    std::string a;
    std::string b;
    for (std::uint32_t i = 0; i < 4000; ++i) {
        a += std::format("line {}\n", i);
        b += std::format("line {}\n", i % 7 == 0 ? i + 100000 : i);
    }
    const LineSequence sa = makeSequence(a);
    const LineSequence sb = makeSequence(b);
    DiffOptions options;
    options.limits.maxWork = 1u << 16;
    const auto start = std::chrono::steady_clock::now();
    auto diff = detail::diffLinesWith(sa, sb, options, {}, detail::LineHashes::ConstantForTests);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(diff.has_value());
    EXPECT_TRUE(diff->degraded);
    EXPECT_LE(diff->work, options.limits.maxWork);
    EXPECT_TRUE(appliesCleanly(sa, sb, *diff, options));
    // The default cap bounds the flood too (about 8000^2 / 2 probes would be needed).
    DiffOptions defaults;
    const auto defaultStart = std::chrono::steady_clock::now();
    auto bounded = detail::diffLinesWith(sa, sb, defaults, {}, detail::LineHashes::ConstantForTests);
    const auto defaultElapsed = std::chrono::steady_clock::now() - defaultStart;
    ASSERT_TRUE(bounded.has_value());
    EXPECT_TRUE(bounded->degraded);
    EXPECT_TRUE(appliesCleanly(sa, sb, *bounded, defaults));
    std::printf("[ diff ] flooded interner: %lld us under maxWork %llu, %lld us under the default cap\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()),
                static_cast<unsigned long long>(options.limits.maxWork),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(defaultElapsed).count()));
    // With real hashes the same input is exact under the default cap.
    auto normal = diffLines(sa, sb, defaults);
    ASSERT_TRUE(normal.has_value());
    EXPECT_FALSE(normal->degraded);
    // Colliding hashes still intern correctly with enough work: equal ids for equal lines only.
    options.limits.maxWork = std::numeric_limits<std::uint64_t>::max();
    auto flooded = detail::diffLinesWith(sa, sb, options, {}, detail::LineHashes::ConstantForTests);
    ASSERT_TRUE(flooded.has_value());
    EXPECT_FALSE(flooded->degraded);
    EXPECT_EQ(formatHunks(hunkTuples(*flooded)), formatHunks(hunkTuples(*normal)));
}

// Interns `sequences` under a budget of `affordable` lines (kInternUnitsPerLine units each) and
// returns the interner's heap bytes; ids receives the id arrays.
std::size_t internUnder(std::span<const LineSequence* const> sequences, std::span<std::vector<std::uint32_t>> ids,
                        std::uint64_t affordable) {
    detail::WorkBudget budget(affordable * detail::kInternUnitsPerLine, {});
    std::size_t bytes = 0;
    (void)detail::internSequences(sequences, ids, budget, detail::LineHashes::Fnv, &bytes);
    return bytes;
}

std::string uniqueLines(std::string_view prefix, std::uint32_t count) {
    std::string text;
    for (std::uint32_t i = 0; i < count; ++i) {
        text += std::format("{} {}\n", prefix, i);
    }
    return text;
}

TEST(Limits, InternerSizedByBudget) {
    // 100k unique lines per side under a budget for 1000 lines: the interner is built for 1000
    // keys and the first id array reserves 1000 ids, not 200k (ADR 0019, D7). The budget runs out
    // in the first sequence, so the second one is not reached.
    const LineSequence a = makeSequence(uniqueLines("a", 100'000));
    const LineSequence b = makeSequence(uniqueLines("b", 100'000));
    const LineSequence* both[] = {&a, &b};
    std::vector<std::uint32_t> ids[2];
    const std::size_t bytes = internUnder(both, ids, 1000);
    const std::size_t want = detail::Interner(1000, detail::kInternUnitsPerLine).memoryBytes();
    EXPECT_EQ(bytes, want);
    EXPECT_LT(bytes, detail::Interner(200'000, detail::kInternUnitsPerLine).memoryBytes() / 100);
    EXPECT_EQ(ids[0].capacity(), 1000u);
    EXPECT_LE(ids[0].size(), 1000u);
    EXPECT_GT(ids[0].size(), 900u); // Only the probes beyond the first slot cost more.
    EXPECT_EQ(ids[1].capacity(), 0u);
    // 300 + 100k lines under a budget for 1000: the second array reserves the 700 the first left.
    const LineSequence small = makeSequence(uniqueLines("s", 300));
    const LineSequence* mixed[] = {&small, &b};
    std::vector<std::uint32_t> mixedIds[2];
    EXPECT_EQ(internUnder(mixed, mixedIds, 1000), want);
    EXPECT_EQ(mixedIds[0].capacity(), 300u);
    EXPECT_EQ(mixedIds[0].size(), 300u);
    EXPECT_EQ(mixedIds[1].capacity(), 700u);
    EXPECT_LE(mixedIds[1].size(), 700u);
    // A budget for every line sizes for every line, and interns them all.
    std::vector<std::uint32_t> allIds[2];
    EXPECT_EQ(internUnder(mixed, allIds, std::numeric_limits<std::uint64_t>::max() / detail::kInternUnitsPerLine),
              detail::Interner(100'300, detail::kInternUnitsPerLine).memoryBytes());
    EXPECT_EQ(allIds[0].size(), 300u);
    EXPECT_EQ(allIds[1].size(), 100'000u);
}

} // namespace
} // namespace frost::diff::test
