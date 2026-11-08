// SPDX-License-Identifier: Apache-2.0
//
// doctest support of the test runner (RUN-8, PLAN.md Appendix C R2): which ctest tests run a doctest
// program, the command lines that list and run such a test's cases with doctest's XML reporter, and
// the parsers of doctest's own output (its XML case list, its XML run report and the failure blocks
// of its console reporter that ctest's report carries). The only source of DoctestCase and of
// doctest's TestResults (test_model.hpp, truth rule). Pure functions; thread-safe.
//
/*
 * FrostEngine registers one ctest test per doctest suite (`FrostTests --no-intro=true
 * --test-suite=<suite>`), so a ctest test is a set of doctest cases. The test runner lists that set
 * by running the test's own command with --list-test-cases (doctest applies the test's filters
 * itself), and runs some or all of its cases by running the command with the XML reporter. ctest is
 * not involved there: doctest reads no environment variable that would make the processes ctest
 * starts write a report, so a ctest run gives one result per ctest test, with the failing cases
 * read from the console output ctest's JUnit report holds (parseDoctestConsoleFailures).
 *
 * Formats (doctest 2.4 and 2.5; checked against the 2.5.3 the pinned FrostEngine builds):
 *
 *   --list-test-cases --reporters=xml:
 *     <doctest binary="FrostTests" version="2.5.3"><Options .../>
 *       <TestCase name="..." testsuite="..." filename="..." line="12" skipped="false"/> ...
 *       <OverallResultsTestCases unskipped="N"/></doctest>
 *
 *   a run with --reporters=xml --duration=true --no-skipped-summary=true:
 *     <doctest ...><Options .../>
 *       <TestSuite name="Core">
 *         <TestCase name="..." filename="..." line="12">
 *           <SubCase name="..." filename="..." line="14"> (nested; holds what follows) </SubCase>
 *           <Expression success="false" type="CHECK" filename="..." line="15">
 *             <Original>a == b</Original><Expanded>1 == 2</Expanded><Info>...</Info></Expression>
 *           <Message type="ERROR" filename="..." line="16"><Text>...</Text></Message>
 *           <Exception crash="false">what()</Exception>
 *           <OverallResultsAsserts successes="3" failures="1" test_case_success="false" duration="0.0012"/>
 *         </TestCase>
 *       </TestSuite>
 *       <OverallResultsAsserts successes=".." failures=".."/>
 *       <OverallResultsTestCases successes="1" failures="1"/></doctest>
 *     The report is written as the run goes, so a program that crashes leaves it cut short: the
 *     cases that ended keep their results and the one that had started has no end.
 *
 *   console failure block (what ctest's report holds for a FrostEngine suite):
 *     ===============================================================================
 *     E:\x\CoreTests.cpp(12):            (gnu style: "file:12:")
 *     TEST SUITE: Core
 *     TEST CASE:  adds
 *
 *     E:\x\CoreTests.cpp(15): ERROR: CHECK( 1 == 2 ) is NOT correct!
 *       values: CHECK( 1 == 2 )
 *
 * File names are as doctest printed them: __FILE__ (absolute with CMake's Ninja and Visual Studio
 * generators), unless the program strips a prefix.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/run/test_model.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::run {

// One case of a doctest program, as its XML case list states it.
struct DoctestCase {
    std::string name{};                     // The TEST_CASE name (not unique across files).
    std::string suite{};                    // The TEST_SUITE; "" outside one.
    std::optional<TestLocation> location{}; // Where the case is defined; nullopt for line 0.

    friend bool operator==(const DoctestCase&, const DoctestCase&) = default;
};

/*
 * Whether `command` (program and arguments, as ctest lists it) runs a doctest program: an argument
 * is one of doctest's own selection or output options (--test-suite=, -ts=, --test-case=, -tc=,
 * --source-file=, -sf= and their -exclude forms, --subcase=, --no-intro, --reporters=), with one or
 * two dashes and optionally doctest's "dt-" prefix. A doctest program run without arguments cannot
 * be told apart; the window offers to list one explicitly. Pure.
 */
[[nodiscard]] bool isDoctestCommand(const std::vector<std::string>& command);

/*
 * A doctest filter value that names exactly `names` (comma-separated; ',' and '\' escaped with a
 * backslash, as doctest reads them). doctest's '*' and '?' wildcards cannot be escaped: a name that
 * holds one may select further cases, which the report then shows as well. Pure.
 */
[[nodiscard]] std::string doctestFilterValue(const std::vector<std::string>& names);

/*
 * The arguments that list `command`'s cases into `reportPath`: the command's own arguments, then
 * --list-test-cases --reporters=xml --out=<reportPath> (doctest takes the last of a repeated option,
 * so these win; with "dt-" when the command spells its options that way). Pure.
 */
[[nodiscard]] std::vector<std::string> doctestListArguments(const std::vector<std::string>& command,
                                                            std::string_view reportPath);

/*
 * The arguments that run `cases` of `command` (all of its cases when empty) into `reportPath`:
 * the command's own arguments, [--test-case=<doctestFilterValue(cases)>], --reporters=xml
 * --duration=true --no-skipped-summary=true --out=<reportPath>. Pure.
 */
[[nodiscard]] std::vector<std::string> doctestRunArguments(const std::vector<std::string>& command,
                                                           const std::vector<std::string>& cases,
                                                           std::string_view reportPath);

/*
 * The cases of a --list-test-cases XML report, in the program's order. Errors: Corrupt (not XML, no
 * <doctest> root, no <OverallResultsTestCases>, or a case count other than its "unskipped").
 * O(report size).
 */
[[nodiscard]] Result<std::vector<DoctestCase>> parseDoctestCaseList(std::string_view report);

// A doctest XML run report, read.
struct DoctestReport {
    // The cases that ended, in the report's order: Passed or Failed (test_case_success), the
    // duration doctest measured, every failed Expression, error Message and Exception as a
    // failure, and the case's location as its definition.
    std::vector<TestResult> cases{};
    // The case that started and has no end (the program ended during it); nullopt otherwise.
    std::optional<DoctestCase> unfinished{};
    // Whether doctest wrote its run summary (the run reached its end), and the summary's counts.
    bool complete = false;
    std::size_t passedCases = 0;
    std::size_t failedCases = 0;
};

/*
 * A run report (header comment). A report cut short keeps what it holds (complete is false).
 * Errors: Corrupt (no <doctest> root: not a doctest report, or cut before it started).
 * O(report size).
 */
[[nodiscard]] Result<DoctestReport> parseDoctestXmlReport(std::string_view report);

// How the doctest process of a direct run ended.
struct DoctestProcessEnd {
    int exitCode = 0;
    bool stopped = false;                                // The user stopped the run.
    std::optional<std::chrono::microseconds> duration{}; // From launch to exit, as measured.
    std::string output{};                                // What the program printed.
};

/*
 * The result of the ctest test `testName` that a direct doctest run gave, from its report and how
 * its process ended, with the report's cases as `cases`:
 *   - complete report: Failed when doctest counted a failed case, or when the program exited
 *     non-zero anyway (reason "Exit code N": FrostEngine's main fails a run its GPU validation
 *     flagged); otherwise Passed;
 *   - report cut short and not stopped: Failed (reason "Exit code N before doctest finished"); the
 *     unfinished case is Failed too ("Ended before doctest reported its end");
 *   - stopped: the cases that ended, and nullopt for the test itself when none did.
 * nullopt as well when `report` is an error (no result is claimed; the caller reports the error).
 * Pure.
 */
[[nodiscard]] std::optional<TestResult>
doctestRunResult(const std::string& testName, const Result<DoctestReport>& report, const DoctestProcessEnd& end);

/*
 * The failed cases of a doctest console output (header comment): one Failed TestResult per case
 * block that holds an ERROR or FATAL ERROR line (an assertion, a message, a crash or an exception),
 * named by its "TEST CASE:" line, its definition from the block's first line and each such line as a
 * failure with its location. Cases doctest then judged as passing ("marking it as not failed") are
 * left out. Colour sequences are ignored. Empty for an output without such blocks. O(output size).
 */
[[nodiscard]] std::vector<TestResult> parseDoctestConsoleFailures(std::string_view output);

} // namespace frost::run
