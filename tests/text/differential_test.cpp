// SPDX-License-Identifier: Apache-2.0
//
// TextDifferential.Quick: 200 seeds x 1000 random ops of TextBuffer against the brute-force
// TextOracle (text_ops.hpp), the -Quick share of the piece-tree differential (ADR 0012), plus
// the tests of its settings and of the reproduction command that failures print.
/*
 * Each seed picks its piece cap from {4, 5, 7, 16, 64, 4096} and an initial text (empty for a
 * quarter of the seeds, so every cap also starts empty); one seed in seven runs with a small
 * buffer limit (seedBufferLimit). A full check runs every 64 ops with 16 random queries of every
 * conversion, and a sweep of every line ends each seed. Each seed is deterministic (no time or
 * thread input); seeds are independent, so they run on up to four threads to keep the Debug wall
 * time within the -Quick ceiling (15 s), as sched's seed test does.
 *
 * Environment overrides, for reproducing a failure (text_ops.hpp, quickSettings):
 *   FROST_TEXT_SEED=<s>        run only seed s;
 *   FROST_TEXT_OPS=<n>         n ops per seed instead of 1000;
 *   FROST_TEXT_CHECK_EVERY=<k> a full check every k ops instead of 64;
 *   FROST_TEXT_QUERIES=<q>     q random queries per full check instead of 16;
 *   FROST_TEXT_THREADS=<t>     t worker threads (1..1024) instead of min(4, hardware threads / 2).
 * A failure prints the command that reproduces it (the lowest failing seed); TextStress.Differential
 * prints the same command with its own settings.
 */

#include "support/test_env.hpp"
#include "text/support/text_ops.hpp"
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
#include <utility>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::Result;
using frost::test::environment;
using frost::text::test::DriverStats;
using frost::text::test::QuickSettings;
using frost::text::test::quickSettings;
using frost::text::test::reproduceCommand;
using frost::text::test::runSeed;
using frost::text::test::SeedRun;

TEST(TextDifferential, Quick) {
    constexpr std::uint64_t kSeeds = 200;
    const Result<QuickSettings> settings =
        quickSettings([](std::string_view name) { return environment(std::string(name).c_str()); });
    ASSERT_TRUE(settings.has_value()) << settings.error().describe();
    const std::optional<std::uint64_t> onlySeed = settings->onlySeed;
    const SeedRun base = settings->run;
    const std::uint64_t first = onlySeed.value_or(0);
    const std::uint64_t last = onlySeed ? *onlySeed + 1 : kSeeds;
    const unsigned threads =
        onlySeed ? 1u : settings->threads.value_or(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u));
    const auto start = std::chrono::steady_clock::now();

    std::atomic<std::uint64_t> next{first};
    std::mutex mutex;
    DriverStats total;
    std::uint64_t seeds = 0;
    std::map<std::uint64_t, std::string> failures; // By seed, so the report names the lowest one.
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::uint64_t seed = next.fetch_add(1); seed < last; seed = next.fetch_add(1)) {
                SeedRun run = base;
                run.seed = seed;
                const auto result = runSeed(run);
                const std::lock_guard lock(mutex);
                if (!result) {
                    failures.emplace(seed, result.error().message());
                    continue;
                }
                ++seeds;
                total.ops += result->ops;
                total.inserts += result->inserts;
                total.erases += result->erases;
                total.replaces += result->replaces;
                total.batches += result->batches;
                total.restores += result->restores;
                total.reloads += result->reloads;
                total.rejected += result->rejected;
                total.overLimit += result->overLimit;
                total.fullChecks += result->fullChecks;
                total.checkedBytes += result->checkedBytes;
                total.maxBytes = std::max(total.maxBytes, result->maxBytes);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("TextDifferential.Quick: %llu seeds on %u threads, %llu ops (%llu inserts, %llu erases, %llu "
                "replaces, %llu batches, %llu restores, %llu reloads, %llu rejected, %llu of them over the buffer "
                "limit), %llu full checks (mean text %llu bytes), largest text %llu bytes, %.2f s\n",
                static_cast<unsigned long long>(seeds), threads, static_cast<unsigned long long>(total.ops),
                static_cast<unsigned long long>(total.inserts), static_cast<unsigned long long>(total.erases),
                static_cast<unsigned long long>(total.replaces), static_cast<unsigned long long>(total.batches),
                static_cast<unsigned long long>(total.restores), static_cast<unsigned long long>(total.reloads),
                static_cast<unsigned long long>(total.rejected), static_cast<unsigned long long>(total.overLimit),
                static_cast<unsigned long long>(total.fullChecks),
                static_cast<unsigned long long>(total.checkedBytes / std::max<std::uint64_t>(total.fullChecks, 1)),
                static_cast<unsigned long long>(total.maxBytes), seconds);
    if (!failures.empty()) {
        const auto& [seed, message] = *failures.begin();
        SeedRun failed = base;
        failed.seed = seed;
        FAIL() << failures.size() << " seeds failed; the first:\n"
               << message << "\nReproduce with: " << reproduceCommand(failed);
    }
    // Every op kind ran, and the buffer limit rejected some edits (on the default scale).
    if (!onlySeed && base.ops >= 1000) {
        EXPECT_GT(total.batches, 0u);
        EXPECT_GT(total.restores, 0u);
        EXPECT_GT(total.reloads, 0u);
        EXPECT_GT(total.rejected, 0u);
        EXPECT_GT(total.overLimit, 0u);
    }
}

// The printed command must re-run the failing configuration exactly: a review found that the
// stress test's command dropped its 64 queries per check, so the reproduction drew other queries.
TEST(TextDifferential, ReproduceCommandRoundTrips) {
    const SeedRun runs[] = {
        {.seed = 1'000'001, .ops = 20'000, .checkEvery = 16, .queriesPerCheck = 64}, // TextStress.Differential.
        {.seed = 17, .ops = 1000, .checkEvery = 64, .queriesPerCheck = 16},          // TextDifferential.Quick.
        {.seed = ~std::uint64_t{0}, .ops = 1, .checkEvery = 0, .queriesPerCheck = 0xFFFF'FFFFu},
    };
    for (const SeedRun& run : runs) {
        const std::string command = reproduceCommand(run);
        // The NAME=value words before the executable, as a shell passes them.
        std::map<std::string, std::string, std::less<>> variables;
        std::istringstream words(command);
        std::string word;
        while (words >> word && word.find('=') != std::string::npos) {
            variables.emplace(word.substr(0, word.find('=')), word.substr(word.find('=') + 1));
        }
        EXPECT_EQ(word, "text_tests") << command;
        ASSERT_TRUE(static_cast<bool>(words >> word)) << command;
        EXPECT_EQ(word, "--gtest_filter=TextDifferential.Quick") << command;
        const Result<QuickSettings> settings = quickSettings([&](std::string_view name) {
            const auto found = variables.find(name);
            return found == variables.end() ? std::string() : found->second;
        });
        ASSERT_TRUE(settings.has_value()) << settings.error().describe();
        EXPECT_EQ(settings->onlySeed, std::optional<std::uint64_t>(run.seed)) << command;
        EXPECT_TRUE(settings->run == run) << command;
    }
}

TEST(TextDifferential, SettingsDefaultsAndBadValues) {
    const auto only = [](std::string_view variable, std::string value) {
        return [variable, value](std::string_view name) { return name == variable ? value : std::string(); };
    };
    const Result<QuickSettings> defaults = quickSettings(only("UNSET", "1"));
    ASSERT_TRUE(defaults.has_value()) << defaults.error().describe();
    EXPECT_FALSE(defaults->onlySeed.has_value());
    EXPECT_TRUE(defaults->run == SeedRun{});
    EXPECT_FALSE(defaults->threads.has_value());

    const Result<QuickSettings> threads = quickSettings(only("FROST_TEXT_THREADS", "3"));
    ASSERT_TRUE(threads.has_value()) << threads.error().describe();
    EXPECT_EQ(threads->threads, std::optional<unsigned>(3u));

    // Not a number, trailing text, a sign, beyond 32 bits, and zero threads (which would run no seed).
    const std::pair<std::string_view, std::string> bad[] = {
        {"FROST_TEXT_OPS", "many"},   {"FROST_TEXT_OPS", "12x"},
        {"FROST_TEXT_QUERIES", "-1"}, {"FROST_TEXT_CHECK_EVERY", "4294967296"},
        {"FROST_TEXT_SEED", " 1"},    {"FROST_TEXT_THREADS", "0"},
    };
    for (const auto& [variable, value] : bad) {
        const Result<QuickSettings> settings = quickSettings(only(variable, value));
        ASSERT_FALSE(settings.has_value()) << variable << "=" << value;
        EXPECT_EQ(settings.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(settings.error().message().find(variable), std::string::npos) << settings.error().message();
    }
}

} // namespace
