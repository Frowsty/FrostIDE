// SPDX-License-Identifier: Apache-2.0
// RUN-12 strict real-format value projection and source identity invalidation; authored, not run.
#include "frost/io/mem_fs.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/run/native_analysis.hpp"

#include <gtest/gtest.h>
namespace frost::run {
TEST(NativeCoverage, LiveAdmissionRevocationStopsAlreadyRunningOwnedProcess) {
    io::ScriptedProcessLauncher launcher;
    io::ProcessSpec command;
#ifdef _WIN32
    command.executable = "C:/tools/app.exe";
    command.workingDirectory = "C:/tools";
#else
    command.executable = "/tools/app";
    command.workingDirectory = "/tools";
#endif
    launcher.expect({.expectExecutable = command.executable, .neverExits = true});
    int checks = 0;
    auto result = runAnalysisCommand(
        launcher, command, {}, [] { return Result<void>{}; }, std::nullopt, 1024, {},
        [&]() -> Result<void> {
            ++checks;
            return fail(ErrorCode::PermissionDenied, "trust revoked while running");
        });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    EXPECT_GT(checks, 0);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1u);
    EXPECT_TRUE(launches.front().terminated);
}

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
#ifdef _WIN32
    constexpr auto projectRoot = "C:/project";
#else
    constexpr auto projectRoot = "/project";
#endif
    for (auto rows :
         {"DA:1,1\nDA:1,2\nLF:2\nLH:2\nend_of_record\n", "DA:1,1\nLF:1\nLH:0\nend_of_record\n", "DA:1,1\nLF:1\nLH:1\n",
          "DA:1,1\nBRDA:1,0,0,1\nBRDA:1,0,0,1\nLF:1\nLH:1\nBRF:2\nBRH:2\nend_of_record\n"})
        EXPECT_FALSE(parseLlvmCoverage(std::string("SF:unit.cpp\n") + rows, projectRoot));
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
