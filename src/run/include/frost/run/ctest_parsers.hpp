// SPDX-License-Identifier: Apache-2.0
//
// Parsers of ctest's own output (RUN-7, PLAN.md Appendix C R2): the test list of
// `ctest -N --show-only=json-v1`, the report of `ctest --output-junit <file>` and the progress lines
// ctest prints while it runs. The only source of DiscoveredTest and of ctest's TestResults
// (test_model.hpp, truth rule). Pure functions; thread-safe.
//
/*
 * Formats, as ctest 3.21+ writes them (checked against the pinned cmake 4.4.5):
 *
 *   --show-only=json-v1: {"kind": "ctestInfo", "version": {"major": 1, ...}, "backtraceGraph":
 *   {"nodes": [{"file": i, "line": n, "parent": j}, ...], "files": [...]}, "tests": [{"name": ...,
 *   "command": [...], "backtrace": k, "properties": [{"name": "LABELS", "value": [...]}, ...]}]}.
 *   A test without "command" is one whose program ctest could not find.
 *
 *   --output-junit: <testsuite ...><testcase name="..." time="0.015" status="run|fail|disabled|
 *   notrun"> with <failure message="Failed|Timeout|..."/> or <skipped message="..."/> and
 *   <system-out>. status "notrun" with a message starting "SKIP_" (SKIP_RETURN_CODE=n,
 *   SKIP_REGULAR_EXPRESSION_MATCHED) is a skip; any other "notrun" is a test ctest could not start
 *   ("Unable to find executable"), which ctest itself counts as failed (NotRun).
 *
 *   Progress (stdout):  "    Start  3: name"  and
 *                       "3/7 Test  #3: name ....................***Failed    0.02 sec"
 *   The status after the dots is "Passed", or "***" and ctest's reason: "Skipped",
 *   "Not Run (Disabled)", "Not Run", "Timeout", "Failed", "Exception: ...". A name may contain
 *   anything, so a finished line is matched against the name ctest gave that test number.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/run/test_model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::run {

/*
 * The tests of a `ctest -N --show-only=json-v1` output, in ctest's order (number = position + 1).
 * Text before the JSON object (a line some ctest versions print first) is skipped. Errors: Corrupt
 * (not JSON, not kind "ctestInfo", a major version other than 1, a test without a name).
 * O(output size).
 */
[[nodiscard]] Result<std::vector<DiscoveredTest>> parseCTestShowOnly(std::string_view output);

/*
 * The results of a `ctest --output-junit` report, in the report's order (header comment).
 * A <testcase> whose status this parser does not know gives no result. Errors: Corrupt (not XML,
 * no <testsuite> root). O(report size).
 */
[[nodiscard]] Result<std::vector<TestResult>> parseCTestJUnit(std::string_view report);

// One recognised progress line.
struct CTestProgressLine {
    enum class Kind : std::uint8_t { Started, Finished };
    Kind kind = Kind::Started;
    std::size_t number = 0; // ctest's test number.
    std::string text{};     // Started: the name. Finished: everything after "#n: " (name, dots, status, time).
};

// `line` (without its end of line; a trailing '\r' is ignored) as a progress line, or nullopt.
[[nodiscard]] std::optional<CTestProgressLine> parseCTestProgressLine(std::string_view line);

/*
 * The interim result a Finished progress line states for the test `name` (the name ctest's list
 * gives line.number); nullopt when the line is not Finished, does not start with `name`, or its
 * status cannot be read. The result has interim = true and no output.
 */
[[nodiscard]] std::optional<TestResult> ctestProgressResult(const CTestProgressLine& line, std::string_view name);

} // namespace frost::run
