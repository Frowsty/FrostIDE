// SPDX-License-Identifier: Apache-2.0
//
// Stress runs of frost::diff (label `stress`, excluded from Debug; ADR 0019, D16). Each prints its
// master seed, and every failure its case seed; each stays within about 120 s per configuration.
/*
 * DiffStress.Property: the apply property and checkDiff over 100k pairs optimised (20k sanitised;
 * 1M at FROST_STRESS_FULL=1 in optimised unsanitised builds), Small and Large size classes, both
 * splits, under the default cap and the occasional tiny cap.
 * DiffStress.Git: the git differential over 20k adversarial and 5k code-like pairs (10k + 2k
 * sanitised; 100k + 10k at FROST_STRESS_FULL=1) in batches of 5000, with the dialect rules,
 * two-sided on both dialects.
 * MergeStress.Property: 200k triples (40k sanitised): C1-C4, the identities under caps, the
 * constructive oracle and degraded soundness.
 *
 * Reproduction: FROST_DIFF_SEED=<n> replaces a test's master seed (the whole run changes);
 * FROST_DIFF_CASE_SEED=<n> runs only the one case whose seed a failure printed ("pair seed",
 * "triple seed", or the git classes' "seed"). Both are decimal; a malformed value fails the test.
 */

#include "frost/diff/merge.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/git_test_support.hpp"
#include "diff/support/generators.hpp"
#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <optional>
#include <tuple>

namespace frost::diff::test {
namespace {

using frost::test::kOptimized;
using frost::test::kSanitized;
using frost::test::stressFull;

bool exitScale() { return stressFull() && kOptimized && !kSanitized; }

// A decimal seed from environment variable `name`, or nullopt when it is unset. A value that is
// not a whole decimal uint64 records a test failure and also gives nullopt.
std::optional<std::uint64_t> seedVariable(const char* name) {
    const std::string value = frost::test::environment(name);
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t seed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seed);
    if (error != std::errc() || end != value.data() + value.size()) {
        ADD_FAILURE() << name << "='" << value << "' is not a decimal 64-bit seed";
        return std::nullopt;
    }
    return seed;
}

std::uint64_t masterSeed(std::uint64_t fallback) { return seedVariable("FROST_DIFF_SEED").value_or(fallback); }

// FROST_DIFF_CASE_SEED, printed when set.
std::optional<std::uint64_t> caseSeed() {
    const std::optional<std::uint64_t> seed = seedVariable("FROST_DIFF_CASE_SEED");
    if (seed) {
        std::printf("[ stress ] FROST_DIFF_CASE_SEED=%llu: only that case runs\n",
                    static_cast<unsigned long long>(*seed));
    }
    return seed;
}

TEST(DiffStress, Property) {
    const std::optional<std::uint64_t> only = caseSeed();
    const std::uint32_t cases = only ? 1 : exitScale() ? 1'000'000 : kSanitized ? 20'000 : 100'000;
    const std::uint64_t master = masterSeed(0x57E55001);
    std::printf("[ stress ] DiffStress.Property: %u pairs, master seed %llu\n", cases,
                static_cast<unsigned long long>(master));
    SplitMix64 rng(master);
    for (std::uint32_t c = 0; c < cases; ++c) {
        const std::uint64_t seed = only ? *only : rng.next();
        SplitMix64 local(seed);
        const PairClass kind = kAllPairClasses[local.below(std::size(kAllPairClasses))];
        const SizeClass size = local.below(50) == 0 ? SizeClass::Large : SizeClass::Small;
        const LineSplit split = local.below(2) == 0 ? LineSplit::Lsp : LineSplit::Lf;
        TextPair pair = generatePair(kind, local, size);
        const LineSequence a = makeSequence(std::move(pair.a), split);
        const LineSequence b = makeSequence(std::move(pair.b), split);
        DiffOptions options;
        if (local.below(10) == 0) {
            options.limits.maxWork = local.below(4096);
        }
        if (local.below(8) == 0) {
            options.algorithm = Algorithm::Myers;
        }
        auto diff = diffLines(a, b, options);
        ASSERT_TRUE(diff.has_value());
        ASSERT_TRUE(appliesCleanly(a, b, *diff, options)) << "pair seed " << seed;
    }
}

TEST(DiffStress, Git) {
    auto session = startGitSession("diff-stress");
    ASSERT_TRUE(session.has_value());
    const std::optional<std::uint64_t> only = caseSeed();
    const bool full = exitScale();
    const std::uint32_t adversarial = only ? 1 : full ? 100'000 : kSanitized ? 10'000 : 20'000;
    const std::uint32_t codeLike = only ? 1 : full ? 10'000 : kSanitized ? 2'000 : 5'000;
    const std::uint64_t master = masterSeed(0x57E55002);
    std::printf("[ stress ] DiffStress.Git: %u adversarial + %u code-like, master seed %llu\n", adversarial, codeLike,
                static_cast<unsigned long long>(master));
    // git-fewer cases measured on git 2.56 over the optimised run's 20000 adversarial and 5000
    // code-like cases (ADR 0019); other configurations draw other cases, hence the slack.
    constexpr NewDialectFewer kAdversarialFewer{213, 20'000};
    constexpr NewDialectFewer kCodeLikeFewer{1, 5'000};
    constexpr std::uint32_t kSlack = 2;
    SplitMix64 rng(master);
    for (const auto& [kind, total, measured] : {std::tuple{PairClass::Adversarial, adversarial, kAdversarialFewer},
                                                std::tuple{PairClass::CodeLike, codeLike, kCodeLikeFewer}}) {
        ClassCounts sum;
        for (std::uint32_t done = 0; done < total; done += 5000) {
            const std::vector<SeededCase> cases =
                only ? std::vector<SeededCase>{caseFromSeed(kind, *only)}
                     : generateCases(kind, std::min<std::uint32_t>(5000, total - done), rng.next());
            const ClassCounts counts = compareWithGit(*session, pairClassName(kind), cases);
            sum.cases += counts.cases;
            sum.exact += counts.exact;
            sum.gitFewer += counts.gitFewer;
            sum.equalCost += counts.equalCost;
            sum.gitMore += counts.gitMore;
            sum.fallbacks += counts.fallbacks;
            sum.fallbackCases += counts.fallbackCases;
        }
        printCounts(*session, pairClassName(kind), sum);
        EXPECT_EQ(sum.gitMore, 0u);
        if (session->dialect == GitDialect::Classic) {
            EXPECT_EQ(sum.exact, sum.cases);
        } else if (!only) {
            EXPECT_LE(sum.gitFewer, measured.cap(total, kSlack))
                << pairClassName(kind) << ": " << sum.gitFewer << " of " << total
                << " cases where git changes fewer lines; " << measured.count << " of " << measured.cases
                << " were measured on git 2.56 (ADR 0019)";
        }
    }
    printTimes(*session, "DiffStress.Git");
}

TEST(MergeStress, Property) {
    const std::optional<std::uint64_t> only = caseSeed();
    const std::uint32_t cases = only ? 1 : kSanitized ? 40'000 : 200'000;
    const std::uint64_t master = masterSeed(0x57E55003);
    std::printf("[ stress ] MergeStress.Property: %u triples, master seed %llu\n", cases,
                static_cast<unsigned long long>(master));
    SplitMix64 rng(master);
    constexpr std::uint64_t kCaps[] = {0, 1, 16, 1000, kDefaultMaxWork};
    for (std::uint32_t c = 0; c < cases; ++c) {
        const std::uint64_t seed = only ? *only : rng.next();
        SplitMix64 local(seed);
        MergeOptions options;
        options.diff.limits.maxWork = kCaps[local.below(std::size(kCaps))];
        if (local.below(4) == 0) {
            // The constructive oracle and soundness under the cap.
            const ConstructiveMerge truth = constructiveMerge(local);
            auto model = MergeModel::create(makeSequence(truth.base), makeSequence(truth.ours),
                                            makeSequence(truth.theirs), options);
            ASSERT_TRUE(model.has_value());
            ASSERT_TRUE(checkMerge(model->base(), model->ours(), model->theirs(), model->merge()).has_value())
                << "triple seed " << seed;
            if (!model->merge().degraded) {
                ASSERT_EQ(model->merge().conflicts, truth.conflicts) << "triple seed " << seed;
            }
            // Soundness: every base line of a true conflict lies inside a Conflict chunk.
            for (const ConstructiveMerge::Chunk& want : truth.expected) {
                for (std::uint32_t line = want.base0; want.kind == 3 && line < want.base1; ++line) {
                    bool covered = false;
                    for (const MergeChunk& got : model->merge().chunks) {
                        covered = covered || (got.kind == ChunkKind::Conflict && got.base.begin.value() <= line &&
                                              line < got.base.end.value());
                    }
                    ASSERT_TRUE(covered) << "triple seed " << seed << ": base line " << line;
                }
            }
            if (model->merge().conflicts == 0) {
                ASSERT_EQ(truth.conflicts, 0u) << "triple seed " << seed;
                auto text = model->resolvedText();
                ASSERT_TRUE(text.has_value());
                ASSERT_EQ(text->text, truth.cleanText) << "triple seed " << seed;
            }
            continue;
        }
        const TextTriple triple = randomTriple(local, local.below(50) == 0 ? SizeClass::Large : SizeClass::Small);
        const LineSequence base = makeSequence(triple.base);
        const LineSequence ours = makeSequence(triple.ours);
        const LineSequence theirs = makeSequence(triple.theirs);
        auto merge = merge3(base, ours, theirs, options);
        ASSERT_TRUE(merge.has_value());
        ASSERT_TRUE(checkMerge(base, ours, theirs, *merge).has_value()) << "triple seed " << seed;
        auto same = MergeModel::create(base, ours, ours, options);
        ASSERT_TRUE(same.has_value());
        ASSERT_EQ(same->merge().conflicts, 0u) << "triple seed " << seed;
        auto sameText = same->resolvedText();
        ASSERT_TRUE(sameText.has_value());
        ASSERT_EQ(sameText->text, triple.ours) << "triple seed " << seed;
        auto onlyTheirs = MergeModel::create(base, base, theirs, options);
        ASSERT_TRUE(onlyTheirs.has_value());
        auto theirsText = onlyTheirs->resolvedText();
        ASSERT_TRUE(theirsText.has_value());
        ASSERT_EQ(theirsText->text, triple.theirs) << "triple seed " << seed;
    }
}

} // namespace
} // namespace frost::diff::test
