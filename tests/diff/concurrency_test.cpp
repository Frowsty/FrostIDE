// SPDX-License-Identifier: Apache-2.0
//
// Concurrency.SharedSequences (ADR 0019, D14): LineSequences are immutable and diffLines/merge3 are
// reentrant, so eight threads diffing and merging the same sequences get the serial results. Runs
// under TSan in the WSL -Full leg.

#include "frost/diff/merge.hpp"
#include "frost/diff/refine.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/support/generators.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace frost::diff::test {
namespace {

TEST(Concurrency, SharedSequences) {
    SplitMix64 rng(0xD1FF0050);
    struct Input {
        LineSequence base;
        LineSequence ours;
        LineSequence theirs;
        std::string diff; // Serial results.
        std::string merge;
    };
    std::vector<Input> inputs;
    for (int k = 0; k < 8; ++k) {
        const TextTriple triple = randomTriple(rng);
        Input input{makeSequence(triple.base), makeSequence(triple.ours), makeSequence(triple.theirs), {}, {}};
        auto diff = diffLines(input.base, input.ours);
        auto merge = merge3(input.base, input.ours, input.theirs);
        ASSERT_TRUE(diff.has_value() && merge.has_value());
        input.diff = formatHunks(hunkTuples(*diff));
        input.merge = std::to_string(merge->chunks.size()) + "/" + std::to_string(merge->conflicts);
        inputs.push_back(std::move(input));
    }
    const std::uint32_t iterations = scaled(2000, 300, 200);
    std::atomic<std::uint32_t> mismatches{0};
    std::vector<std::thread> threads;
    for (std::uint32_t t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (std::uint32_t i = 0; i < iterations; ++i) {
                // Each thread holds its own copies of the shared sequences (copies share storage).
                const Input& input = inputs[(t + i) % inputs.size()];
                const LineSequence base = input.base;
                auto diff = diffLines(base, input.ours);
                auto merge = merge3(base, input.ours, input.theirs);
                if (!diff || !merge || formatHunks(hunkTuples(*diff)) != input.diff ||
                    std::to_string(merge->chunks.size()) + "/" + std::to_string(merge->conflicts) != input.merge) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
                if (diff && !diff->hunks.empty()) {
                    auto refined = refineHunk(base, input.ours, diff->hunks.front(), Granularity::Word);
                    if (!refined) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(mismatches.load(), 0u);
}

} // namespace
} // namespace frost::diff::test
