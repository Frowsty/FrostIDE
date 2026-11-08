// SPDX-License-Identifier: Apache-2.0
//
// The test runner's model (RUN-7, PLAN.md Appendix C R2): the tests ctest discovers, the Google Test
// cases a test program lists, and how each one ran. Every value of DiscoveredTest, TestResult and
// TestOutcome comes from a real tool's output, parsed in ctest_parsers.cpp and gtest_parsers.cpp,
// the only files frost-lint lets build them (truth rule, ADR 0078 decision 1). Qt-free.
//
/*
 * Where the values come from:
 *   - DiscoveredTest: one entry of `ctest -N --show-only=json-v1` (ctest_parsers.hpp). The command,
 *     working directory, labels, DISABLED and DEF_SOURCE_LINE come from the test's properties as
 *     ctest prints them; `declaration` is the add_test call of its backtrace.
 *   - GTestCase: one case of a Google Test program's `--gtest_list_tests` (gtest_parsers.hpp).
 *   - TestResult: one <testcase> of `ctest --output-junit`, one finished test of ctest's console
 *     progress (while a run goes on, or when it was stopped before the report was written), or
 *     one test of a Google Test JSON report (`--gtest_output=json`), attached to the ctest test
 *     whose process wrote it.
 * An outcome is never assumed: a test that has no result simply has none.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::run {

// A file and 1-based line a tool reported. `file` is as the tool spelled it (absolute for ctest's
// backtraces and DEF_SOURCE_LINE; Google Test prints __FILE__, which the compiler may make relative).
struct TestLocation {
    std::string file{};
    int line = 0;

    friend bool operator==(const TestLocation&, const TestLocation&) = default;
};

// One test as `ctest -N --show-only=json-v1` lists it.
struct DiscoveredTest {
    std::string name{};                        // Unique in the build directory ("Suite.Case", "passes").
    std::size_t number = 0;                    // ctest's test number, 1-based ("Test #3"), its position in the list.
    std::vector<std::string> command{};        // Program and arguments; empty when ctest found no program.
    std::string workingDirectory{};            // WORKING_DIRECTORY, as ctest printed it ("" when absent).
    std::vector<std::string> labels{};         // LABELS, in ctest's order.
    bool disabled = false;                     // DISABLED.
    std::optional<TestLocation> definition{};  // DEF_SOURCE_LINE (gtest_discover_tests sets it).
    std::optional<TestLocation> declaration{}; // The add_test call (the backtrace's innermost frame).
    std::string gtestFilter{};                 // The --gtest_filter=... value of the command; "" without one.

    friend bool operator==(const DiscoveredTest&, const DiscoveredTest&) = default;
};

// One case of a Google Test program, as `--gtest_list_tests` prints it.
struct GTestCase {
    std::string suite{};      // "Suite", "Prefix/Param", "Typed/0".
    std::string name{};       // "Case", "Case/3".
    std::string typeParam{};  // "# TypeParam = int" -> "int"; "" otherwise.
    std::string valueParam{}; // "# GetParam() = 4" -> "4"; "" otherwise.

    // "Suite.Case", the spelling --gtest_filter and the reports use.
    [[nodiscard]] std::string fullName() const { return suite + "." + name; }

    friend bool operator==(const GTestCase&, const GTestCase&) = default;
};

/*
 * How a test ran, in ctest's terms:
 *   - Passed, Failed: the test completed and ctest (or Google Test) judged it. A timeout or a crash
 *     is Failed with ctest's reason ("Timeout", "Exception: SegFault").
 *   - Skipped: the test asked to be skipped (SKIP_RETURN_CODE, SKIP_REGULAR_EXPRESSION, GTEST_SKIP).
 *   - NotRun: ctest could not start it (no program: "Unable to find executable"); ctest counts it
 *     as failed, and so does Run Failed.
 *   - Disabled: the DISABLED property (or a Google Test DISABLED_ case that was not run).
 */
enum class TestOutcome : std::uint8_t { NotRun, Passed, Failed, Skipped, Disabled };

// "Passed", "Failed", "Skipped", "Not run" or "Disabled".
[[nodiscard]] std::string_view toString(TestOutcome outcome) noexcept;

// Whether Run Failed reruns a test with this outcome (Failed and NotRun, as ctest's --rerun-failed).
[[nodiscard]] bool countsAsFailure(TestOutcome outcome) noexcept;

// One failure message a test reported, with the location the tool printed for it.
struct TestFailure {
    std::string message{};
    std::optional<TestLocation> location{};

    friend bool operator==(const TestFailure&, const TestFailure&) = default;
};

// How one test, or one Google Test case inside a ctest test, ran.
struct TestResult {
    std::string name{}; // The ctest test name, or the case's full name ("Suite.Case") in `cases`.
    TestOutcome outcome = TestOutcome::NotRun;
    // The run time the tool reported; nullopt when it reported none.
    std::optional<std::chrono::microseconds> duration{};
    std::string reason{}; // ctest's verdict text ("Failed", "Timeout", "SKIP_RETURN_CODE=1"); "" for a pass.
    std::string output{}; // What the test printed (the JUnit report's system-out).
    std::vector<TestFailure> failures{}; // Google Test's failure messages, in order.
    // Where a Google Test case is defined (its report's file and line); nullopt for ctest's results.
    std::optional<TestLocation> definition{};
    // The Google Test cases that ran in this ctest test's process, from its JSON report, in order.
    // Empty for a test that is not a Google Test program and for a case itself.
    std::vector<TestResult> cases{};
    bool interim = false; // From ctest's console progress, not from its report (header comment).

    friend bool operator==(const TestResult&, const TestResult&) = default;
};

/*
 * "12 ms", "1.53 s", "2 min 05 s"; "" for nullopt. Microsecond durations below 1 ms read "< 1 ms".
 * Pure.
 */
[[nodiscard]] std::string formatTestDuration(std::optional<std::chrono::microseconds> duration);

} // namespace frost::run
