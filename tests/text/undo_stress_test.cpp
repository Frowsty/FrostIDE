// SPDX-License-Identifier: Apache-2.0
//
// Stress tests for Document, anchors and the undo tree (label `stress`, ADR 0017; run in the
// configurations -Full adds: Release and ASan on Windows, ASan+UBSan and TSan in WSL).
/*
 * Scales keep every case under 120 s in each -Full configuration:
 *   - UndoStress.Differential: the UndoOracle differential (undo_ops.hpp) as seeds of 20k ops on up
 *     to four threads: 500k ops in optimised unsanitised builds, 200k under ASan, 100k under TSan
 *     and in Debug;
 *   - AnchorStress.ConcurrentReaders: the anchor reader scenario (document_concurrent_readers.hpp)
 *     with 500k writer ops optimised, 100k under ASan, 50k under TSan and in Debug.
 * Each test prints its scale and wall time. A failing differential seed prints the command that
 * re-runs it alone through UndoDifferential.Quick with the same settings (reproduceCommand).
 */

#include "support/test_env.hpp"
#include "text/document_concurrent_readers.hpp"
#include "text/support/undo_ops.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using frost::test::kOptimized;
using frost::test::kSanitized;
using frost::text::test::DocumentConcurrentResult;
using frost::text::test::DocumentConcurrentRun;
using frost::text::test::reproduceCommand;
using frost::text::test::runDocumentConcurrentReaders;
using frost::text::test::runUndoSeed;
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
