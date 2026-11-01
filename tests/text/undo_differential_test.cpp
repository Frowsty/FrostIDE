// SPDX-License-Identifier: Apache-2.0
//
// UndoDifferential.Quick: 200 seeds x 1000 random ops of Document against the UndoOracle
// (undo_ops.hpp), the -Quick share of the undo and anchor differential (ADR 0017), plus the tests
// of its settings and of the reproduction command that failures print.
/*
 * Each seed picks its piece cap from {4, 5, 7, 16, 4096}, its history cap from {1000, 8, 1}, a
 * small buffer limit for one seed in seven and an initial text of 0-4 KiB; a full check runs every
 * 32 ops and at the end. Seeds are deterministic and independent, so they run on up to four
 * threads to keep the Debug wall time within the -Quick ceiling (15 s).
 *
 * Environment overrides, for reproducing a failure (undo_ops.hpp, undoQuickSettings):
 *   FROST_UNDO_SEED=<s>        run only seed s;
 *   FROST_UNDO_OPS=<n>         n ops per seed instead of 1000;
 *   FROST_UNDO_CHECK_EVERY=<k> a full check every k ops instead of 32;
 *   FROST_UNDO_THREADS=<t>     t worker threads (1..1024) instead of min(4, hardware threads / 2).
 * A failure prints the command that reproduces it (the lowest failing seed).
 */

#include "support/test_env.hpp"
#include "text/support/undo_ops.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::Result;
using frost::test::environment;
using frost::text::test::reproduceCommand;
using frost::text::test::runUndoSeed;
using frost::text::test::UndoDriverStats;
using frost::text::test::undoQuickSettings;
using frost::text::test::UndoQuickSettings;
using frost::text::test::UndoSeedRun;

TEST(UndoDifferential, Quick) {
    constexpr std::uint64_t kSeeds = 200;
    const Result<UndoQuickSettings> settings =
        undoQuickSettings([](std::string_view name) { return environment(std::string(name).c_str()); });
    ASSERT_TRUE(settings.has_value()) << settings.error().describe();
    const std::optional<std::uint64_t> onlySeed = settings->onlySeed;
    const UndoSeedRun base = settings->run;
    const std::uint64_t first = onlySeed.value_or(0);
    const std::uint64_t last = onlySeed ? *onlySeed + 1 : kSeeds;
    const unsigned threads =
        onlySeed ? 1u : settings->threads.value_or(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u));
    const auto start = std::chrono::steady_clock::now();

    std::atomic<std::uint64_t> next{first};
    std::mutex mutex;
    UndoDriverStats total;
    std::uint64_t seeds = 0;
    std::map<std::uint64_t, std::string> failures;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::uint64_t seed = next.fetch_add(1); seed < last; seed = next.fetch_add(1)) {
                UndoSeedRun run = base;
                run.seed = seed;
                const auto result = runUndoSeed(run);
                const std::lock_guard lock(mutex);
                if (!result) {
                    failures.emplace(seed, result.error().message());
                    continue;
                }
                ++seeds;
                total.ops += result->ops;
                total.edits += result->edits;
                total.noOps += result->noOps;
                total.undos += result->undos;
                total.redos += result->redos;
                total.groups += result->groups;
                total.rollbacks += result->rollbacks;
                total.commits += result->commits;
                total.cancels += result->cancels;
                total.preedits += result->preedits;
                total.anchorOps += result->anchorOps;
                total.rejected += result->rejected;
                total.fullChecks += result->fullChecks;
                total.maxBytes = std::max(total.maxBytes, result->maxBytes);
                total.maxNodes = std::max(total.maxNodes, result->maxNodes);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("UndoDifferential.Quick: %llu seeds on %u threads, %llu ops (%llu edits, %llu no-ops, %llu undos, "
                "%llu redos, %llu groups, %llu rollbacks, %llu commits, %llu cancels, %llu preedits, %llu anchor ops, "
                "%llu rejected), %llu full checks, largest text %llu bytes, most nodes %llu, %.2f s\n",
                static_cast<unsigned long long>(seeds), threads, static_cast<unsigned long long>(total.ops),
                static_cast<unsigned long long>(total.edits), static_cast<unsigned long long>(total.noOps),
                static_cast<unsigned long long>(total.undos), static_cast<unsigned long long>(total.redos),
                static_cast<unsigned long long>(total.groups), static_cast<unsigned long long>(total.rollbacks),
                static_cast<unsigned long long>(total.commits), static_cast<unsigned long long>(total.cancels),
                static_cast<unsigned long long>(total.preedits), static_cast<unsigned long long>(total.anchorOps),
                static_cast<unsigned long long>(total.rejected), static_cast<unsigned long long>(total.fullChecks),
                static_cast<unsigned long long>(total.maxBytes), static_cast<unsigned long long>(total.maxNodes),
                seconds);
    if (!failures.empty()) {
        const auto& [seed, message] = *failures.begin();
        UndoSeedRun failed = base;
        failed.seed = seed;
        FAIL() << failures.size() << " seeds failed; the first:\n"
               << message << "\nReproduce with: " << reproduceCommand(failed);
    }
    if (!onlySeed && base.ops >= 1000) {
        EXPECT_GT(total.undos, 0u);
        EXPECT_GT(total.redos, 0u);
        EXPECT_GT(total.rollbacks, 0u);
        EXPECT_GT(total.commits, 0u);
        EXPECT_GT(total.cancels, 0u);
        EXPECT_GT(total.rejected, 0u);
    }
}

TEST(UndoDifferential, ReproduceCommandRoundTrips) {
    const UndoSeedRun runs[] = {
        {.seed = 2'000'003, .ops = 20'000, .checkEvery = 64},
        {.seed = 17, .ops = 1000, .checkEvery = 32},
        {.seed = ~std::uint64_t{0}, .ops = 1, .checkEvery = 0},
    };
    for (const UndoSeedRun& run : runs) {
        const std::string command = reproduceCommand(run);
        std::map<std::string, std::string, std::less<>> variables;
        std::istringstream words(command);
        std::string word;
        while (words >> word && word.find('=') != std::string::npos) {
            variables.emplace(word.substr(0, word.find('=')), word.substr(word.find('=') + 1));
        }
        EXPECT_EQ(word, "text_tests") << command;
        ASSERT_TRUE(static_cast<bool>(words >> word)) << command;
        EXPECT_EQ(word, "--gtest_filter=UndoDifferential.Quick") << command;
        const Result<UndoQuickSettings> settings = undoQuickSettings([&](std::string_view name) {
            const auto found = variables.find(name);
            return found == variables.end() ? std::string() : found->second;
        });
        ASSERT_TRUE(settings.has_value()) << settings.error().describe();
        EXPECT_EQ(settings->onlySeed, std::optional<std::uint64_t>(run.seed)) << command;
        EXPECT_TRUE(settings->run == run) << command;
    }
    // Defaults and a bad value.
    const Result<UndoQuickSettings> defaults = undoQuickSettings([](std::string_view) { return std::string(); });
    ASSERT_TRUE(defaults.has_value());
    EXPECT_TRUE(defaults->run == UndoSeedRun{});
    const Result<UndoQuickSettings> bad = undoQuickSettings(
        [](std::string_view name) { return name == "FROST_UNDO_THREADS" ? std::string("0") : std::string(); });
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
