// SPDX-License-Identifier: Apache-2.0
//
// DiffProperty (ADR 0019, D16): applying a diff turns A into B, byte for byte, through three
// independent appliers (TextBuffer::applyEdits, the support byte patcher, the POSIX unified
// applier over renderUnified), with checkDiff's H1-H4 on every result; and the identities of
// diffLines (empty sides, equal inputs, Lsp vs Lf without lone CRs); and generators that give the
// same bytes on every platform.

#include "diff/diff_test_util.hpp"
#include "diff/support/generators.hpp"
#include "fnv.hpp"
#include <gtest/gtest.h>

#include <cstdio>

namespace frost::diff::test {
namespace {

TEST(DiffProperty, ApplyTurnsAIntoB) {
    SplitMix64 rng(0xD1FF0010);
    const std::uint32_t cases = scaled(20000, 6000, 3000);
    std::uint64_t hunks = 0;
    std::uint32_t fallbacks = 0;
    for (std::uint32_t c = 0; c < cases; ++c) {
        const PairClass kind = kAllPairClasses[c % std::size(kAllPairClasses)];
        const LineSplit split = (c / std::size(kAllPairClasses)) % 2 == 0 ? LineSplit::Lsp : LineSplit::Lf;
        const std::uint64_t seed = rng.next();
        SplitMix64 local(seed);
        TextPair pair = generatePair(kind, local);
        const LineSequence a = makeSequence(pair.a, split);
        const LineSequence b = makeSequence(pair.b, split);
        DiffOptions options;
        if (c % 7 == 3) {
            options.algorithm = Algorithm::Myers;
        }
        auto diff = diffLines(a, b, options);
        ASSERT_TRUE(diff.has_value()) << diff.error().describe();
        ASSERT_FALSE(diff->degraded) << "seed " << seed;
        ASSERT_TRUE(appliesCleanly(a, b, *diff, options))
            << pairClassName(kind) << " seed " << seed << " split " << (split == LineSplit::Lsp ? "Lsp" : "Lf");
        hunks += diff->hunks.size();
        fallbacks += diff->fallbacks;
    }
    std::printf("[ diff ] %u pairs, %llu hunks, %u fallback regions\n", cases, static_cast<unsigned long long>(hunks),
                fallbacks);
}

/*
 * The generators give the same bytes on every platform, so a seed printed on Windows reproduces
 * in WSL: one FNV-1a digest over pairs of every class and size, code-like pairs, pathological
 * pairs, constructive merges and random triples, against the digest both platforms produced.
 * A change here means a generator changed (or a draw order became compiler-dependent, as
 * function arguments are): update kExpected only for an intended generator change.
 */
TEST(Generators, SameOnEveryPlatform) {
    constexpr std::uint64_t kExpected = 0xCD2C'0925'0645'5821;
    std::string all;
    const auto add = [&](std::string_view bytes) {
        all += bytes;
        all += '\0';
    };
    SplitMix64 rng(0xD1FF0019);
    for (const PairClass kind : kAllPairClasses) {
        for (int k = 0; k < 22; ++k) {
            const TextPair pair = generatePair(kind, rng, k < 20 ? SizeClass::Small : SizeClass::Large);
            add(pair.a);
            add(pair.b);
        }
    }
    const TextPair code = codeLikePair(rng, 300, 20);
    add(code.a);
    add(code.b);
    for (const Pathology kind : {Pathology::P2, Pathology::P4, Pathology::P5, Pathology::P7}) {
        const TextPair pair = pathologicalPair(kind, rng, kind == Pathology::P7 ? 1 : 1000);
        add(pair.a);
        add(pair.b);
    }
    for (int k = 0; k < 20; ++k) {
        const ConstructiveMerge merge = constructiveMerge(rng);
        add(merge.base);
        add(merge.ours);
        add(merge.theirs);
        add(merge.cleanText);
        add(std::to_string(merge.conflicts));
        const TextTriple triple = randomTriple(rng);
        add(triple.base);
        add(triple.ours);
        add(triple.theirs);
    }
    const std::uint64_t digest = detail::fnv1a64(all);
    std::printf("[ diff ] generator digest 0x%016llx over %zu bytes\n", static_cast<unsigned long long>(digest),
                all.size());
    EXPECT_EQ(digest, kExpected);
}

TEST(DiffProperty, Identities) {
    SplitMix64 rng(0xD1FF0011);
    const std::uint32_t cases = scaled(3000, 1000, 500);
    for (std::uint32_t c = 0; c < cases; ++c) {
        const PairClass kind = kAllPairClasses[c % std::size(kAllPairClasses)];
        TextPair pair = generatePair(kind, rng);
        for (const LineSplit split : {LineSplit::Lsp, LineSplit::Lf}) {
            const LineSequence a = makeSequence(pair.a, split);
            const LineSequence b = makeSequence(pair.b, split);
            const LineSequence empty = makeSequence("", split);
            // diff(A, A) is empty.
            auto same = diffLines(a, a);
            ASSERT_TRUE(same.has_value());
            EXPECT_TRUE(same->hunks.empty());
            // diff("", B) is one insert, diff(A, "") one delete (or nothing when the side is empty).
            auto insert = diffLines(empty, b);
            ASSERT_TRUE(insert.has_value());
            if (b.size() == 0) {
                EXPECT_TRUE(insert->hunks.empty());
            } else {
                ASSERT_EQ(insert->hunks.size(), 1u);
                EXPECT_EQ(insert->hunks[0].b, (LineRange{LineIndex{0}, LineIndex{b.size()}}));
                EXPECT_TRUE(insert->hunks[0].a.empty());
            }
            auto erase = diffLines(a, empty);
            ASSERT_TRUE(erase.has_value());
            if (a.size() == 0) {
                EXPECT_TRUE(erase->hunks.empty());
            } else {
                ASSERT_EQ(erase->hunks.size(), 1u);
                EXPECT_EQ(erase->hunks[0].a, (LineRange{LineIndex{0}, LineIndex{a.size()}}));
                EXPECT_TRUE(erase->hunks[0].b.empty());
            }
        }
        // Lsp and Lf agree on inputs without a lone CR.
        if (!hasLoneCr(pair.a) && !hasLoneCr(pair.b)) {
            auto lsp = diffLines(makeSequence(pair.a, LineSplit::Lsp), makeSequence(pair.b, LineSplit::Lsp));
            auto lf = diffLines(makeSequence(pair.a, LineSplit::Lf), makeSequence(pair.b, LineSplit::Lf));
            ASSERT_TRUE(lsp.has_value() && lf.has_value());
            EXPECT_EQ(formatHunks(hunkTuples(*lsp)), formatHunks(hunkTuples(*lf))) << printable(pair.a);
        }
    }
    // Both empty: no hunks.
    auto none = diffLines(LineSequence(), LineSequence());
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->hunks.empty());
    EXPECT_FALSE(none->degraded);
}

TEST(DiffProperty, EditsAreSortedAndNonAdjacent) {
    SplitMix64 rng(0xD1FF0012);
    for (std::uint32_t c = 0; c < scaled(2000, 600, 300); ++c) {
        TextPair pair = generatePair(kAllPairClasses[c % std::size(kAllPairClasses)], rng);
        const LineSequence a = makeSequence(pair.a);
        const LineSequence b = makeSequence(pair.b);
        auto diff = diffLines(a, b);
        ASSERT_TRUE(diff.has_value());
        const std::vector<text::Edit> edits = toEdits(a, b, *diff);
        ASSERT_EQ(edits.size(), diff->hunks.size());
        for (std::size_t k = 1; k < edits.size(); ++k) {
            ASSERT_LT(edits[k - 1].range.end.value(), edits[k].range.begin.value());
        }
        for (const auto& edit : edits) {
            // Views into b's bytes.
            ASSERT_TRUE(edit.text.empty() ||
                        (edit.text.data() >= b.bytes().data() &&
                         edit.text.data() + edit.text.size() <= b.bytes().data() + b.bytes().size()));
        }
    }
}

} // namespace
} // namespace frost::diff::test
