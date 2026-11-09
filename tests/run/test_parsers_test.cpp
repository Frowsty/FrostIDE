// SPDX-License-Identifier: Apache-2.0
//
// The parsers of the test runner (RUN-7): ctest's json-v1 test list, its JUnit report and progress
// lines (ctest_parsers.hpp), Google Test's case list and JSON report and how reports attach to
// ctest results (gtest_parsers.hpp), and the pure parts of test_runner.hpp (ctestSetupFor,
// testPresetFor, the discovery arguments, the exact-name regex). The inputs are outputs the pinned
// ctest 4.4.5 and Google Test 1.18 printed for tests/run/fixtures/ctest_project and run_gtest_fixture,
// with their paths shortened; test_runner_test.cpp drives the real tools.

#include "frost/project/cmake_file_api.hpp"
#include "frost/project/cmake_presets.hpp"
#include "frost/project/cmake_profile.hpp"
#include "frost/run/ctest_parsers.hpp"
#include "frost/run/gtest_parsers.hpp"
#include "frost/run/test_runner.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::run::TestOutcome;

constexpr std::string_view kShowOnly = R"({
  "backtraceGraph" : { "commands" : [ "add_test" ], "files" : [ "E:/fixture/CMakeLists.txt" ],
    "nodes" : [ { "file" : 0 }, { "command" : 0, "file" : 0, "line" : 4, "parent" : 0 },
                { "file" : 0 }, { "command" : 0, "file" : 0, "line" : 6, "parent" : 2 } ] },
  "kind" : "ctestInfo",
  "tests" : [
    { "backtrace" : 1, "command" : [ "C:/tools/cmake/bin/cmake.exe", "-E", "true" ], "name" : "passes",
      "properties" : [ { "name" : "WORKING_DIRECTORY", "value" : "E:/fixture/b" } ] },
    { "backtrace" : 3, "command" : [ "C:/tools/cmake/bin/cmake.exe", "-E", "false" ], "name" : "skipped",
      "properties" : [ { "name" : "LABELS", "value" : [ "core", "fast" ] },
                       { "name" : "SKIP_RETURN_CODE", "value" : 1 },
                       { "name" : "WORKING_DIRECTORY", "value" : "E:/fixture/b" } ] },
    { "backtrace" : 3, "name" : "missing", "properties" : [ { "name" : "DISABLED", "value" : true } ] },
    { "command" : [ "E:/b/run_unit_tests.exe", "--gtest_filter=RunConfiguration.Validation",
                    "--gtest_also_run_disabled_tests" ],
      "name" : "RunConfiguration.Validation",
      "properties" : [ { "name" : "DEF_SOURCE_LINE", "value" : "E:/src/tests/run/run_configuration_test.cpp:79" },
                       { "name" : "LABELS", "value" : "unit" } ] }
  ],
  "version" : { "major" : 1, "minor" : 0 }
})";

TEST(CTestParsers, ShowOnlyListsEveryTestWithItsProperties) {
    auto tests = frost::run::parseCTestShowOnly(kShowOnly);
    ASSERT_TRUE(tests.has_value()) << tests.error().describe();
    ASSERT_EQ(tests->size(), 4U);
    const auto& passes = (*tests)[0];
    EXPECT_EQ(passes.name, "passes");
    EXPECT_EQ(passes.number, 1U);
    EXPECT_EQ(passes.command, (std::vector<std::string>{"C:/tools/cmake/bin/cmake.exe", "-E", "true"}));
    EXPECT_EQ(passes.workingDirectory, "E:/fixture/b");
    ASSERT_TRUE(passes.declaration.has_value());
    EXPECT_EQ(passes.declaration->file, "E:/fixture/CMakeLists.txt");
    EXPECT_EQ(passes.declaration->line, 4);
    EXPECT_FALSE(passes.definition.has_value());

    const auto& skipped = (*tests)[1];
    EXPECT_EQ(skipped.labels, (std::vector<std::string>{"core", "fast"}));
    EXPECT_EQ(skipped.declaration->line, 6);

    const auto& missing = (*tests)[2];
    EXPECT_TRUE(missing.command.empty()) << "ctest found no program";
    EXPECT_TRUE(missing.disabled);
    EXPECT_EQ(missing.number, 3U);

    const auto& gtest = (*tests)[3];
    EXPECT_EQ(gtest.gtestFilter, "RunConfiguration.Validation");
    EXPECT_EQ(gtest.labels, (std::vector<std::string>{"unit"}));
    ASSERT_TRUE(gtest.definition.has_value());
    EXPECT_EQ(gtest.definition->file, "E:/src/tests/run/run_configuration_test.cpp");
    EXPECT_EQ(gtest.definition->line, 79);
    EXPECT_FALSE(gtest.declaration.has_value());
}

TEST(CTestParsers, ShowOnlySkipsALeadingLineAndRefusesWhatIsNoList) {
    auto withBanner = frost::run::parseCTestShowOnly(std::string("Some notice\n") + std::string(kShowOnly));
    ASSERT_TRUE(withBanner.has_value()) << withBanner.error().describe();
    EXPECT_EQ(withBanner->size(), 4U);

    for (const std::string_view bad :
         {std::string_view{}, std::string_view{"No tests were found!!!\n"}, std::string_view{"{\"kind\": \"x\"}"},
          std::string_view{R"({"kind": "ctestInfo", "version": {"major": 2}, "tests": []})"},
          std::string_view{R"({"kind": "ctestInfo", "version": {"major": 1}, "tests": [{"command": []}]})"}}) {
        auto parsed = frost::run::parseCTestShowOnly(bad);
        ASSERT_FALSE(parsed.has_value()) << bad;
        EXPECT_EQ(parsed.error().code(), frost::ErrorCode::Corrupt) << bad;
    }
    auto empty = frost::run::parseCTestShowOnly(R"({"kind": "ctestInfo", "version": {"major": 1}, "tests": []})");
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
}

constexpr std::string_view kJUnit = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="(empty)"
	tests="7" failures="2" disabled="1" skipped="2" hostname="" time="1" timestamp="2026-11-08T12:21:47" >
	<testcase name="passes" classname="passes" time="0.0147974" status="run">
		<properties/>
		<system-out></system-out>
	</testcase>
	<testcase name="fails" classname="fails" time="0.0152775" status="fail">
		<failure message="Failed"/>
		<properties/>
		<system-out></system-out>
	</testcase>
	<testcase name="skipped" classname="skipped" time="0.0144679" status="notrun">
		<skipped message="SKIP_RETURN_CODE=1"/>
		<properties><property name="cmake_labels" value="core;fast"/></properties>
		<system-out></system-out>
	</testcase>
	<testcase name="disabled" classname="disabled" time="0" status="disabled">
		<properties/>
		<system-out>Disabled</system-out>
	</testcase>
	<testcase name="has spaces (and regex+chars)" classname="has spaces (and regex+chars)" time="0.0137613" status="run">
		<properties/>
		<system-out>hello &lt;out&gt; &amp; stuff
</system-out>
	</testcase>
	<testcase name="missing" classname="missing" time="0" status="notrun">
		<skipped message="Unable to find executable"/>
		<properties/>
		<system-out>Unable to find executable: does_not_exist_program</system-out>
	</testcase>
	<testcase name="slow" classname="slow" time="1.01512" status="fail">
		<failure message="Timeout"/>
		<properties/>
		<system-out></system-out>
	</testcase>
	<testcase name="future" classname="future" time="0" status="something-new"/>
</testsuite>)xml";

TEST(CTestParsers, JUnitReportGivesCTestsVerdicts) {
    auto results = frost::run::parseCTestJUnit(kJUnit);
    ASSERT_TRUE(results.has_value()) << results.error().describe();
    ASSERT_EQ(results->size(), 7U) << "the unknown status gives no result";
    const auto at = [&](std::string_view name) {
        const auto found = std::ranges::find(*results, name, &frost::run::TestResult::name);
        EXPECT_NE(found, results->end()) << name;
        return *found;
    };
    EXPECT_EQ(at("passes").outcome, TestOutcome::Passed);
    EXPECT_EQ(at("passes").duration, std::chrono::microseconds(14797));
    EXPECT_TRUE(at("passes").reason.empty());
    EXPECT_FALSE(at("passes").interim);
    EXPECT_EQ(at("fails").outcome, TestOutcome::Failed);
    EXPECT_EQ(at("fails").reason, "Failed");
    EXPECT_EQ(at("skipped").outcome, TestOutcome::Skipped);
    EXPECT_EQ(at("skipped").reason, "SKIP_RETURN_CODE=1");
    EXPECT_EQ(at("disabled").outcome, TestOutcome::Disabled);
    EXPECT_TRUE(at("disabled").output.empty()) << "ctest's 'Disabled' placeholder is not test output";
    EXPECT_EQ(at("has spaces (and regex+chars)").output, "hello <out> & stuff\n");
    EXPECT_EQ(at("missing").outcome, TestOutcome::NotRun);
    EXPECT_EQ(at("missing").reason, "Unable to find executable");
    EXPECT_TRUE(frost::run::countsAsFailure(at("missing").outcome));
    EXPECT_EQ(at("slow").outcome, TestOutcome::Failed);
    EXPECT_EQ(at("slow").reason, "Timeout");
    EXPECT_EQ(at("slow").duration, std::chrono::microseconds(1015120));

    for (const std::string_view bad :
         {std::string_view{}, std::string_view{"<testsuites/>"}, std::string_view{"<testsuite><testcase"}}) {
        auto parsed = frost::run::parseCTestJUnit(bad);
        ASSERT_FALSE(parsed.has_value()) << bad;
        EXPECT_EQ(parsed.error().code(), frost::ErrorCode::Corrupt);
    }
}

TEST(CTestParsers, ProgressLinesGiveInterimResults) {
    using Kind = frost::run::CTestProgressLine::Kind;
    const auto started = frost::run::parseCTestProgressLine("    Start 5: has spaces (and regex+chars)\r");
    ASSERT_TRUE(started.has_value());
    EXPECT_EQ(started->kind, Kind::Started);
    EXPECT_EQ(started->number, 5U);
    EXPECT_EQ(started->text, "has spaces (and regex+chars)");
    EXPECT_FALSE(frost::run::ctestProgressResult(*started, started->text).has_value());

    const auto interim = [](std::string_view line, std::string_view name) {
        const auto parsed = frost::run::parseCTestProgressLine(line);
        EXPECT_TRUE(parsed.has_value()) << line;
        return parsed ? frost::run::ctestProgressResult(*parsed, name) : std::nullopt;
    };
    auto passed =
        interim("5/7 Test #5: has spaces (and regex+chars) .....   Passed    0.01 sec", "has spaces (and regex+chars)");
    ASSERT_TRUE(passed.has_value());
    EXPECT_EQ(passed->outcome, TestOutcome::Passed);
    EXPECT_EQ(passed->duration, 10ms);
    EXPECT_TRUE(passed->interim);
    auto failed = interim("2/7 Test #2: fails ............................***Failed    0.02 sec", "fails");
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->outcome, TestOutcome::Failed);
    EXPECT_EQ(failed->reason, "Failed");
    EXPECT_EQ(interim("3/7 Test #3: skipped ..........................***Skipped   0.01 sec", "skipped")->outcome,
              TestOutcome::Skipped);
    EXPECT_EQ(
        interim("4/7 Test #4: disabled .........................***Not Run (Disabled)   0.00 sec", "disabled")->outcome,
        TestOutcome::Disabled);
    EXPECT_EQ(interim("6/7 Test #6: missing ..........................***Not Run   0.00 sec", "missing")->outcome,
              TestOutcome::NotRun);
    auto timeout = interim("7/7 Test #7: slow .............................***Timeout   1.02 sec", "slow");
    EXPECT_EQ(timeout->outcome, TestOutcome::Failed);
    EXPECT_EQ(timeout->reason, "Timeout");
    // ctest cuts a name longer than its width (30 + 4) and puts the status right after it.
    const std::string longName = "WorkbenchClangdSpawn.APresetSwitchRestartsClangdOnce";
    auto cut = interim("12/40 Test #12: WorkbenchClangdSpawn.APresetSwi***Failed    3.50 sec", longName);
    ASSERT_TRUE(cut.has_value());
    EXPECT_EQ(cut->name, longName);
    EXPECT_EQ(cut->outcome, TestOutcome::Failed);
    EXPECT_TRUE(interim("13/40 Test #13: WorkbenchClangdSpawn.APresetSwit   Passed    1.00 sec", longName).has_value());

    // Lines of another test, or that are no progress at all.
    EXPECT_FALSE(interim("2/7 Test #2: fails ........***Failed    0.02 sec", "passes").has_value());
    EXPECT_FALSE(interim("2/7 Test #2: failsafe .....   Passed    0.02 sec", "fails").has_value());
    for (const std::string_view other : {"Test project E:/fixture/b", "50% tests passed, 3 tests failed out of 6",
                                         "Total Test time (real) = 1.08 sec", "\t  2 - fails (Failed)", "Start", ""}) {
        EXPECT_FALSE(frost::run::parseCTestProgressLine(other).has_value()) << other;
    }
}

constexpr std::string_view kGTestList = "Running main() from C:\\tools\\gtest_main.cc\n"
                                        "Fixture.\n"
                                        "  Passes\n"
                                        "  Fails\n"
                                        "  DISABLED_Off\n"
                                        "Sizes/0.  # TypeParam = unsigned char\n"
                                        "  AreAtLeastOneByte\n"
                                        "Small/Numbers.\r\n"
                                        "  AreSmall/0  # GetParam() = 1\r\n";

TEST(GTestParsers, ListGivesEveryCaseWithItsParameters) {
    auto cases = frost::run::parseGTestList(kGTestList);
    ASSERT_TRUE(cases.has_value()) << cases.error().describe();
    std::vector<std::string> names;
    for (const auto& c : *cases)
        names.push_back(c.fullName());
    EXPECT_EQ(names, (std::vector<std::string>{"Fixture.Passes", "Fixture.Fails", "Fixture.DISABLED_Off",
                                               "Sizes/0.AreAtLeastOneByte", "Small/Numbers.AreSmall/0"}));
    EXPECT_EQ((*cases)[3].typeParam, "unsigned char");
    EXPECT_EQ((*cases)[4].valueParam, "1");
    EXPECT_TRUE((*cases)[0].typeParam.empty());

    auto empty = frost::run::parseGTestList("Running main() from gtest_main.cc\n");
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
    EXPECT_FALSE(frost::run::parseGTestList("Fixture.\nOther.\n  Case\n").has_value()) << "a suite without cases";
    EXPECT_FALSE(frost::run::parseGTestList("Fixture.\n  Case\nnot a list line\n").has_value());
}

constexpr std::string_view kGTestReport = R"({
  "tests": 5, "failures": 1, "disabled": 1, "errors": 0, "time": "0.002s", "name": "AllTests",
  "testsuites": [
    { "name": "Fixture", "tests": 4, "testsuite": [
        { "name": "Passes", "file": "E:\\src\\gtest_fixture.cpp", "line": 14, "status": "RUN",
          "result": "COMPLETED", "time": "0.001s", "classname": "Fixture" },
        { "name": "Fails", "file": "E:\\src\\gtest_fixture.cpp", "line": 16, "status": "RUN",
          "result": "COMPLETED", "time": "0s", "classname": "Fixture",
          "failures": [ { "failure": "E:\\src\\gtest_fixture.cpp:18\nExpected equality of these values:\n  2 + 2\n",
                          "type": "" },
                        { "failure": "unknown file\nC++ exception thrown", "type": "" } ] },
        { "name": "Skips", "status": "RUN", "result": "SKIPPED", "time": "0s",
          "skipped": [ { "message": "E:\\src\\gtest_fixture.cpp:21\nan intended skip\n" } ] },
        { "name": "DISABLED_Off", "status": "NOTRUN", "result": "SUPPRESSED", "time": "0s" } ] },
    { "name": "Small\/Numbers", "testsuite": [
        { "name": "AreSmall\/0", "value_param": "1", "status": "RUN", "result": "COMPLETED", "time": "0s" },
        { "name": "Listed", "file": "x.cpp", "line": 3 } ] }
  ]
})";

TEST(GTestParsers, JsonReportGivesEachCasesVerdictAndFailures) {
    auto cases = frost::run::parseGTestJsonReport(kGTestReport);
    ASSERT_TRUE(cases.has_value()) << cases.error().describe();
    ASSERT_EQ(cases->size(), 5U) << "the listing entry without a result is no result";
    EXPECT_EQ((*cases)[0].name, "Fixture.Passes");
    EXPECT_EQ((*cases)[0].outcome, TestOutcome::Passed);
    EXPECT_EQ((*cases)[0].duration, 1ms);
    ASSERT_TRUE((*cases)[0].definition.has_value());
    EXPECT_EQ((*cases)[0].definition->file, "E:\\src\\gtest_fixture.cpp");
    EXPECT_EQ((*cases)[0].definition->line, 14);
    EXPECT_FALSE((*cases)[2].definition.has_value()) << "the report gave no file for it";
    const auto& fails = (*cases)[1];
    EXPECT_EQ(fails.outcome, TestOutcome::Failed);
    ASSERT_EQ(fails.failures.size(), 2U);
    ASSERT_TRUE(fails.failures[0].location.has_value());
    EXPECT_EQ(fails.failures[0].location->file, "E:\\src\\gtest_fixture.cpp");
    EXPECT_EQ(fails.failures[0].location->line, 18);
    EXPECT_EQ(fails.failures[0].message, "Expected equality of these values:\n  2 + 2\n");
    EXPECT_FALSE(fails.failures[1].location.has_value());
    EXPECT_EQ(fails.failures[1].message, "unknown file\nC++ exception thrown");
    EXPECT_EQ((*cases)[2].outcome, TestOutcome::Skipped);
    EXPECT_EQ((*cases)[2].reason, "an intended skip");
    EXPECT_EQ((*cases)[3].outcome, TestOutcome::Disabled);
    EXPECT_EQ((*cases)[4].name, "Small/Numbers.AreSmall/0");

    EXPECT_FALSE(frost::run::parseGTestJsonReport("not json").has_value());
    EXPECT_FALSE(frost::run::parseGTestJsonReport("{}").has_value());
}

TEST(GTestParsers, ReportNamesAndProgramStems) {
    EXPECT_EQ(frost::run::gtestProgramStem("C:/b/run_tests.exe"), "run_tests");
    EXPECT_EQ(frost::run::gtestProgramStem("C:\\b\\Run.EXE"), "Run");
    EXPECT_EQ(frost::run::gtestProgramStem("/home/b/run_tests"), "run_tests");
    EXPECT_EQ(frost::run::gtestReportStem("run_tests.json"), "run_tests");
    EXPECT_EQ(frost::run::gtestReportStem("run_tests_12.json"), "run_tests");
    EXPECT_EQ(frost::run::gtestReportStem("run_unit_tests.json"), "run_unit_tests");
    EXPECT_EQ(frost::run::gtestReportStem("run_tests.xml"), "");
    EXPECT_EQ(frost::run::gtestReportStem(".json"), "");
}

TEST(GTestParsers, ReportsAttachToTheFilteredCaseOrToTheWholeProgram) {
    auto tests = frost::run::parseCTestShowOnly(R"({"kind": "ctestInfo", "version": {"major": 1}, "tests": [
        {"name": "whole", "command": ["C:/b/fixture.exe"]},
        {"name": "Fixture.Fails", "command": ["C:/b/fixture.exe", "--gtest_filter=Fixture.Fails"]},
        {"name": "twinA", "command": ["C:/b/twin.exe"]},
        {"name": "twinB", "command": ["C:/b/twin.exe", "--flag"]},
        {"name": "plain", "command": ["C:/b/cmake.exe", "-E", "true"]}]})");
    ASSERT_TRUE(tests.has_value());
    auto results = frost::run::parseCTestJUnit(R"(<testsuite>
        <testcase name="whole" time="0.1" status="fail"><failure message="Failed"/></testcase>
        <testcase name="Fixture.Fails" time="0.1" status="fail"><failure message="Failed"/></testcase>
        <testcase name="twinA" time="0.1" status="run"/>
        <testcase name="twinB" time="0.1" status="run"/>
        <testcase name="plain" time="0.1" status="run"/></testsuite>)");
    ASSERT_TRUE(results.has_value());
    auto wholeCases = frost::run::parseGTestJsonReport(kGTestReport);
    auto oneCase = frost::run::parseGTestJsonReport(R"({"testsuites": [{"name": "Fixture", "testsuite": [
        {"name": "Fails", "status": "RUN", "result": "COMPLETED", "time": "0s",
         "failures": [{"failure": "C:/src/f.cpp:18\nboom"}]}]}]})");
    ASSERT_TRUE(wholeCases.has_value());
    ASSERT_TRUE(oneCase.has_value());
    std::vector<frost::run::GTestReport> reports(3);
    reports[0].fileStem = frost::run::gtestReportStem("fixture.json");
    reports[0].cases = *wholeCases;
    reports[1].fileStem = frost::run::gtestReportStem("fixture_1.json");
    reports[1].cases = *oneCase;
    reports[2].fileStem = frost::run::gtestReportStem("twin.json");
    reports[2].cases = *wholeCases;
    frost::run::attachGTestReports(*results, *tests, reports);

    const auto& whole = (*results)[0];
    EXPECT_EQ(whole.outcome, TestOutcome::Failed) << "ctest's verdict stands";
    ASSERT_EQ(whole.cases.size(), wholeCases->size()) << "the whole program's report holds all its cases";
    EXPECT_EQ(whole.cases.front().name, "Fixture.Passes");
    const auto& filtered = (*results)[1];
    EXPECT_TRUE(filtered.cases.empty());
    ASSERT_EQ(filtered.failures.size(), 1U) << "the filtered test gets its own process's failure";
    EXPECT_EQ(filtered.failures[0].location->line, 18);
    EXPECT_TRUE((*results)[2].cases.empty()) << "two tests share twin.exe: ambiguous";
    EXPECT_TRUE((*results)[3].cases.empty());
    EXPECT_TRUE((*results)[4].cases.empty());
}

TEST(TestModel, DurationsAndNames) {
    EXPECT_EQ(frost::run::formatTestDuration(std::nullopt), "");
    EXPECT_EQ(frost::run::formatTestDuration(500us), "< 1 ms");
    EXPECT_EQ(frost::run::formatTestDuration(12ms), "12 ms");
    EXPECT_EQ(frost::run::formatTestDuration(1530ms), "1.53 s");
    EXPECT_EQ(frost::run::formatTestDuration(125s), "2 min 05 s");
    EXPECT_EQ(frost::run::toString(TestOutcome::NotRun), "Not run");
    EXPECT_FALSE(frost::run::countsAsFailure(TestOutcome::Skipped));
    EXPECT_TRUE(frost::run::countsAsFailure(TestOutcome::Failed));
}

// A model and profile as the File API reply and the presets give them.
frost::project::CMakeProject modelWithCache(std::vector<frost::project::CMakeCacheEntry> cache) {
    frost::project::CMakeProject model;
#ifdef _WIN32
    model.buildDirectory = "C:\\proj\\build";
#else
    model.buildDirectory = "/proj/build";
#endif
    model.cmakeVersion = "4.4.5";
    model.configuration = "Debug";
    model.cache = std::move(cache);
    model.cacheAvailable = true;
    return model;
}

TEST(TestRunnerSetup, CTestComesFromTheCacheAndThePresetAddsItsSelection) {
#ifdef _WIN32
    const std::string ctest = "C:/sdk/cmake/bin/ctest.exe";
    const std::string cmake = "C:/sdk/cmake/bin/cmake.exe";
    const std::string nativeCTest = "C:\\sdk\\cmake\\bin\\ctest.exe";
#else
    const std::string ctest = "/sdk/cmake/bin/ctest";
    const std::string cmake = "/sdk/cmake/bin/cmake";
    const std::string nativeCTest = ctest;
#endif
    frost::project::CMakeProfile profile;
    profile.name = "debug";
    profile.configurePreset = "debug";
    profile.environment = {{"FROM_CONFIGURE", "1"}};

    auto fromCTest =
        frost::run::ctestSetupFor(modelWithCache({{"CMAKE_CTEST_COMMAND", "INTERNAL", ctest}}), profile, nullptr);
    ASSERT_TRUE(fromCTest.has_value()) << fromCTest.error().describe();
    EXPECT_EQ(fromCTest->ctest, nativeCTest);
    EXPECT_TRUE(fromCTest->configuration.empty()) << "a single-configuration generator needs no -C";
    EXPECT_EQ(fromCTest->environment, profile.environment);
    auto fromCMake =
        frost::run::ctestSetupFor(modelWithCache({{"CMAKE_COMMAND", "INTERNAL", cmake}}), profile, nullptr);
    ASSERT_TRUE(fromCMake.has_value()) << fromCMake.error().describe();
    EXPECT_EQ(fromCMake->ctest, nativeCTest);
    auto none = frost::run::ctestSetupFor(modelWithCache({}), profile, nullptr);
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code(), frost::ErrorCode::NotFound);
    auto old = modelWithCache({{"CMAKE_CTEST_COMMAND", "INTERNAL", ctest}});
    old.cmakeVersion = "3.20.0";
    EXPECT_EQ(frost::run::ctestSetupFor(old, profile, nullptr).error().code(), frost::ErrorCode::Unsupported);

    auto multi = modelWithCache({{"CMAKE_CTEST_COMMAND", "INTERNAL", ctest}});
    multi.multiConfig = true;
    multi.configuration = "RelWithDebInfo";
    frost::project::CMakeTestPreset preset;
    preset.name = "debug-tests";
    preset.includeLabel = "unit";
    preset.excludeName = "slow";
    preset.jobs = 4;
    preset.stopOnFailure = true;
    preset.environment = {{"FROM_PRESET", "1"}};
    auto withPreset = frost::run::ctestSetupFor(multi, profile, &preset);
    ASSERT_TRUE(withPreset.has_value());
    EXPECT_EQ(withPreset->configuration, "RelWithDebInfo");
    EXPECT_EQ(withPreset->testPreset, "debug-tests");
    EXPECT_EQ(withPreset->jobs, 4);
    EXPECT_TRUE(withPreset->stopOnFailure);
    EXPECT_EQ(withPreset->environment, preset.environment);
    preset.configuration = "Debug";
    EXPECT_EQ(frost::run::ctestSetupFor(multi, profile, &preset)->configuration, "Debug");

    const auto arguments = frost::run::ctestDiscoveryArguments(*withPreset);
    EXPECT_EQ(arguments, (std::vector<std::string>{"--test-dir", multi.buildDirectory, "-C", "RelWithDebInfo", "-E",
                                                   "slow", "-L", "unit", "-N", "--show-only=json-v1"}));
}

TEST(TestRunnerSetup, ThePresetOfTheProfilesConfigurePresetIsChosen) {
    frost::project::CMakePresets presets;
    const auto add = [&](std::string name, std::string configure, bool hidden = false) {
        frost::project::CMakeTestPreset preset;
        preset.name = std::move(name);
        preset.configurePreset = std::move(configure);
        preset.hidden = hidden;
        presets.testPresets.push_back(std::move(preset));
    };
    add("base", "windows-debug", true);
    add("other", "windows-release");
    add("windows-debug-unit", "windows-debug");
    add("windows-debug", "windows-debug");
    frost::project::CMakeProfile profile;
    profile.configurePreset = "windows-debug";
    const auto* chosen = frost::run::testPresetFor(presets, profile);
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->name, "windows-debug") << "the one named like the profile's preset";
    profile.configurePreset = "windows-release";
    EXPECT_EQ(frost::run::testPresetFor(presets, profile)->name, "other");
    profile.configurePreset = "linux-debug";
    EXPECT_EQ(frost::run::testPresetFor(presets, profile), nullptr);
    profile.configurePreset.clear();
    EXPECT_EQ(frost::run::testPresetFor(presets, profile), nullptr) << "a settings profile has no presets";
}

TEST(TestRunnerSetup, TheExactNameRegexEscapesEveryMetacharacter) {
    EXPECT_EQ(frost::run::ctestExactNameRegex({"a", "b.c"}), "^(a|b\\.c)$");
    EXPECT_EQ(frost::run::ctestExactNameRegex({"spaces (and regex+chars) [x]"}),
              "^(spaces \\(and regex\\+chars\\) \\[x\\])$");
    EXPECT_EQ(frost::run::ctestExactNameRegex({"^$|*?\\{}"}), "^(\\^\\$\\|\\*\\?\\\\\\{\\})$");
}

} // namespace
