// SPDX-License-Identifier: Apache-2.0
//
// Stress tests for frost::text (label `stress`; skipped in the plain Debug configuration, run in
// the configurations -Full adds: Release and ASan on Windows, ASan+UBSan and TSan in WSL).
/*
 * M1 exit criterion (ADR 0012, PLAN.md section 7): 10M random ops of the piece-tree differential
 * in Release on both OSes, selected with FROST_STRESS_FULL=1 (Release only; sanitizer builds keep
 * their scaled counts). The default scale keeps every case under 120 s in each -Full
 * configuration:
 *   - TextStress.Differential: 1M ops in optimised unsanitised builds, fewer under sanitizers
 *     (kDifferentialOps below), as seeds of 20k ops run on up to four threads;
 *   - TextStress.F2SingleLine: a 50 MB single line (F2) loaded and edited 1000 times, 8 MB under
 *     sanitizers or in Debug;
 *   - TextStress.ConcurrentReaders: 1M edits with concurrent readers, fewer under sanitizers.
 * Each test prints its scale and wall time. A failing differential seed prints the command that
 * re-runs it alone with these settings (text_ops.hpp, reproduceCommand).
 */

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/text/text_summary.hpp"

#include "support/test_env.hpp"
#include "text/concurrent_readers.hpp"
#include "text/support/text_ops.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::LineIndex;
using frost::Result;
using frost::sched::detail::SplitMix64;
using frost::test::kOptimized;
using frost::test::kSanitized;
using frost::test::stressFull;
using frost::text::ByteRange;
using frost::text::Point;
using frost::text::TextBuffer;
using frost::text::TextChange;
using frost::text::test::SeedRun;

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

// Scales (measured; ADR 0012 and the progress log record the timings).
constexpr std::uint64_t kExitOps = 10'000'000;
constexpr std::uint64_t kDifferentialOps = !kSanitized        ? (kOptimized ? 1'000'000 : 200'000)
                                           : kThreadSanitizer ? 300'000
                                           : kOptimized       ? 500'000
                                                              : 200'000;
constexpr std::uint32_t kOpsPerSeed = 20'000;

TEST(TextStress, Differential) {
    const bool exitScale = stressFull() && kOptimized && !kSanitized;
    const std::uint64_t ops = exitScale ? kExitOps : kDifferentialOps;
    const std::uint64_t seeds = ops / kOpsPerSeed;
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    std::atomic<std::uint64_t> next{0};
    std::atomic<std::uint64_t> fullChecks{0};
    std::atomic<std::uint64_t> overLimit{0};
    std::atomic<std::uint64_t> maxBytes{0};
    std::mutex mutex;
    std::string firstFailure;
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::uint64_t i = next.fetch_add(1); i < seeds; i = next.fetch_add(1)) {
                // Seeds from 1'000'000 on, disjoint from TextDifferential.Quick's 0..199.
                const SeedRun run{.seed = 1'000'000 + i, .ops = kOpsPerSeed, .checkEvery = 16, .queriesPerCheck = 64};
                const auto result = frost::text::test::runSeed(run);
                if (!result) {
                    const std::lock_guard lock(mutex);
                    if (firstFailure.empty()) {
                        // Names every SeedRun field, so the reproduction makes the same checks.
                        firstFailure =
                            result.error().message() + "\nReproduce with: " + frost::text::test::reproduceCommand(run);
                    }
                    next.store(seeds); // Stop the other workers early.
                    return;
                }
                fullChecks.fetch_add(result->fullChecks, std::memory_order_relaxed);
                overLimit.fetch_add(result->overLimit, std::memory_order_relaxed);
                std::uint64_t seen = maxBytes.load(std::memory_order_relaxed);
                while (result->maxBytes > seen && !maxBytes.compare_exchange_weak(seen, result->maxBytes)) {
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::printf("TextStress.Differential: %llu ops (%llu seeds x %u, %s scale) on %u threads, %llu full checks, "
                "%llu rejections by the buffer limit, largest text %llu bytes, %.1f s\n",
                static_cast<unsigned long long>(ops), static_cast<unsigned long long>(seeds), kOpsPerSeed,
                exitScale ? "exit" : "default", threads, static_cast<unsigned long long>(fullChecks.load()),
                static_cast<unsigned long long>(overLimit.load()), static_cast<unsigned long long>(maxBytes.load()),
                secondsSince(start));
    EXPECT_EQ(firstFailure, "");
}

TEST(TextStress, F2SingleLine) {
    const std::size_t lineBytes = (kOptimized && !kSanitized) ? 50'000'000 : 8'000'000;
    constexpr int kEdits = 1000;
    SplitMix64 rng(50);
    // A single line of mixed-width characters, no breaks.
    constexpr std::string_view kTokens[] = {
        "a", "b", " ", "\t", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", std::string_view("\0", 1)};
    std::string mirror;
    mirror.reserve(lineBytes + 64);
    while (mirror.size() < lineBytes) {
        mirror += kTokens[rng.below(std::size(kTokens))];
    }
    const auto start = std::chrono::steady_clock::now();
    Result<TextBuffer> loaded = TextBuffer::fromUtf8(mirror);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    TextBuffer& buffer = *loaded;
    const double loadSeconds = secondsSince(start);
    const auto floorChar = [&](std::uint64_t x) {
        while (x > 0 && x < mirror.size() && (static_cast<unsigned char>(mirror[x]) & 0xC0u) == 0x80u) {
            --x;
        }
        return x;
    };
    for (int edit = 1; edit <= kEdits; ++edit) {
        const std::uint64_t n = mirror.size();
        const std::uint64_t a = floorChar(rng.below(n + 1));
        const std::uint64_t b = floorChar(std::min(n, a + rng.below(24)));
        std::string text;
        for (std::uint64_t k = rng.below(12); k > 0; --k) {
            text += kTokens[rng.below(std::size(kTokens))];
        }
        if (a == b && text.empty()) {
            text = "x";
        }
        const Result<TextChange> change = buffer.replace(ByteRange{ByteOffset{a}, ByteOffset{b}}, text);
        ASSERT_TRUE(change.has_value()) << change.error().describe();
        mirror.replace(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a), text);
        const auto& s = buffer.current();
        // Windowed checks: the edited bytes, the single line's points and the UTF-16 count there.
        const std::uint64_t lo = a >= 16 ? a - 16 : 0;
        const std::uint64_t hi = std::min<std::uint64_t>(mirror.size(), a + text.size() + 16);
        ASSERT_EQ(s.size().value(), mirror.size()) << "edit " << edit;
        ASSERT_EQ(s.text(ByteRange{ByteOffset{lo}, ByteOffset{hi}}), mirror.substr(lo, hi - lo)) << "edit " << edit;
        ASSERT_EQ(s.lineCount(), 1u);
        ASSERT_EQ(s.pointAt(ByteOffset{a}), (Point{LineIndex{0}, frost::ByteColumn{static_cast<std::uint32_t>(a)}}));
        ASSERT_EQ(change->startPoint.line, LineIndex{0});
        ASSERT_EQ(change->newEndPoint.column.value(), a + text.size());
        ASSERT_EQ(s.lineInfo(LineIndex{0})->content.end.value(), mirror.size());
        if (edit % 100 == 0 || edit == kEdits) {
            ASSERT_EQ(s.text(), mirror) << "edit " << edit;
            ASSERT_EQ(s.summary(), frost::text::summarize(mirror)) << "edit " << edit;
            ASSERT_EQ(s.summary().longestLineChars, s.summary().chars);
        }
    }
    ASSERT_TRUE(buffer.checkInvariants().has_value());
    std::printf("TextStress.F2SingleLine: %zu-byte line, load %.3f s, %d edits, %.1f s in total\n", lineBytes,
                loadSeconds, kEdits, secondsSince(start));
}

TEST(TextStress, ConcurrentReaders) {
    const std::uint32_t edits = (kOptimized && !kSanitized) ? 1'000'000 : kThreadSanitizer ? 100'000 : 200'000;
    const auto start = std::chrono::steady_clock::now();
    const frost::text::test::ConcurrentResult result =
        frost::text::test::runConcurrentReaders({.edits = edits, .maxDocBytes = 16 * 1024, .seed = 7});
    std::printf("TextStress.ConcurrentReaders: %u edits, %u readers, %llu reader iterations, %llu deep checks, "
                "%.1f s\n",
                edits, result.readers, static_cast<unsigned long long>(result.readerIterations),
                static_cast<unsigned long long>(result.deepChecks), secondsSince(start));
    EXPECT_EQ(result.failure, "");
}

} // namespace
