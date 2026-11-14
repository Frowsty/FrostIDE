// SPDX-License-Identifier: Apache-2.0
//
// Parsers of Google Test's own output (RUN-7, PLAN.md Appendix C R2): the case list of
// `<program> --gtest_list_tests` and the JSON report a program writes for --gtest_output=json (or
// the GTEST_OUTPUT variable), and the rule that attaches those reports to the ctest tests whose
// processes wrote them. The only source of GTestCase, of Google Test's TestResults and of
// TestFailure (test_model.hpp, truth rule). Pure functions; thread-safe.
//
/*
 * Formats (Google Test 1.10+, checked against the 1.18 this repository's own tests use):
 *
 *   --gtest_list_tests:   Suite.
 *                           Case
 *                         Typed/0.  # TypeParam = int
 *                           Case
 *                         Prefix/Param.
 *                           Case/0  # GetParam() = 4
 *   Lines before the first suite (a "Running main() from ..." banner) are ignored.
 *
 *   JSON report: {"testsuites": [{"name": "Suite", "testsuite": [{"name": "Case", "status":
 *   "RUN"|"NOTRUN", "result": "COMPLETED"|"SKIPPED"|"SUPPRESSED", "time": "0.001s",
 *   "failures": [{"failure": "file:line\nmessage", "type": ""}], ...}]}]}. A completed case with
 *   failures failed, one without passed; SKIPPED is GTEST_SKIP; NOTRUN/SUPPRESSED is a DISABLED_
 *   case. The listing variant (--gtest_list_tests with --gtest_output=json, which ctest's
 *   gtest_discover_tests runs) has no "result": it lists, it reports nothing, and gives no result.
 *
 * Attaching. A ctest run started with GTEST_OUTPUT=json:<directory>/ makes every Google Test
 * program write <directory>/<program stem>.json, or <stem>_1.json, <stem>_2.json, ... when the file
 * exists (Google Test's unique file names). A report's cases go to:
 *   - the one ctest test of the run whose program has that stem and that runs it without a filter
 *     (a whole Google Test program as one ctest test): its result gets all the cases, unless the
 *     report holds a single case that a filtered test names (that process was the filtered one);
 *   - otherwise each case goes to the ctest test whose --gtest_filter is exactly its full name
 *     (gtest_discover_tests registers one ctest test per case): its result gets the case's failures.
 * A report that matches neither (two such tests share the program) is left out: the ctest
 * results stand on their own.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/run/test_model.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::run {

/*
 * A Google Test filter value that names `cases` (`Suite.Case`, joined with ':'). Pure.
 */
[[nodiscard]] std::string gtestFilterValue(std::span<const std::string> cases);

/*
 * The arguments that run `cases` (all when empty) of a Google Test program whose base arguments are
 * `baseArgs`, writing a JSON report to `jsonReportPath` when non-empty: `baseArgs` without
 * competing `--gtest_output=` / `--gtest_list_tests` / `--gtest_repeat=` (and without
 * `--gtest_filter=` when `cases` is non-empty), then `[--gtest_filter=<cases>]` and
 * `[--gtest_output=json:<jsonReportPath>]`. Pure.
 */
[[nodiscard]] std::vector<std::string> gtestRunArguments(std::span<const std::string> baseArgs,
                                                         std::span<const std::string> cases,
                                                         std::string_view jsonReportPath);

// How the Google Test process of a direct run ended (RUN-10).
struct GTestProcessEnd {
    int exitCode = 0;
    bool stopped = false;
    std::optional<std::chrono::microseconds> duration{};
    std::string output{};
};

/*
 * The result of the ctest test `testName` that a direct Google Test run gave, from its JSON
 * report's cases and how its process ended, with the report's cases as `cases`. nullopt when
 * `cases` is an error, or when stopped with no finished cases. Pure.
 */
[[nodiscard]] std::optional<TestResult>
gtestRunResult(const std::string& testName, const Result<std::vector<TestResult>>& cases, const GTestProcessEnd& end);

/*
 * The cases of a `--gtest_list_tests` output, in the program's order. Errors: Corrupt (a case line
 * before any suite line, or a suite without cases). O(output size).
 */
[[nodiscard]] Result<std::vector<GTestCase>> parseGTestList(std::string_view output);

/*
 * The cases a JSON report says ran (or were disabled), in the report's order, each named by its
 * full name ("Suite.Case"). Errors: Corrupt (not JSON, no "testsuites" array). O(report size).
 */
[[nodiscard]] Result<std::vector<TestResult>> parseGTestJsonReport(std::string_view report);

// The program stem Google Test names a report after: "C:/x/run_tests.exe" -> "run_tests" (the
// ".exe" is dropped in any case, as Google Test drops it on Windows). Pure.
[[nodiscard]] std::string gtestProgramStem(std::string_view program);

// One report file: the stem of its name (gtestReportStem) and its cases.
struct GTestReport {
    std::string fileStem{}; // "run_tests" for run_tests.json and run_tests_3.json alike.
    std::vector<TestResult> cases{};
};

// "run_tests_3.json" -> "run_tests"; "run_tests.json" -> "run_tests"; "" for a name that is not *.json.
[[nodiscard]] std::string gtestReportStem(std::string_view fileName);

/*
 * Attaches `reports` to the ctest results of one run (header comment, "Attaching"). `tests` is the
 * discovered list the run was started from; a result whose name is not in it gets nothing.
 */
void attachGTestReports(std::vector<TestResult>& results, const std::vector<DiscoveredTest>& tests,
                        const std::vector<GTestReport>& reports);

} // namespace frost::run
