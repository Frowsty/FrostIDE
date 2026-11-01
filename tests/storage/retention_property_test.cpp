// SPDX-License-Identifier: Apache-2.0
//
// Property test of LocalHistory retention against RetentionOracle, an independent plain C++ model
// of the rules (support/retention_oracle.hpp): random records and prunes, with clock steps back
// and forth, in memory (200 seeds x 200 operations) and on disk (20 x 100, which also covers the
// incremental-vacuum and checkpoint paths).

#include "frost/storage/local_history.hpp"

#include "support/retention_run.hpp"
#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <format>
#include <string>

namespace {

using frost::ManualClock;
using frost::storage::HistoryOptions;
using frost::storage::LocalHistory;
using frost::storage::test::runRetentionSeed;

constexpr frost::IClock::SystemTime kStart{std::chrono::seconds(1'700'000'000)};

TEST(RetentionProperty, OracleModelsTheRules) {
    // The oracle itself, on a hand-checked case: newest-per-path protection, then size.
    frost::storage::test::RetentionOracle oracle;
    EXPECT_TRUE(oracle.record("a", "11111", false, 0).added);  // id 1
    EXPECT_FALSE(oracle.record("a", "11111", false, 1).added); // dedup with id 1
    EXPECT_TRUE(oracle.record("a", "22", false, 2).added);     // id 2
    EXPECT_TRUE(oracle.record("b", "11111", false, 3).added);  // id 3, shares content with id 1
    EXPECT_TRUE(oracle.record("b", "333", true, 4).added);     // id 4, labelled
    // now 10, maxAge 7: ids 1 and 2 are old (created < 3); id 2 is a's newest and stays.
    EXPECT_EQ(oracle.prune(10, 7, 1000, true), 1u);
    EXPECT_EQ(oracle.ids(), (std::vector<std::int64_t>{2, 3, 4}));
    EXPECT_EQ(oracle.referencedBytes(), 10u);
    // Size 6: drop oldest unprotected first: id 3 (5 bytes) goes, then 5 bytes remain.
    EXPECT_EQ(oracle.prune(10, 100, 6, true), 1u);
    EXPECT_EQ(oracle.ids(), (std::vector<std::int64_t>{2, 4}));
    EXPECT_EQ(oracle.referencedBytes(), 5u);
}

TEST(RetentionProperty, MatchesOracle) {
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        ManualClock clock(frost::IClock::SteadyTime{}, kStart);
        auto history = LocalHistory::openInMemory(clock);
        ASSERT_TRUE(history.has_value()) << history.error().describe();
        const std::string failure = runRetentionSeed(seed, 200, *history, clock);
        ASSERT_TRUE(failure.empty()) << failure;
    }
}

TEST(RetentionProperty, MatchesOracleOnDisk) {
    frost::test::ScratchDir dir("storage-retention");
    for (std::uint64_t seed = 1001; seed <= 1020; ++seed) {
        ManualClock clock(frost::IClock::SteadyTime{}, kStart);
        HistoryOptions options;
        options.synchronous = frost::storage::Synchronous::Normal;
        const std::string path = dir.join(std::format("history{}.db", seed));
        auto history = LocalHistory::open(path, clock, options);
        ASSERT_TRUE(history.has_value()) << history.error().describe();
        const std::string failure = runRetentionSeed(seed, 100, *history, clock);
        ASSERT_TRUE(failure.empty()) << failure;
        FROST_STORAGE_ASSERT_OK(history->database().integrityCheck());
    }
}

} // namespace
