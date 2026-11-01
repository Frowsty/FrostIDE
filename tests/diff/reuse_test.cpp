// SPDX-License-Identifier: Apache-2.0
//
/*
 * The work-saving paths of the diff engine against their plain definitions (ADR 0019, D3 and D7):
 * the histogram's reuse across regions (index narrowing, the small-region check, the run cache)
 * marks exactly the flags of the plain rules, and the interner's partner path assigns exactly the
 * ids of interning every line through the table, over every generator class and the evenly spaced
 * shapes; both spend less work where they apply.
 */

#include "diff/diff_test_util.hpp"
#include "diff/support/generators.hpp"
#include "histogram.hpp"
#include "interner.hpp"
#include "work_budget.hpp"
#include <gtest/gtest.h>

#include <array>
#include <format>
#include <limits>
#include <span>
#include <vector>

namespace frost::diff::test {
namespace {

constexpr std::uint64_t kUnlimited = std::numeric_limits<std::uint64_t>::max();

// Unique lines, with every `every`-th replaced by a new unique line (the O(N x D) shape).
TextPair evenlySpacedPair(std::uint32_t lines, std::uint32_t every) {
    TextPair pair;
    for (std::uint32_t i = 0; i < lines; ++i) {
        const std::string line = std::format("line {}\n", i);
        pair.a += line;
        pair.b += i % every == every / 2 ? std::format("edit {}\n", i) : line;
    }
    return pair;
}

std::vector<TextPair> inputs() {
    SplitMix64 rng(0xD1FF0400);
    std::vector<TextPair> pairs;
    for (const PairClass kind : kAllPairClasses) {
        for (std::uint32_t c = 0; c < scaled(300, 60, 30); ++c) {
            pairs.push_back(generatePair(kind, rng));
        }
    }
    for (std::uint32_t c = 0; c < scaled(20, 5, 3); ++c) {
        pairs.push_back(codeLikePair(rng, 2000, 200));
    }
    for (const std::uint32_t every : {2u, 3u, 10u, 40u}) {
        pairs.push_back(evenlySpacedPair(2000, every));
    }
    return pairs;
}

struct Interned {
    std::array<std::vector<std::uint32_t>, 3> ids;
    std::uint32_t count = 0;
    std::uint64_t work = 0;
};

Interned intern(std::span<const LineSequence* const> sequences, detail::PartnerPath partners) {
    Interned out;
    detail::WorkBudget budget(kUnlimited, {});
    out.count = detail::internSequences(sequences, std::span(out.ids).first(sequences.size()), budget,
                                        detail::LineHashes::Fnv, nullptr, partners);
    out.work = budget.used();
    return out;
}

struct Flags {
    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    std::uint32_t fallbacks = 0;
    std::uint64_t work = 0;
};

Flags histogram(const Interned& ids, detail::HistogramReuse reuse) {
    Flags out;
    out.a.assign(ids.ids[0].size(), 0);
    out.b.assign(ids.ids[1].size(), 0);
    detail::WorkBudget budget(kUnlimited, {});
    const detail::HistogramResult result =
        detail::histogramDiff(ids.ids[0], ids.ids[1], ids.count, out.a, out.b, budget, reuse);
    EXPECT_FALSE(result.exhausted);
    out.fallbacks = result.fallbacks;
    out.work = budget.used();
    return out;
}

TEST(Histogram, ReuseIsExact) {
    std::uint64_t workOn = 0;
    std::uint64_t workOff = 0;
    for (const TextPair& pair : inputs()) {
        const LineSequence a = makeSequence(pair.a, LineSplit::Lf);
        const LineSequence b = makeSequence(pair.b, LineSplit::Lf);
        if (a.size() == 0 || b.size() == 0) {
            continue;
        }
        const LineSequence* both[] = {&a, &b};
        const Interned ids = intern(both, detail::PartnerPath::On);
        const Flags on = histogram(ids, detail::HistogramReuse::On);
        const Flags off = histogram(ids, detail::HistogramReuse::Off);
        ASSERT_EQ(on.a, off.a) << printable(pair.a, 120) << " vs " << printable(pair.b, 120);
        ASSERT_EQ(on.b, off.b) << printable(pair.a, 120) << " vs " << printable(pair.b, 120);
        EXPECT_EQ(on.fallbacks, off.fallbacks);
        workOn += on.work;
        workOff += off.work;
    }
    std::printf("[ diff ] histogram work with reuse %llu, without %llu\n", static_cast<unsigned long long>(workOn),
                static_cast<unsigned long long>(workOff));
}

TEST(Histogram, ReuseMakesEvenlySpacedEditsCheap) {
    // 2000 lines with every 10th edited: the plain rules re-index and re-grow most of the file for
    // each of the 200 edits; with reuse the work is a small fraction (ADR 0019, D3).
    const TextPair pair = evenlySpacedPair(2000, 10);
    const LineSequence a = makeSequence(pair.a);
    const LineSequence b = makeSequence(pair.b);
    const LineSequence* both[] = {&a, &b};
    const Interned ids = intern(both, detail::PartnerPath::On);
    const Flags on = histogram(ids, detail::HistogramReuse::On);
    const Flags off = histogram(ids, detail::HistogramReuse::Off);
    EXPECT_EQ(on.a, off.a);
    EXPECT_EQ(on.b, off.b);
    EXPECT_LT(on.work * 3, off.work) << on.work << " vs " << off.work;
}

TEST(Interner, PartnerPathIsExact) {
    SplitMix64 rng(0xD1FF0401);
    std::vector<std::vector<std::string>> cases;
    for (const TextPair& pair : inputs()) {
        cases.push_back({pair.a, pair.b});
    }
    for (std::uint32_t c = 0; c < scaled(500, 100, 50); ++c) {
        TextTriple triple = randomTriple(rng);
        cases.push_back({std::move(triple.base), std::move(triple.ours), std::move(triple.theirs)});
    }
    std::uint64_t workOn = 0;
    std::uint64_t workOff = 0;
    for (const std::vector<std::string>& texts : cases) {
        std::vector<LineSequence> sequences;
        std::vector<const LineSequence*> pointers;
        for (const std::string& text : texts) {
            sequences.push_back(makeSequence(text));
        }
        for (const LineSequence& sequence : sequences) {
            pointers.push_back(&sequence);
        }
        const Interned on = intern(pointers, detail::PartnerPath::On);
        const Interned off = intern(pointers, detail::PartnerPath::Off);
        ASSERT_EQ(on.count, off.count);
        for (std::size_t s = 0; s < pointers.size(); ++s) {
            ASSERT_EQ(on.ids[s], off.ids[s]) << "sequence " << s << ": " << printable(texts[s], 120);
        }
        // FNV-1a collisions aside (none in these inputs), the partner path only saves work.
        EXPECT_LE(on.work, off.work);
        workOn += on.work;
        workOff += off.work;
    }
    std::printf("[ diff ] interning work with the partner path %llu, without %llu\n",
                static_cast<unsigned long long>(workOn), static_cast<unsigned long long>(workOff));
    EXPECT_LT(workOn, workOff);
}

} // namespace
} // namespace frost::diff::test
