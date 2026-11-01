// SPDX-License-Identifier: Apache-2.0
//
// TextConcurrency.ReadersDuringEdits: one writer and up to four reader threads share snapshots of
// one TextBuffer while it is edited 20k times (concurrent_readers.hpp; ADR 0013, D7). The WSL
// -Full TSan tree runs it under ThreadSanitizer.

#include "text/concurrent_readers.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

namespace {

TEST(TextConcurrency, ReadersDuringEdits) {
    const auto start = std::chrono::steady_clock::now();
    const frost::text::test::ConcurrentResult result =
        frost::text::test::runConcurrentReaders({.edits = 20'000, .maxDocBytes = 16 * 1024, .seed = 1});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("TextConcurrency.ReadersDuringEdits: 20000 edits, %u readers, %llu reader iterations, %llu deep "
                "checks, %.2f s\n",
                result.readers, static_cast<unsigned long long>(result.readerIterations),
                static_cast<unsigned long long>(result.deepChecks), seconds);
    EXPECT_EQ(result.failure, "");
    EXPECT_GT(result.readerIterations, 0u);
}

} // namespace
