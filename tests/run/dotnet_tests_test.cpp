// SPDX-License-Identifier: Apache-2.0
// Checks .NET test commands, discovery identity, and bounded TRX parsing.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/run/dotnet_tests.hpp"

#include <gtest/gtest.h>

#include <algorithm>
namespace frost::run {
TEST(DotnetTests, AdapterDiscoveryPreservesOrderedRepeatedAndMultilineDisplayNames) {
    auto list = parseDotnetTestDiscovery(
        "Test run for X.dll\nThe following Tests are available:\r\n    Suite.Pass\r\n    Suite.Signature:\r\n"
        "System.Boolean Function[T](T)\r\n    Suite.Pass\r\n");
    ASSERT_TRUE(list);
    EXPECT_EQ(*list, (std::vector<std::string>{"Suite.Pass", "Suite.Signature:\nSystem.Boolean Function[T](T)",
                                               "Suite.Pass"}));
    EXPECT_FALSE(parseDotnetTestDiscovery("class FakeTest {}"));
    EXPECT_FALSE(parseDotnetTestDiscovery("The following Tests are available:\n    \n"));
}
TEST(DotnetTests, TrxReportsActualFailureIdentityDurationAndStack) {
    auto report = parseDotnetTrx(R"trx(<TestRun xmlns="http://microsoft.com/schemas/VisualStudio/TeamTest/2010">
        <Results>
            <UnitTestResult testId="id" executionId="exec" testName="Fail(1)" outcome="Failed" duration="00:00:01.1234567">
                <Output><ErrorInfo><Message>Expected 1</Message>
                    <StackTrace>at Suite.Fail in Test.cs:line 12</StackTrace></ErrorInfo></Output>
            </UnitTestResult>
        </Results>
        <TestDefinitions><UnitTest id="id"><TestMethod className="Suite" name="Fail"/></UnitTest></TestDefinitions>
        <ResultSummary outcome="Failed"/>
    </TestRun>)trx");
    ASSERT_TRUE(report);
    ASSERT_EQ(report->size(), 1u);
    EXPECT_EQ(report->front().fullyQualifiedName, "Suite.Fail");
    EXPECT_EQ(report->front().result.outcome, TestOutcome::Failed);
    EXPECT_EQ(report->front().result.duration, std::chrono::microseconds(1123456));
    ASSERT_FALSE(report->front().result.failures.empty());
    EXPECT_NE(report->front().result.failures.front().message.find("line 12"), std::string::npos);
    EXPECT_FALSE(parseDotnetTrx("<TestRun><Results/></TestRun>"));
}
TEST(DotnetTests, CommandsOwnOneFrameworkAndRejectFilterInjection) {
#ifdef _WIN32
    const std::string root = "C:/test";
#else
    const std::string root = "/test";
#endif
    DotnetTestRequest request;
    request.project = {root + "/dotnet", root + "/Tests.csproj", "Debug", "net10.0", {}};
    request.reportDirectory = root + "/report";
    request.selected = {"Suite.Pass"};
    auto spec = dotnetTestSpec(request);
    ASSERT_TRUE(spec);
    EXPECT_NE(std::find(spec->arguments.begin(), spec->arguments.end(), "--no-build"), spec->arguments.end());
    EXPECT_EQ(spec->arguments.back(), "FullyQualifiedName=Suite.Pass");
    request.discover = true;
    EXPECT_FALSE(dotnetTestSpec(request));
    request.discover = false;
    request.selected = {"Suite.Pass|FullyQualifiedName~Other"};
    EXPECT_FALSE(dotnetTestSpec(request));
    request.platform = DotnetTestPlatform::MicrosoftTestingPlatform;
    EXPECT_FALSE(dotnetTestSpec(request));
    request.selected.clear();
    ASSERT_TRUE(dotnetTestSpec(request));
}
TEST(DotnetTests, MalformedOrDuplicateDefinitionsNeverCreateMethodSelectionAuthority) {
    const auto report = [](const std::string& definitions) {
        return "<TestRun><Results><UnitTestResult testId=\"id\" executionId=\"exec\" testName=\"Display\" "
               "outcome=\"Passed\"/></Results><TestDefinitions>" +
               definitions + "</TestDefinitions><ResultSummary outcome=\"Completed\"/></TestRun>";
    };
    for (const auto* definition :
         {"<UnitTest id=\"id\"><TestMethod name=\"Pass\"/></UnitTest>",
          "<UnitTest id=\"id\"><TestMethod className=\"Suite\"/></UnitTest>",
          "<UnitTest><TestMethod className=\"Suite\" name=\"Pass\"/></UnitTest>",
          "<UnitTest id=\"id\"><TestMethod className=\"Suite\" name=\"Pass\"/></UnitTest><UnitTest "
          "id=\"id\"><TestMethod className=\"Other\" name=\"Wrong\"/></UnitTest>"})
        EXPECT_FALSE(parseDotnetTrx(report(definition)));
    const auto unknown = parseDotnetTrx(report(""));
    ASSERT_TRUE(unknown);
    ASSERT_EQ(unknown->size(), 1u);
    EXPECT_TRUE(unknown->front().fullyQualifiedName.empty());
    const auto noMethod = parseDotnetTrx(report("<UnitTest id=\"id\"/>"));
    ASSERT_TRUE(noMethod);
    EXPECT_TRUE(noMethod->front().fullyQualifiedName.empty());
}
TEST(DotnetTests, OversizedReportIsRefusedByTheBoundedFilesystemBoundary) {
    io::MemFs fs{systemClock()};
    const auto root = io::kNativePathStyle == io::PathStyle::Windows ? std::string("C:/bounded-report")
                                                                     : std::string("/bounded-report");
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.writeFileAtomic(root + "/results.trx", std::string(16 * 1024 * 1024 + 1, 'x')));
    const auto result = readDotnetTrxReport(fs, root + "/results.trx");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
}
} // namespace frost::run
