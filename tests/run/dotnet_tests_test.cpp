// SPDX-License-Identifier: Apache-2.0
#include "frost/run/dotnet_tests.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
#include <algorithm>
namespace frost::run {
TEST(DotnetTests, AdapterDiscoveryIsAuthoritativeAndDuplicateNamesRefuse) {
    auto list = parseDotnetTestDiscovery("Test run for X.dll\nThe following Tests are available:\r\n    Suite.Pass\r\n    Suite.Fail\r\n"); ASSERT_TRUE(list); ASSERT_EQ(list->size(),2u); EXPECT_EQ(list->at(1),"Suite.Fail");
    EXPECT_FALSE(parseDotnetTestDiscovery("class FakeTest {}"));
    EXPECT_FALSE(parseDotnetTestDiscovery("The following Tests are available:\n    Suite.Same\n    Suite.Same\n"));
}
TEST(DotnetTests, TrxReportsActualFailureIdentityDurationAndStack) {
    auto report = parseDotnetTrx(R"(<TestRun xmlns="http://microsoft.com/schemas/VisualStudio/TeamTest/2010"><Results><UnitTestResult testId="id" executionId="exec" testName="Fail(1)" outcome="Failed" duration="00:00:01.1234567"><Output><ErrorInfo><Message>Expected 1</Message><StackTrace>at Suite.Fail in Test.cs:line 12</StackTrace></ErrorInfo></Output></UnitTestResult></Results><TestDefinitions><UnitTest id="id"><TestMethod className="Suite" name="Fail"/></UnitTest></TestDefinitions><ResultSummary outcome="Failed"/></TestRun>)");
    ASSERT_TRUE(report); ASSERT_EQ(report->size(),1u); EXPECT_EQ(report->front().fullyQualifiedName,"Suite.Fail"); EXPECT_EQ(report->front().result.outcome,TestOutcome::Failed); EXPECT_EQ(report->front().result.duration,std::chrono::microseconds(1123456)); EXPECT_NE(report->front().result.failures.front().message.find("line 12"),std::string::npos);
    EXPECT_FALSE(parseDotnetTrx("<TestRun><Results/></TestRun>"));
}
TEST(DotnetTests, CommandsOwnOneFrameworkAndRejectFilterInjection) {
#ifdef _WIN32
    const std::string root="C:/test";
#else
    const std::string root="/test";
#endif
    DotnetTestRequest request; request.project={root+"/dotnet",root+"/Tests.csproj","Debug","net10.0",{}}; request.reportDirectory=root+"/report"; request.selected={"Suite.Pass"};
    auto spec=dotnetTestSpec(request); ASSERT_TRUE(spec); EXPECT_NE(std::find(spec->arguments.begin(),spec->arguments.end(),"--no-build"),spec->arguments.end()); EXPECT_EQ(spec->arguments.back(),"FullyQualifiedName=Suite.Pass");
    request.selected={"Suite.Pass|FullyQualifiedName~Other"}; EXPECT_FALSE(dotnetTestSpec(request));
    request.platform=DotnetTestPlatform::MicrosoftTestingPlatform; EXPECT_FALSE(dotnetTestSpec(request)); request.selected.clear(); ASSERT_TRUE(dotnetTestSpec(request));
}
}
