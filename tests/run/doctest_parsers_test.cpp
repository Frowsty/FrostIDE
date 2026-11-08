// SPDX-License-Identifier: Apache-2.0
//
// The doctest parts of the test runner (RUN-8, doctest_parsers.hpp): which commands run a doctest
// program, the list and run command lines, and the parsers of doctest's XML case list, XML run
// report and console failure blocks. The inputs are real doctest 2.5.3 output (the version the
// pinned FrostEngine builds), captured from fixtures/doctest/capture.cpp built with the pinned
// clang-cl and -fmacro-prefix-map=<dir>=E:\fixture\doctest:
//   list.xml          capture --list-test-cases --reporters=xml
//   list_math.xml     capture --test-suite=Math --list-test-cases --reporters=xml
//   run_math.xml      capture --no-intro=true --test-suite=Math --reporters=xml --duration=true
//                             --no-skipped-summary=true   (exit 1)
//   run_one.xml       the same with --test-case=compares\, with a comma   (exit 1)
//   run_exit.xml      the same over --test-suite=Exit, whose second case calls std::_Exit(3)
//   console_math.txt  capture --no-intro=true --test-suite=Math   (the console reporter)
//   console_gnu.txt   capture --no-intro=true --test-suite=Math --test-case=requires --gnu-file-line=true
// The expected values are the ones those files state (counts, names, lines). The real FrostEngine
// tests drive FrostTests itself (tests/app/corpus_doctest_test.cpp).

#include "frost/io/file_system.hpp"
#include "frost/run/doctest_parsers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::run::TestOutcome;

constexpr std::string_view kSource = "E:\\fixture\\doctest\\capture.cpp";

std::string fixture(std::string_view name) {
    auto text = frost::io::realFileSystem().readFile(std::string(FROST_RUN_FIXTURES) + "/doctest/" + std::string(name));
    EXPECT_TRUE(text.has_value()) << name;
    return text ? std::move(*text) : std::string();
}

const frost::run::TestResult* find(const std::vector<frost::run::TestResult>& results, std::string_view name) {
    const auto found = std::ranges::find(results, name, &frost::run::TestResult::name);
    return found == results.end() ? nullptr : &*found;
}

TEST(DoctestCommand, RecognisesDoctestsOwnSelectionOptions) {
    using frost::run::isDoctestCommand;
    // FrostEngine's per-suite tests (cmake/FrostDiscoverTestSuites.cmake) and doctest's CMake helper.
    EXPECT_TRUE(isDoctestCommand({"E:/b/bin/FrostTests.exe", "--no-intro=true", "--test-suite=Core"}));
    EXPECT_TRUE(isDoctestCommand({"/b/tests", "--test-case=adds"}));
    EXPECT_TRUE(isDoctestCommand({"/b/tests", "-ts=Core"}));
    EXPECT_TRUE(isDoctestCommand({"/b/tests", "--dt-test-suite=Core"}));
    EXPECT_TRUE(isDoctestCommand({"/b/tests", "-sf=*math*"}));
    EXPECT_TRUE(isDoctestCommand({"/b/tests", "--no-intro"}));
    // Google Test, Catch2, a script and cmake -E echo (FrostEngine's NotBuilt/Coverage markers) are not.
    EXPECT_FALSE(isDoctestCommand({"/b/gtest", "--gtest_filter=Suite.Case"}));
    EXPECT_FALSE(isDoctestCommand({"/b/catch", "--reporter", "xml", "-r", "junit"}));
    EXPECT_FALSE(isDoctestCommand({"/py/python.exe", "-B", "fixture.py", "setup", "--root", "/x"}));
    EXPECT_FALSE(isDoctestCommand(
        {"/cmake/cmake.exe", "-E", "echo", "FrostTests executable not found: E:/b/bin/FrostTests.exe"}));
    EXPECT_FALSE(isDoctestCommand({"/b/tests"}));
    EXPECT_FALSE(isDoctestCommand({}));
    // The program itself is never read as an option.
    EXPECT_FALSE(isDoctestCommand({"--test-suite=x"}));
}

TEST(DoctestCommand, FilterValuesEscapeCommasAndBackslashesAsDoctestReadsThem) {
    EXPECT_EQ(frost::run::doctestFilterValue({"adds"}), "adds");
    EXPECT_EQ(frost::run::doctestFilterValue({"compares, with a comma", "a\\b", "c"}),
              "compares\\, with a comma,a\\\\b,c");
}

TEST(DoctestCommand, ListAndRunArgumentsFollowTheTestsOwnArguments) {
    const std::vector<std::string> command = {"E:/b/FrostTests.exe", "--no-intro=true", "--test-suite=Core"};
    EXPECT_EQ(frost::run::doctestListArguments(command, "E:/b/Testing/Temporary/l.xml"),
              (std::vector<std::string>{"--no-intro=true", "--test-suite=Core", "--list-test-cases", "--reporters=xml",
                                        "--out=E:/b/Testing/Temporary/l.xml"}));
    EXPECT_EQ(frost::run::doctestRunArguments(command, {}, "E:/r.xml"),
              (std::vector<std::string>{"--no-intro=true", "--test-suite=Core", "--reporters=xml", "--duration=true",
                                        "--no-skipped-summary=true", "--out=E:/r.xml"}));
    EXPECT_EQ(
        frost::run::doctestRunArguments(command, {"adds", "x, y"}, "E:/r.xml"),
        (std::vector<std::string>{"--no-intro=true", "--test-suite=Core", "--test-case=adds,x\\, y", "--reporters=xml",
                                  "--duration=true", "--no-skipped-summary=true", "--out=E:/r.xml"}));
    // A command that spells doctest's options with "dt-" gets ours spelled that way too.
    EXPECT_EQ(frost::run::doctestListArguments({"/b/t", "--dt-test-suite=Core"}, "/r.xml"),
              (std::vector<std::string>{"--dt-test-suite=Core", "--dt-list-test-cases", "--dt-reporters=xml",
                                        "--dt-out=/r.xml"}));
}

TEST(DoctestCaseList, ReadsTheXmlListingWithSuitesAndLocations) {
    auto all = frost::run::parseDoctestCaseList(fixture("list.xml"));
    ASSERT_TRUE(all.has_value()) << all.error().describe();
    // list.xml: <OverallResultsTestCases unskipped="11"/> (the skip() case is not listed).
    ASSERT_EQ(all->size(), 11u);
    EXPECT_EQ((*all)[0].name, "adds");
    EXPECT_EQ((*all)[0].suite, "Math");
    ASSERT_TRUE((*all)[0].location.has_value());
    EXPECT_EQ((*all)[0].location->file, kSource);
    EXPECT_EQ((*all)[0].location->line, 9);
    EXPECT_EQ((*all)[1].name, "compares, with a comma");
    const auto outside = std::ranges::find(*all, "outside every suite", &frost::run::DoctestCase::name);
    ASSERT_NE(outside, all->end());
    EXPECT_EQ(outside->suite, "");
    EXPECT_EQ(outside->location->line, 42);
    EXPECT_EQ(std::ranges::count(*all, "Exit", &frost::run::DoctestCase::suite), 3);
    EXPECT_EQ(std::ranges::count(*all, "is skipped", &frost::run::DoctestCase::name), 0);

    auto math = frost::run::parseDoctestCaseList(fixture("list_math.xml"));
    ASSERT_TRUE(math.has_value()) << math.error().describe();
    ASSERT_EQ(math->size(), 7u); // unskipped="7"
    EXPECT_TRUE(std::ranges::all_of(*math, [](const auto& c) { return c.suite == "Math"; }));
    EXPECT_EQ(math->back().name, "may fail");
    EXPECT_EQ(math->back().location->line, 37);
}

TEST(DoctestCaseList, RefusesWhatIsNoCaseList) {
    for (const std::string_view bad :
         {std::string_view{"not xml at all <"}, std::string_view{"<testsuite/>"},
          // No summary: a listing cut short.
          std::string_view{
              R"(<doctest binary="t"><TestCase name="a" testsuite="" filename="a.cpp" line="1"/></doctest>)"},
          // A count other than the summary's.
          std::string_view{R"(<doctest binary="t"><TestCase name="a" filename="a.cpp" line="1"/>)"
                           R"(<OverallResultsTestCases unskipped="2"/></doctest>)"}}) {
        auto parsed = frost::run::parseDoctestCaseList(bad);
        ASSERT_FALSE(parsed.has_value()) << bad;
        EXPECT_EQ(parsed.error().code(), frost::ErrorCode::Corrupt);
    }
    // A program with no case: an empty, valid list. Line 0 (--no-line-numbers) is no location.
    auto empty = frost::run::parseDoctestCaseList(R"(<doctest><OverallResultsTestCases unskipped="0"/></doctest>)");
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
    auto noLine = frost::run::parseDoctestCaseList(
        R"(<doctest><TestCase name="a" filename="a.cpp" line="0"/><OverallResultsTestCases unskipped="1"/></doctest>)");
    ASSERT_TRUE(noLine.has_value());
    EXPECT_FALSE((*noLine)[0].location.has_value());
}

TEST(DoctestReport, ReadsEveryCaseWithOutcomeDurationAndFailureLocations) {
    auto report = frost::run::parseDoctestXmlReport(fixture("run_math.xml"));
    ASSERT_TRUE(report.has_value()) << report.error().describe();
    EXPECT_TRUE(report->complete);
    // <OverallResultsTestCases successes="2" failures="5"/>
    EXPECT_EQ(report->passedCases, 2u);
    EXPECT_EQ(report->failedCases, 5u);
    EXPECT_FALSE(report->unfinished.has_value());
    ASSERT_EQ(report->cases.size(), 7u);

    const auto* adds = find(report->cases, "adds");
    ASSERT_NE(adds, nullptr);
    EXPECT_EQ(adds->outcome, TestOutcome::Passed);
    EXPECT_EQ(adds->duration, 34us); // duration="3.4e-05"
    EXPECT_TRUE(adds->failures.empty());
    ASSERT_TRUE(adds->definition.has_value());
    EXPECT_EQ(adds->definition->line, 9);

    const auto* compares = find(report->cases, "compares, with a comma");
    ASSERT_NE(compares, nullptr);
    EXPECT_EQ(compares->outcome, TestOutcome::Failed);
    EXPECT_EQ(compares->duration, 210us); // duration="0.00021"
    ASSERT_EQ(compares->failures.size(), 1u);
    EXPECT_EQ(compares->failures[0].message, "CHECK( a == 2 ) is NOT correct!\n  values: CHECK( 1 == 2 )");
    ASSERT_TRUE(compares->failures[0].location.has_value());
    EXPECT_EQ(compares->failures[0].location->file, kSource);
    EXPECT_EQ(compares->failures[0].location->line, 14);

    const auto* requires_ = find(report->cases, "requires");
    ASSERT_NE(requires_, nullptr);
    ASSERT_EQ(requires_->failures.size(), 1u);
    EXPECT_EQ(requires_->failures[0].message, "REQUIRE( 2 * 2 == 5 ) is NOT correct!\n  values: REQUIRE( 4 == 5 )");
    EXPECT_EQ(requires_->failures[0].location->line, 18);

    // A failure inside a subcase keeps its INFO and names the subcase.
    const auto* subcases = find(report->cases, "has subcases");
    ASSERT_NE(subcases, nullptr);
    ASSERT_EQ(subcases->failures.size(), 1u);
    EXPECT_EQ(
        subcases->failures[0].message,
        "CHECK( 7 == 8 ) is NOT correct!\n  values: CHECK( 7 == 8 )\n  logged: the value is 7\n  in subcase: second");
    EXPECT_EQ(subcases->failures[0].location->line, 24);

    // An exception has no location of its own.
    const auto* throws = find(report->cases, "throws");
    ASSERT_NE(throws, nullptr);
    EXPECT_EQ(throws->outcome, TestOutcome::Failed);
    ASSERT_EQ(throws->failures.size(), 1u);
    EXPECT_EQ(throws->failures[0].message, "test case THREW exception: boom");
    EXPECT_FALSE(throws->failures[0].location.has_value());
    EXPECT_EQ(throws->definition->line, 27);

    // MESSAGE is a WARNING, no failure; FAIL_CHECK is an ERROR.
    const auto* messages = find(report->cases, "messages");
    ASSERT_NE(messages, nullptr);
    ASSERT_EQ(messages->failures.size(), 1u);
    EXPECT_EQ(messages->failures[0].message, "ERROR: a failure message");
    EXPECT_EQ(messages->failures[0].location->line, 32);

    // may_fail: doctest judged it a success although an assertion failed.
    const auto* mayFail = find(report->cases, "may fail");
    ASSERT_NE(mayFail, nullptr);
    EXPECT_EQ(mayFail->outcome, TestOutcome::Passed);

    // Passed + failed per case equals doctest's own summary.
    EXPECT_EQ(std::ranges::count(report->cases, TestOutcome::Passed, &frost::run::TestResult::outcome),
              static_cast<std::ptrdiff_t>(report->passedCases));
    EXPECT_EQ(std::ranges::count(report->cases, TestOutcome::Failed, &frost::run::TestResult::outcome),
              static_cast<std::ptrdiff_t>(report->failedCases));
}

TEST(DoctestReport, AReportCutShortKeepsTheCasesThatEndedAndNamesTheUnfinishedOne) {
    auto report = frost::run::parseDoctestXmlReport(fixture("run_exit.xml"));
    ASSERT_TRUE(report.has_value()) << report.error().describe();
    EXPECT_FALSE(report->complete);
    ASSERT_EQ(report->cases.size(), 1u);
    EXPECT_EQ(report->cases[0].name, "passes first");
    EXPECT_EQ(report->cases[0].outcome, TestOutcome::Passed);
    ASSERT_TRUE(report->unfinished.has_value());
    EXPECT_EQ(report->unfinished->name, "exits the process");
    EXPECT_EQ(report->unfinished->suite, "Exit");
    EXPECT_EQ(report->unfinished->location->line, 50);

    // The program exited with 3 (capture's std::_Exit(3)).
    frost::run::DoctestProcessEnd end;
    end.exitCode = 3;
    end.duration = 120ms;
    auto result = frost::run::doctestRunResult("FrostTests.Exit", report, end);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->name, "FrostTests.Exit");
    EXPECT_EQ(result->outcome, TestOutcome::Failed);
    EXPECT_EQ(result->reason, "Exit code 3 before doctest finished its report");
    EXPECT_EQ(result->duration, 120ms);
    ASSERT_EQ(result->cases.size(), 2u);
    EXPECT_EQ(result->cases[1].name, "exits the process");
    EXPECT_EQ(result->cases[1].outcome, TestOutcome::Failed);
    ASSERT_EQ(result->cases[1].failures.size(), 1u);
    EXPECT_EQ(result->cases[1].failures[0].location->line, 50);

    // Stopped by the user: the case that ended is a result, the unfinished one is not.
    end.stopped = true;
    auto stopped = frost::run::doctestRunResult("FrostTests.Exit", report, end);
    ASSERT_TRUE(stopped.has_value());
    EXPECT_EQ(stopped->outcome, TestOutcome::NotRun);
    EXPECT_EQ(stopped->reason, "Stopped");
    ASSERT_EQ(stopped->cases.size(), 1u);
}

TEST(DoctestReport, RunResultsFollowDoctestsSummaryAndTheExitCode) {
    auto one = frost::run::parseDoctestXmlReport(fixture("run_one.xml"));
    ASSERT_TRUE(one.has_value()) << one.error().describe();
    ASSERT_EQ(one->cases.size(), 1u);
    EXPECT_EQ(one->cases[0].name, "compares, with a comma");
    frost::run::DoctestProcessEnd end;
    end.exitCode = 1;
    auto failed = frost::run::doctestRunResult("T", one, end);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->outcome, TestOutcome::Failed);
    EXPECT_EQ(failed->reason, "1 of 1 case(s) failed");

    const std::string passing =
        R"(<doctest binary="t"><TestSuite name="S"><TestCase name="a" filename="a.cpp" line="3">)"
        R"(<OverallResultsAsserts successes="1" failures="0" test_case_success="true" duration="0.5"/>)"
        R"(</TestCase></TestSuite><OverallResultsTestCases successes="1" failures="0"/></doctest>)";
    auto passed = frost::run::parseDoctestXmlReport(passing);
    ASSERT_TRUE(passed.has_value());
    end.exitCode = 0;
    auto ok = frost::run::doctestRunResult("T", passed, end);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->outcome, TestOutcome::Passed);
    EXPECT_EQ(ok->cases[0].duration, 500ms);
    // Every case passed but the program failed anyway (FrostEngine's main fails a run its GPU
    // validation flagged): the test failed, with the exit code as the reason.
    end.exitCode = 1;
    auto exitFailed = frost::run::doctestRunResult("T", passed, end);
    ASSERT_TRUE(exitFailed.has_value());
    EXPECT_EQ(exitFailed->outcome, TestOutcome::Failed);
    EXPECT_EQ(exitFailed->reason, "Exit code 1");

    // No report: no result is claimed.
    const frost::Result<frost::run::DoctestReport> missing = frost::fail(frost::ErrorCode::NotFound, "none");
    EXPECT_FALSE(frost::run::doctestRunResult("T", missing, end).has_value());
    auto notDoctest = frost::run::parseDoctestXmlReport(R"(<testsuites/>)");
    ASSERT_FALSE(notDoctest.has_value());
    EXPECT_EQ(notDoctest.error().code(), frost::ErrorCode::Corrupt);
    EXPECT_FALSE(frost::run::parseDoctestXmlReport("").has_value());
}

TEST(DoctestConsole, FailureBlocksGiveTheFailedCasesWithTheirLocations) {
    const auto cases = frost::run::parseDoctestConsoleFailures(fixture("console_math.txt"));
    // console_math.txt: blocks for compares, requires, has subcases, throws, messages and may fail,
    // which doctest then marks as not failed. Its summary says 5 failed.
    ASSERT_EQ(cases.size(), 5u);
    EXPECT_EQ(cases[0].name, "compares, with a comma");
    EXPECT_EQ(cases[0].outcome, TestOutcome::Failed);
    ASSERT_TRUE(cases[0].definition.has_value());
    EXPECT_EQ(cases[0].definition->file, kSource);
    EXPECT_EQ(cases[0].definition->line, 12);
    ASSERT_EQ(cases[0].failures.size(), 1u);
    EXPECT_EQ(cases[0].failures[0].message, "ERROR: CHECK( a == 2 ) is NOT correct!\n  values: CHECK( 1 == 2 )");
    EXPECT_EQ(cases[0].failures[0].location->file, kSource);
    EXPECT_EQ(cases[0].failures[0].location->line, 14);
    EXPECT_EQ(cases[1].name, "requires");
    EXPECT_EQ(cases[1].failures[0].location->line, 18);
    EXPECT_TRUE(cases[1].failures[0].message.starts_with("FATAL ERROR: REQUIRE( 2 * 2 == 5 )"));
    EXPECT_EQ(cases[2].name, "has subcases");
    EXPECT_EQ(cases[2].failures[0].location->line, 24);
    EXPECT_NE(cases[2].failures[0].message.find("logged: the value is 7"), std::string::npos);
    EXPECT_EQ(cases[3].name, "throws");
    EXPECT_EQ(cases[3].failures[0].message, "ERROR: test case THREW exception: boom");
    EXPECT_EQ(cases[4].name, "messages");
    ASSERT_EQ(cases[4].failures.size(), 1u); // The MESSAGE line is no failure.
    EXPECT_EQ(cases[4].failures[0].location->line, 32);
    EXPECT_TRUE(std::ranges::none_of(cases, [](const auto& c) { return c.name == "may fail"; }));

    const auto gnu = frost::run::parseDoctestConsoleFailures(fixture("console_gnu.txt"));
    ASSERT_EQ(gnu.size(), 1u);
    EXPECT_EQ(gnu[0].name, "requires");
    EXPECT_EQ(gnu[0].definition->line, 17);
    EXPECT_EQ(gnu[0].failures[0].location->file, kSource);
    EXPECT_EQ(gnu[0].failures[0].location->line, 18);

    // Colours (--force-colors) are ignored; an output without failure blocks gives nothing.
    std::string coloured = fixture("console_gnu.txt");
    coloured.insert(coloured.find("TEST CASE:"), "\x1b[0;33m");
    EXPECT_EQ(frost::run::parseDoctestConsoleFailures(coloured).size(), 1u);
    EXPECT_TRUE(frost::run::parseDoctestConsoleFailures("[doctest] test cases: 3 | 3 passed | 0 failed |\n").empty());
    EXPECT_TRUE(frost::run::parseDoctestConsoleFailures("").empty());
}

} // namespace
