// SPDX-License-Identifier: Apache-2.0
//
// AnchorConcurrency.ReadersResolveDuringEdits: the unit-scale run of the anchor reader scenario in
// document_concurrent_readers.hpp (ADR 0017, decision 13). Sanitized trees run it as is; TSan runs
// it in the WSL -Full tree.

#include "text/document_concurrent_readers.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>

namespace {

using frost::text::test::DocumentConcurrentResult;
using frost::text::test::DocumentConcurrentRun;
using frost::text::test::runDocumentConcurrentReaders;

TEST(AnchorConcurrency, ReadersResolveDuringEdits) {
    for (const std::uint64_t seed : {1u, 2u}) {
        const DocumentConcurrentResult result = runDocumentConcurrentReaders(DocumentConcurrentRun{3'000, seed});
        ASSERT_TRUE(result.failure.empty()) << result.failure;
        std::printf("AnchorConcurrency seed %llu: %u readers, %llu publications, %llu reader iterations, %llu deep "
                    "checks\n",
                    static_cast<unsigned long long>(seed), result.readers,
                    static_cast<unsigned long long>(result.publications),
                    static_cast<unsigned long long>(result.readerIterations),
                    static_cast<unsigned long long>(result.deepChecks));
        EXPECT_GT(result.publications, 1000u);
        EXPECT_GE(result.readerIterations, result.readers);
    }
}

} // namespace
