// SPDX-License-Identifier: Apache-2.0
//
// Stress tests for Document, anchors and the undo tree (label `stress`, ADR 0017; run in the
// configurations -Full adds: Release and ASan on Windows, ASan+UBSan and TSan in WSL).
/*
 * Scales keep every case under 120 s in each -Full configuration:
 *   - UndoStress.Differential: the UndoOracle differential (undo_ops.hpp) as seeds of 20k ops on up
 *     to four threads: 500k ops in optimised unsanitised builds, 200k under ASan, 100k under TSan
 *     and in Debug;
 *   - UndoStress.ManyAnchors: the same differential with 300 or 1500 tracked anchors plus up to
 *     200 or 2000 churn anchors (about 1600 live at most) and up to 400 carets, as seeds of 2500
 *     ops: 100k ops optimised, 25k under ASan, 20k under TSan and in Debug;
 *   - AnchorStress.ConcurrentReaders: the anchor reader scenario (document_concurrent_readers.hpp)
 *     with 500k writer ops optimised, 100k under ASan, 50k under TSan and in Debug.
 * Each test prints its scale and wall time. A failing differential seed prints the command that
 * re-runs it alone through UndoDifferential.Quick with the same settings (reproduceCommand); a
 * failing ManyAnchors seed prints its seed and anchor settings.
 */

#include "support/test_env.hpp"
#include "text/document_concurrent_readers.hpp"
#include "text/support/text_ops.hpp"
#include "text/support/undo_ops.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using frost::Result;
using frost::test::kOptimized;
using frost::test::kSanitized;
using frost::text::test::DocumentConcurrentResult;
using frost::text::test::DocumentConcurrentRun;
using frost::text::test::reproduceCommand;
using frost::text::test::runDocumentConcurrentReaders;
using frost::text::test::runUndoSeed;
using frost::text::test::SplitMixSource;
using frost::text::test::UndoDriver;
using frost::text::test::UndoDriverOptions;
using frost::text::test::undoSeedInitialText;
using frost::text::test::undoSeedOptions;
using frost::text::test::UndoSeedRun;

#if defined(__SANITIZE_THREAD__)
constexpr bool kThreadSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizer = true;
#else
constexpr bool kThreadSanitizer = false;
#endif
#else
constexpr bool kThreadSanitizer = false;
#endif

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

constexpr std::uint64_t kDifferentialOps = !kSanitized        ? (kOptimized ? 500'000 : 100'000)
                                           : kThreadSanitizer ? 100'000
                                                              : 200'000;
constexpr std::uint32_t kOpsPerSeed = 20'000;
constexpr std::uint32_t kConcurrentOps = !kSanitized        ? (kOptimized ? 500'000 : 50'000)
                                         : kThreadSanitizer ? 50'000
                                                            : 100'000;

TEST(UndoStress, Differential) {
    const std::uint64_t seeds = kDifferentialOps / kOpsPerSeed;
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    std::atomic<std::uint64_t> next{0};
    std::atomic<std::uint64_t> fullChecks{0};
    std::atomic<std::uint64_t> undos{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> maxNodes{0};
    std::mutex mutex;
    std::string firstFailure;
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::uint64_t i = next.fetch_add(1); i < seeds; i = next.fetch_add(1)) {
                // Seeds from 2'000'000 on, disjoint from UndoDifferential.Quick's 0..199.
                const UndoSeedRun run{.seed = 2'000'000 + i, .ops = kOpsPerSeed, .checkEvery = 64};
                const auto result = runUndoSeed(run);
                if (!result) {
                    const std::lock_guard lock(mutex);
                    if (firstFailure.empty()) {
                        firstFailure = result.error().message() + "\nReproduce with: " + reproduceCommand(run);
                    }
                    next.store(seeds); // Stop the other workers early.
                    return;
                }
                fullChecks.fetch_add(result->fullChecks, std::memory_order_relaxed);
                undos.fetch_add(result->undos, std::memory_order_relaxed);
                rejected.fetch_add(result->rejected, std::memory_order_relaxed);
                std::uint64_t seen = maxNodes.load(std::memory_order_relaxed);
                while (result->maxNodes > seen && !maxNodes.compare_exchange_weak(seen, result->maxNodes)) {
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::printf("UndoStress.Differential: %llu ops (%llu seeds x %u) on %u threads, %llu full checks, %llu undos, "
                "%llu rejected ops, most nodes %llu, %.1f s\n",
                static_cast<unsigned long long>(kDifferentialOps), static_cast<unsigned long long>(seeds), kOpsPerSeed,
                threads, static_cast<unsigned long long>(fullChecks.load()),
                static_cast<unsigned long long>(undos.load()), static_cast<unsigned long long>(rejected.load()),
                static_cast<unsigned long long>(maxNodes.load()), secondsSince(start));
    EXPECT_EQ(firstFailure, "");
}

/*
 * The differential with hundreds to about 1600 registry anchors and up to 400 carets, so that
 * Document's transforms and restores take every AnchorSet path (splices and per-move reinsertion as
 * well as sweeps and bulk rebuilds): the unit differential keeps at most 64 churn anchors, which the
 * measured path thresholds (anchor_paths.hpp) send to the sweep and bulk paths almost always.
 * Seeds of 2500 ops from 3,000,000; each prints its settings when it fails. The driver compares
 * every anchor after every op, so the scale is in ops x anchors.
 */
TEST(UndoStress, ManyAnchors) {
    constexpr std::uint64_t kOps = !kSanitized ? (kOptimized ? 100'000 : 20'000) : kThreadSanitizer ? 20'000 : 25'000;
    constexpr std::uint32_t kSeedOps = 2'500;
    const std::uint64_t seeds = kOps / kSeedOps;
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    std::atomic<std::uint64_t> next{0};
    std::atomic<std::uint64_t> undos{0};
    std::atomic<std::uint64_t> mostAnchors{0};
    std::mutex mutex;
    std::string firstFailure;
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::uint64_t i = next.fetch_add(1); i < seeds; i = next.fetch_add(1)) {
                const std::uint64_t seed = 3'000'000 + i;
                UndoDriverOptions options = undoSeedOptions(seed);
                options.maxOps = kSeedOps;
                options.checkEvery = 64;
                options.trackedAnchors = i % 2 == 0 ? 1500 : 300;
                options.maxChurnAnchors = i % 3 == 0 ? 2000 : 200;
                options.maxCarets = i % 4 == 0 ? 400 : 40;
                const std::string label =
                    std::format("many-anchors seed {} (tracked {}, churn {}, carets {})", seed, options.trackedAnchors,
                                options.maxChurnAnchors, options.maxCarets);
                UndoDriver driver(options, label, undoSeedInitialText(seed, options));
                SplitMixSource source(seed);
                Result<void> result{};
                while (!driver.done() && result) {
                    result = driver.step(source);
                }
                if (result) {
                    result = driver.verifyFull();
                }
                if (!result) {
                    const std::lock_guard lock(mutex);
                    if (firstFailure.empty()) {
                        firstFailure = result.error().message();
                    }
                    next.store(seeds);
                    return;
                }
                undos.fetch_add(driver.stats().undos, std::memory_order_relaxed);
                const std::uint64_t live = driver.document().anchors().size();
                std::uint64_t seen = mostAnchors.load(std::memory_order_relaxed);
                while (live > seen && !mostAnchors.compare_exchange_weak(seen, live)) {
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::printf("UndoStress.ManyAnchors: %llu ops (%llu seeds x %u) on %u threads, %llu undos, most live anchors at "
                "the end of a seed %llu, %.1f s\n",
                static_cast<unsigned long long>(kOps), static_cast<unsigned long long>(seeds), kSeedOps, threads,
                static_cast<unsigned long long>(undos.load()), static_cast<unsigned long long>(mostAnchors.load()),
                secondsSince(start));
    EXPECT_EQ(firstFailure, "");
}

TEST(AnchorStress, ConcurrentReaders) {
    const auto start = std::chrono::steady_clock::now();
    const DocumentConcurrentResult result = runDocumentConcurrentReaders(DocumentConcurrentRun{kConcurrentOps, 7});
    std::printf("AnchorStress.ConcurrentReaders: %u writer ops, %u readers, %llu publications, %llu reader "
                "iterations, %llu deep checks, %.1f s\n",
                kConcurrentOps, result.readers, static_cast<unsigned long long>(result.publications),
                static_cast<unsigned long long>(result.readerIterations),
                static_cast<unsigned long long>(result.deepChecks), secondsSince(start));
    EXPECT_EQ(result.failure, "");
}

} // namespace
