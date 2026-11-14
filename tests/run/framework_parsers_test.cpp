// SPDX-License-Identifier: Apache-2.0
// Authored protocol specimens from the upstream reporter schemas, not claimed runtime captures.
#include "frost/run/framework_parsers.hpp"
#include "frost/run/test_runner.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
#include <algorithm>
namespace frost::run {
namespace {
std::string catchReport(std::string_view cases, std::string_view counts) {
    return "<Catch2TestRun catch2-version=\"3.7.1\" xml-format-version=\"3\">" + std::string(cases) +
        "<OverallResultsCases " + std::string(counts) + "/></Catch2TestRun>";
}
const std::string boostLog = "<TestLog><BuildInfo boost=\"1.85.0\"/><TestSuite name=\"module\"><TestCase name=\"passes\" file=\"real.cpp\" line=\"8\"><TestingTime>17</TestingTime></TestCase></TestSuite></TestLog>";
TEST(FrameworkProtocol, CatchListingUsesActualNameAndLocationWithoutOutcomes) {
    auto list = parseFrameworkList(FrameworkProfile::Catch2Xml3,
        "<MatchingTests><TestCase><Name>a &amp; b</Name><Tags>[math]</Tags><SourceInfo><File>real.cpp</File><Line>12</Line></SourceInfo></TestCase></MatchingTests>");
    ASSERT_TRUE(list); ASSERT_EQ(list->size(), 1U);
    EXPECT_EQ(list->front().name, "a & b"); ASSERT_TRUE(list->front().location);
    EXPECT_EQ(list->front().location->line, 12);
    EXPECT_FALSE(parseFrameworkList(FrameworkProfile::Catch2Xml3, "All tests passed"));
}
TEST(FrameworkProtocol, CatchFailureDominatesSkippedSectionsAndCountersMustAccountForEveryCase) {
    const auto xml = catchReport("<TestCase name=\"mixed\"><Section><Failure filename=\"real.cpp\" line=\"7\">bad</Failure></Section><OverallResult success=\"false\" skips=\"1\"/></TestCase>",
        "successes=\"0\" failures=\"1\" expectedFailures=\"0\" skips=\"0\"");
    auto parsed = parseFrameworkRun(FrameworkProfile::Catch2Xml3, "binary", xml, {}, {}, 1, {});
    ASSERT_TRUE(parsed); ASSERT_EQ(parsed->cases.size(), 1U);
    EXPECT_EQ(parsed->cases.front().outcome, TestOutcome::Failed);
    EXPECT_EQ(parsed->outcome, TestOutcome::Failed);
    EXPECT_FALSE(parsed->cases.front().failures.empty());
    EXPECT_FALSE(parseFrameworkRun(FrameworkProfile::Catch2Xml3, "binary",
        catchReport("<TestCase name=\"only\"><OverallResult success=\"true\" skips=\"0\"/></TestCase>",
            "successes=\"2\" failures=\"0\" expectedFailures=\"0\" skips=\"0\""), {}, {}, 0, {}));
    EXPECT_FALSE(parseFrameworkRun(FrameworkProfile::Catch2Xml3, "binary", "<Catch2TestRun/>", {}, {}, 0, {}));
}
TEST(FrameworkProtocol, LiteralSelectionRetainsSplitOptionValuesAndListFlagsNeverConsumeFilters) {
    const std::vector<std::string> selected{"wanted case"};
    auto run = frameworkArguments(FrameworkProfile::Catch2Xml3,
        {"program", "--rng-seed", "123", "--order", "lex", "old case"}, selected, "report.xml", false);
    ASSERT_TRUE(run);
    EXPECT_EQ((*run)[0], "--rng-seed"); EXPECT_EQ((*run)[1], "123");
    EXPECT_EQ((*run)[2], "--order"); EXPECT_EQ((*run)[3], "lex");
    EXPECT_NE(std::ranges::find(*run, "\"wanted case\""), run->end());
    EXPECT_EQ(std::ranges::find(*run, "old case"), run->end());
    auto list = frameworkArguments(FrameworkProfile::Catch2Xml3, {"program", "--list-tests", "baseline"}, {}, "list.xml", true);
    ASSERT_TRUE(list); EXPECT_NE(std::ranges::find(*list, "baseline"), list->end());
    EXPECT_FALSE(frameworkArguments(FrameworkProfile::Catch2Xml3, {"program", "--unknown", "123"}, selected, "report", false));
    EXPECT_FALSE(frameworkArguments(FrameworkProfile::Catch2Xml3, {"program"}, std::vector<std::string>{"wild*"}, "report", false));
}
std::string dot() {
    return "digraph G {rankdir=LR;\ntu0[shape=ellipse,peripheries=2,fontname=Helvetica,color=green,label=\"module\"];\n{\n"
        "tu1[shape=Mrecord,fontname=Helvetica,color=green,label=\"suite|file.cpp(4)\"];\ntu0 -> tu1;\n{\n"
        "tu2[shape=Mrecord,fontname=Helvetica,color=green,label=\"passes|file.cpp(8)\"];\ntu1 -> tu2;\n}\n}\n}\n";
}
TEST(FrameworkProtocol, BoostDotDistinguishesSuitesFromCasesAndRejectsTruncation) {
    const auto text = dot(); auto list = parseFrameworkList(FrameworkProfile::BoostTest185, text);
    ASSERT_TRUE(list); ASSERT_EQ(list->size(), 1U); EXPECT_EQ(list->front().name, "suite/passes");
    ASSERT_TRUE(list->front().location); EXPECT_EQ(list->front().location->line, 8);
    EXPECT_FALSE(parseFrameworkList(FrameworkProfile::BoostTest185, text.substr(0, text.size() - 3)));
    EXPECT_FALSE(parseFrameworkList(FrameworkProfile::BoostTest185, text + "unexpected"));
}
TEST(FrameworkProtocol, BoostUsesAuthoritativeVerdictsAndRetainsUnexpandedSkippedSuites) {
    auto parsed = parseFrameworkRun(FrameworkProfile::BoostTest185, "binary",
        "<TestResult><TestSuite name=\"module\" result=\"passed\"><TestCase name=\"passes\" result=\"passed\"/><TestSuite name=\"disabled\" result=\"skipped\" test_cases_skipped=\"2\"/></TestSuite></TestResult>",
        boostLog, {}, 0, {});
    ASSERT_TRUE(parsed); ASSERT_EQ(parsed->cases.size(), 2U);
    EXPECT_EQ(parsed->cases[0].name, "passes"); EXPECT_EQ(parsed->cases[0].duration, std::chrono::microseconds(17));
    EXPECT_EQ(parsed->cases[1].name, "disabled"); EXPECT_EQ(parsed->cases[1].outcome, TestOutcome::Skipped);
    EXPECT_NE(parsed->cases[1].reason.find("no child outcomes"), std::string::npos);
    EXPECT_FALSE(parseFrameworkRun(FrameworkProfile::BoostTest185, "binary",
        "<TestResult><TestSuite name=\"module\" result=\"passed\"><TestCase name=\"passes\" result=\"passed\"/></TestSuite></TestResult>",
        "<TestLog><BuildInfo boost=\"1.84.0\"/></TestLog>", {}, 0, {}));
}
TEST(FrameworkProtocol, ExactReportMembershipAndFailedExitNeverBecomePasses) {
    const auto xml = catchReport("<TestCase name=\"other\"><OverallResult success=\"true\" skips=\"0\"/></TestCase>",
        "successes=\"1\" failures=\"0\" expectedFailures=\"0\" skips=\"0\"");
    EXPECT_FALSE(parseFrameworkRun(FrameworkProfile::Catch2Xml3, "binary", xml, {}, std::vector<std::string>{"wanted"}, 0, {}));
    EXPECT_FALSE(parseFrameworkRun(FrameworkProfile::Catch2Xml3, "binary", xml, {}, {}, 1, {}));
}
TEST(FrameworkProtocol, DebugLaunchUsesSameLiteralFilterWithoutCreatingReportFiles) {
    CTestSetup setup; setup.buildDirectory = io::kNativePathStyle == io::PathStyle::Windows ? "C:/build" : "/build";
    DiscoveredTest test; test.command = {io::kNativePathStyle == io::PathStyle::Windows ? "C:/bin/tests.exe" : "/bin/tests"};
    auto launch = buildSingleTestLaunch(setup, test, "suite/case", TestCaseFramework::BoostTest);
    ASSERT_TRUE(launch); EXPECT_EQ(launch->arguments, (std::vector<std::string>{"--run_test=suite/case"}));
}
}
}
