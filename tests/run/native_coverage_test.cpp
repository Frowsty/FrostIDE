// SPDX-License-Identifier: Apache-2.0
// RUN-12 strict real-format value projection and source identity invalidation; authored, not run.
#include "frost/io/mem_fs.hpp"
#include "frost/run/native_analysis.hpp"

#include <gtest/gtest.h>
namespace frost::run {
TEST(NativeCoverage, PreservesActualLinesAndUnknownBranches) {
    auto report = parseLlvmCoverage(
        "SF:unit.cpp\nDA:2,13\nDA:7,0\nBRDA:2,0,0,13\nBRDA:2,0,1,-\nLF:2\nLH:1\nBRF:2\nBRH:1\nend_of_record\n",
#ifdef _WIN32
        "C:/project"
#else
        "/project"
#endif
    );
    ASSERT_TRUE(report);
    ASSERT_EQ(report->size(), 1u);
    EXPECT_EQ(report->front().lines, (std::vector<CoverageLine>{{2, 13}, {7, 0}}));
    ASSERT_EQ(report->front().branches.size(), 2u);
    EXPECT_EQ(report->front().branches[0].hits, 13u);
    EXPECT_FALSE(report->front().branches[1].hits);
}
TEST(NativeCoverage, RefusesDuplicateContradictoryAndTruncatedRecords) {
    for (auto rows :
         {"DA:1,1\nDA:1,2\nLF:2\nLH:2\nend_of_record\n", "DA:1,1\nLF:1\nLH:0\nend_of_record\n", "DA:1,1\nLF:1\nLH:1\n",
          "DA:1,1\nBRDA:1,0,0,1\nBRDA:1,0,0,1\nLF:1\nLH:1\nBRF:2\nBRH:2\nend_of_record\n"})
        EXPECT_FALSE(parseLlvmCoverage(std::string("SF:unit.cpp\n") + rows,
#ifdef _WIN32
                                       "C:/project"
#else
                                       "/project"
#endif
                                       ));
}
TEST(NativeCoverage, VersionRequiresExactLlvmToken) {
    EXPECT_TRUE(validateAnalysisLlvmVersion("LLVM (https://llvm.org):\n LLVM version 23.1.4\n", "23.1.4"));
    EXPECT_FALSE(validateAnalysisLlvmVersion("LLVM version 23.1.40\n", "23.1.4"));
    EXPECT_FALSE(validateAnalysisLlvmVersion("tool23.1.4", "23.1.4"));
}
TEST(NativeCoverage, SavedSourceChangeInvalidatesIdentity) {
    ManualClock clock;
    io::MemFs fs(clock);
#ifdef _WIN32
    const std::string source = "C:/source.cpp";
#else
    const std::string source = "/source.cpp";
#endif
    ASSERT_TRUE(fs.writeFileAtomic(source, "int value=1;\n"));
    auto identity = captureAnalysisIdentity(fs, source);
    ASSERT_TRUE(identity);
    EXPECT_TRUE(validateAnalysisIdentity(fs, *identity));
    ASSERT_TRUE(fs.writeFileAtomic(source, "int value=2;\n"));
    EXPECT_FALSE(validateAnalysisIdentity(fs, *identity));
}
} // namespace frost::run
