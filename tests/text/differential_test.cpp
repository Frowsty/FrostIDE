// SPDX-License-Identifier: Apache-2.0
//
// TextDifferential.Quick: 200 seeds x 1000 random ops of TextBuffer against the brute-force
// TextOracle (text_ops.hpp), the -Quick share of the piece-tree differential (ADR 0012).
/*
 * Each seed picks its piece cap from {4, 5, 7, 16, 64, 4096} and an initial text (empty for a
 * quarter of the seeds, so every cap also starts empty). A full check runs every 64 ops with 16
 * random queries of every conversion, and a sweep of every line ends each seed. Each seed is
 * deterministic (no time or thread input); seeds are independent, so they run on up to four
 * threads to keep the Debug wall time within the -Quick ceiling (15 s), as sched's seed test does.
 *
 * Environment overrides, for reproducing a failure:
 *   FROST_TEXT_SEED=<s>        run only seed s;
 *   FROST_TEXT_OPS=<n>         n ops per seed instead of 1000;
 *   FROST_TEXT_CHECK_EVERY=<k> a full check every k ops instead of 64;
 *   FROST_TEXT_THREADS=<t>     t worker threads instead of min(4, hardware threads / 2).
 * A failure prints the command that reproduces it (the lowest failing seed).
 */

#include "support/test_env.hpp"
#include "text/support/text_ops.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using frost::test::environment;
using frost::text::test::DriverStats;
using frost::text::test::runSeed;
using frost::text::test::SeedRun;

// The value of numeric environment variable `name`, if set.
std::optional<std::uint64_t> numberFromEnvironment(const char* name) {
    const std::string value = environment(name);
    if (value.empty()) {
        return std::nullopt;
    }
    return std::stoull(value);
}

TEST(TextDifferential, Quick) {
    constexpr std::uint64_t kSeeds = 200;
    const std::optional<std::uint64_t> onlySeed = numberFromEnvironment("FROST_TEXT_SEED");
    const auto ops = static_cast<std::uint32_t>(numberFromEnvironment("FROST_TEXT_OPS").value_or(1000));
    const auto checkEvery = static_cast<std::uint32_t>(numberFromEnvironment("FROST_TEXT_CHECK_EVERY").value_or(64));
    const std::uint64_t first = onlySeed.value_or(0);
    const std::uint64_t last = onlySeed ? *onlySeed + 1 : kSeeds;
    const unsigned threads =
        onlySeed ? 1u
                 : static_cast<unsigned>(numberFromEnvironment("FROST_TEXT_THREADS")
                                             .value_or(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u)));
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
                const auto result =
                    runSeed(SeedRun{.seed = seed, .ops = ops, .checkEvery = checkEvery, .queriesPerCheck = 16});
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
                "replaces, %llu batches, %llu restores, %llu reloads, %llu rejected), %llu full checks (mean text "
                "%llu bytes), largest text %llu bytes, %.2f s\n",
                static_cast<unsigned long long>(seeds), threads, static_cast<unsigned long long>(total.ops),
                static_cast<unsigned long long>(total.inserts), static_cast<unsigned long long>(total.erases),
                static_cast<unsigned long long>(total.replaces), static_cast<unsigned long long>(total.batches),
                static_cast<unsigned long long>(total.restores), static_cast<unsigned long long>(total.reloads),
                static_cast<unsigned long long>(total.rejected), static_cast<unsigned long long>(total.fullChecks),
                static_cast<unsigned long long>(total.checkedBytes / std::max<std::uint64_t>(total.fullChecks, 1)),
                static_cast<unsigned long long>(total.maxBytes), seconds);
    if (!failures.empty()) {
        const auto& [seed, message] = *failures.begin();
        FAIL() << failures.size() << " seeds failed; the first:\n"
               << message << "\nReproduce with: FROST_TEXT_SEED=" << seed << " FROST_TEXT_OPS=" << ops
               << " FROST_TEXT_CHECK_EVERY=" << checkEvery << " text_tests --gtest_filter=TextDifferential.Quick";
    }
    // Every op kind ran (on the default scale).
    if (!onlySeed && ops >= 1000) {
        EXPECT_GT(total.batches, 0u);
        EXPECT_GT(total.restores, 0u);
        EXPECT_GT(total.reloads, 0u);
        EXPECT_GT(total.rejected, 0u);
    }
}

} // namespace
