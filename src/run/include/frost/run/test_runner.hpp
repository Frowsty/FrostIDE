// SPDX-License-Identifier: Apache-2.0
//
// TestRunner (RUN-7, PLAN.md Appendix C R2): discovers and runs a CMake build directory's tests with
// the real ctest, and lists a Google Test program's cases, through io::IProcessLauncher. Every test,
// outcome, duration and message it reports was parsed from those processes' output
// (ctest_parsers.hpp, gtest_parsers.hpp); when ctest fails without a report, the run reports the
// tool error and no results. Qt-free.
//
/*
 * Command lines (ctest 3.21+; the CTestSetup comes from the CMake model, ctestSetupFor):
 *
 *   discover:  ctest --test-dir <build> [-C <config>] [filters] -N --show-only=json-v1
 *   run:       ctest --test-dir <build> [-C <config>] [filters] [-j <n>] [--timeout <s>]
 *                    [--stop-on-failure] --no-tests=error --output-junit <report>
 *                    [--tests-from-file <list> | -R ^(<escaped names>)$]
 *   list:      <program> --gtest_list_tests   (in the test's working directory)
 *   doctest:   <test's command> --list-test-cases --reporters=xml --out=<report>
 *              <test's command> [--test-case=<names>] --reporters=xml --duration=true
 *                    --no-skipped-summary=true --out=<report>   (RUN-8, doctest_parsers.hpp)
 *
 * A failed ctest test whose command runs a doctest program (isDoctestCommand) gets the cases that
 * doctest's console output in ctest's report names as failed (parseDoctestConsoleFailures), with
 * their failure locations; passing cases have no result there (only a direct doctest run gives one).
 *
 * A run of some tests names them exactly: --tests-from-file with ctest 3.29+ (one name per line),
 * else an anchored -R of the names with every regular-expression character escaped. Each run
 * writes its report and the Google Test reports (GTEST_OUTPUT=json:<dir>/ in ctest's environment,
 * which every Google Test program it starts inherits) under <build>/Testing/Temporary/, ctest's own
 * scratch directory, in files named frostide-<pid>-<n>-*, and removes them once read.
 *
 * Outcome of a run (TestRunReport):
 *   - the report was written: its results (with the Google Test reports attached), whatever ctest's
 *     exit code (8 means "some tests failed", not a tool error);
 *   - stopped (cancel()): the tree is ended (ctest and every test it started); the results are the
 *     tests ctest's progress lines had reported as finished (interim), and stopped is set;
 *   - otherwise: `error` says how ctest failed (its exit code and the end of its output, or "wrote
 *     no output"), and there are no results.
 * While a run goes on, onTestStarted and onTestFinished report ctest's progress lines (interim
 * results); onFinished's report replaces them.
 *
 * One job (discover, run or list) at a time: another fails with Busy. Callbacks run on the
 * runner's threads, never two at a time, and onFinished/done is the job's last; they may call
 * busy() and cancel(), not start a job or destroy the runner (the UI posts to its own thread).
 * The launcher and file system must outlive the runner; the destructor cancels the job and waits
 * for it. The trust gate is the caller's: ctest runs the project's CTestTestfile.cmake scripts and
 * test programs, so only a trusted folder's setup may be passed.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/project/cmake_presets.hpp"
#include "frost/project/cmake_profile.hpp"
#include "frost/run/doctest_parsers.hpp"
#include "frost/run/framework_parsers.hpp"
#include "frost/run/lua_framework_parsers.hpp"
#include "frost/run/test_model.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::run {

// The ctest of one configured build directory, and the test preset's selection and settings.
struct CTestSetup {
    std::string ctest{};          // Absolute ctest program (the one that belongs to the configuring cmake).
    std::string ctestVersion{};   // "4.4.5"; "" when unknown (taken as current).
    std::string buildDirectory{}; // Absolute.
    std::string configuration{};  // -C; "" for single-configuration generators without a preset's.
    std::string testPreset{};     // The test preset the filters came from, for display; "" without one.
    std::string includeName{};    // -R
    std::string excludeName{};    // -E
    std::string includeLabel{};   // -L
    std::string excludeLabel{};   // -LE
    int jobs = 0;                 // -j; 0 leaves it to ctest.
    std::optional<int> timeout{}; // --timeout, seconds.
    bool stopOnFailure = false;   // --stop-on-failure.
    // Applied on top of this process's environment (nullopt unsets).
    std::vector<std::pair<std::string, std::optional<std::string>>> environment{};

    friend bool operator==(const CTestSetup&, const CTestSetup&) = default;
};

/*
 * The setup for `model`'s build directory as `profile` configures it: ctest is the model cache's
 * CMAKE_CTEST_COMMAND (else the ctest beside its CMAKE_COMMAND), -C the multi-config generator's
 * configuration, and `preset` (a test preset of the profile's configure preset, or null) adds its
 * configuration, filters, jobs, timeout, stop-on-failure and environment on top of the profile's.
 * Errors: NotFound (the cache names no ctest), InvalidArgument (a relative path), Unsupported
 * (cmake older than 3.21, which has no --output-junit). Pure.
 */
[[nodiscard]] Result<CTestSetup> ctestSetupFor(const project::CMakeProject& model, const project::CMakeProfile& profile,
                                               const project::CMakeTestPreset* preset);

// The test preset ctestSetupFor should use for `profile`: the visible, enabled test preset of the
// profile's configure preset named like its build or configure preset, else the first such one;
// null without one.
[[nodiscard]] const project::CMakeTestPreset* testPresetFor(const project::CMakePresets& presets,
                                                            const project::CMakeProfile& profile);

// The ctest arguments for discovery (header comment). Pure.
[[nodiscard]] std::vector<std::string> ctestDiscoveryArguments(const CTestSetup& setup);
// An anchored ctest regular expression matching exactly `names` (header comment). Pure.
[[nodiscard]] std::string ctestExactNameRegex(const std::vector<std::string>& names);

enum class TestCaseFramework : std::uint8_t { CTestWhole, GoogleTest, Doctest, Catch2, BoostTest, Busted, LuaUnit };

// Replaces doctest's case filters and case-sensitivity flags with exact, case-sensitive selection.
// Returns the full command, preserving its option prefix. Errors: InvalidArgument (empty command,
// empty case name), Unsupported (a case name contains '*' or '?', which doctest cannot escape). Pure.
[[nodiscard]] Result<std::vector<std::string>> doctestCaseSelectionCommand(const std::vector<std::string>& command,
                                                                           const std::vector<std::string>& cases);

struct SingleTestLaunch {
    std::string program{};
    std::vector<std::string> arguments{};
    std::string workingDirectory{};
    std::vector<std::pair<std::string, std::optional<std::string>>> environment{};
    std::string displayName{};
};

/*
 * Builds the direct launch specification for debugging or running a single ctest test or a single
 * Google Test / doctest case within `test` (RUN-10). Errors: InvalidArgument when `test` has no
 * absolute program or `setup` has no absolute build directory; Unsupported for a doctest case name
 * containing '*' or '?', which the tool cannot select literally. Pure.
 */
[[nodiscard]] Result<SingleTestLaunch>
buildSingleTestLaunch(const CTestSetup& setup, const DiscoveredTest& test, std::string_view caseName = {},
                      TestCaseFramework framework = TestCaseFramework::CTestWhole);

// A test case in a source file resolved to a ctest-discovered test (RUN-10).
struct GutterTestCase {
    DiscoveredTest test{};
    std::string caseName{};
    TestCaseFramework framework = TestCaseFramework::CTestWhole;
    int line = 0; // 1-based line in the source file.
    std::string displayName{};
};

/*
 * Maps test definitions in `filePath` (with source text `content`) to concrete runnable and
 * debuggable ctest-backed test cases (RUN-10, truth rule: only cases backed by `tests` are
 * returned). Pure.
 */
[[nodiscard]] std::vector<GutterTestCase>
resolveGutterTestCasesInFile(std::string_view filePath, std::string_view content, std::span<const DiscoveredTest> tests,
                             std::span<const std::pair<std::string, std::vector<DoctestCase>>> doctestCases = {},
                             std::span<const std::pair<std::string, std::vector<GTestCase>>> gtestCases = {},
                             std::string_view buildDirectory = {});

// How a run ended (header comment, "Outcome of a run").
struct TestRunReport {
    std::optional<Error> error{};
    bool stopped = false;
    std::optional<int> exitCode{}; // ctest's, when it ran to the end.
    std::string commandLine{};     // As started, for display.
    std::vector<TestResult> results{};
};

struct TestRunHandlers {
    std::function<void(const std::string& name)> onTestStarted{};
    std::function<void(const TestResult& interim)> onTestFinished{};
    // ctest's console output, as it arrives (UTF-8 chunks).
    std::function<void(std::string_view text)> onOutput{};
    std::function<void(const TestRunReport& report)> onFinished{};
};

struct TestRunnerOptions {
    IClock* clock = nullptr; // Null: systemClock().
};

class TestRunner {
public:
    [[nodiscard]] static std::unique_ptr<TestRunner> create(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                            TestRunnerOptions options = {});
    ~TestRunner();
    TestRunner(const TestRunner&) = delete ("owns a thread and a process tree");
    TestRunner& operator=(const TestRunner&) = delete ("owns a thread and a process tree");

    /*
     * Lists the tests (header comment). `done` gets them, or the tool error: the launcher's, IoError
     * (ctest exited non-zero; the message holds its exit code and the end of its output), Corrupt
     * (output that is no test list), Cancelled (cancel()). Errors here: Busy, InvalidArgument (a
     * setup without an absolute ctest or build directory).
     */
    [[nodiscard]] Result<void> discover(const CTestSetup& setup,
                                        std::function<void(Result<std::vector<DiscoveredTest>>)> done);

    /*
     * Runs `names` (every test of the setup when empty) of `tests`, the list discover() returned for
     * this setup (it maps progress lines and Google Test reports to tests). Errors here: Busy,
     * InvalidArgument; how the run went is the report's.
     */
    [[nodiscard]] Result<void> run(const CTestSetup& setup, std::vector<DiscoveredTest> tests,
                                   std::vector<std::string> names, TestRunHandlers handlers);

    /*
     * Lists the Google Test cases of `test`'s program (`--gtest_list_tests`, in its working directory,
     * with the setup's environment). `done` gets the cases or the error (as discover()'s, Corrupt
     * for output that is no case list). Errors here: Busy, InvalidArgument (a test without a program).
     */
    [[nodiscard]] Result<void> listGTestCases(const CTestSetup& setup, const DiscoveredTest& test,
                                              std::function<void(Result<std::vector<GTestCase>>)> done);

    /*
     * Lists the doctest cases of each of `tests` (RUN-8, doctest_parsers.hpp): its own command with
     * doctest's --list-test-cases and XML reporter (doctestListArguments), in its working directory,
     * with the setup's environment and its ENVIRONMENT, up to kDoctestListJobs at a time. `done` gets
     * one listing per test, in `tests`' order: its cases, or how listing it failed (the launcher's
     * error, IoError for a non-zero exit, Corrupt for a report that is no case list, Cancelled).
     * Errors here: Busy, InvalidArgument (a test without an absolute program).
     */
    struct DoctestListing {
        std::string test{};
        Result<std::vector<DoctestCase>> cases = std::vector<DoctestCase>{};
    };
    static constexpr std::size_t kDoctestListJobs = 4;
    [[nodiscard]] Result<void> listDoctestCases(const CTestSetup& setup, std::vector<DiscoveredTest> tests,
                                                std::function<void(std::vector<DoctestListing>)> done);

    /*
     * Runs doctest cases directly, without ctest (RUN-8): for each request, its test's command with
     * doctestRunArguments (the request's cases, all of the test's when empty), one process at a
     * time, in the test's working directory with the setup's environment and its ENVIRONMENT. Each
     * finished process gives its test's result (doctestRunResult: the cases from doctest's XML
     * report, the run time measured from launch to exit) through onTestFinished and in the report.
     * A process that writes no report gives no result, and `error` names it (the other tests keep
     * theirs). Stopping ends the running process; the report then has stopped set and the results so
     * far. Errors here: Busy, InvalidArgument (a request whose test has no absolute program).
     */
    struct DoctestRunRequest {
        DiscoveredTest test{};
        std::vector<std::string> cases{};
    };
    [[nodiscard]] Result<void> runDoctest(const CTestSetup& setup, std::vector<DoctestRunRequest> requests,
                                          TestRunHandlers handlers);

    /*
     * Runs Google Test cases directly, without ctest (RUN-10): for each request, its test's program
     * with gtestRunArguments (the request's cases, all of the test's when empty), one process at a
     * time, in the test's working directory with the setup's environment and its ENVIRONMENT. Each
     * finished process gives its test's result (gtestRunResult: the cases from Google Test's JSON
     * report, the run time measured from launch to exit) through onTestFinished and in the report.
     * A selected-case request must report exactly those cases; an empty or mismatched report is a
     * tool error, never a pass.
     */
    struct GTestRunRequest {
        DiscoveredTest test{};
        std::vector<std::string> cases{};
    };
    [[nodiscard]] Result<void> runGTest(const CTestSetup& setup, std::vector<GTestRunRequest> requests,
                                        TestRunHandlers handlers);

    // Explicit protocol selection; no filename/source-macro framework guessing.
    [[nodiscard]] Result<void> listFrameworkCases(const CTestSetup& setup, const DiscoveredTest& test,
                                                  FrameworkProfile profile,
                                                  std::function<void(Result<std::vector<FrameworkCase>>)> done);
    struct FrameworkRunRequest {
        DiscoveredTest test;
        FrameworkProfile profile = FrameworkProfile::Catch2Xml3;
        std::vector<std::string> cases;
    };
    [[nodiscard]] Result<void> runFramework(const CTestSetup& setup, std::vector<FrameworkRunRequest> requests,
                                            TestRunHandlers handlers);

    // LUA-12: same owned RunSession lifecycle as native frameworks, with an explicit Lua
    // configuration and caller-owned scratch directory. No CTest registration is fabricated.
    struct LuaFrameworkRequest {
        RunConfiguration configuration;
        LuaTestProfile profile = LuaTestProfile::Busted220;
        std::string scratchDirectory;
        std::string discoveryBridge;
        std::vector<std::string> cases;
    };
    [[nodiscard]] Result<void> listLuaFramework(LuaFrameworkRequest request,
                                                std::function<void(Result<std::vector<FrameworkCase>>)> done);
    [[nodiscard]] Result<void> runLuaFramework(LuaFrameworkRequest request, TestRunHandlers handlers);

    // Ends the current job's process tree; the job then finishes as stopped/Cancelled. Idempotent.
    void cancel();
    // Whether a job is running (until its last callback has returned).
    [[nodiscard]] bool busy() const;
    // Blocks until no job runs or `timeout` passes; returns whether it got there. Not from a callback.
    [[nodiscard]] bool waitForIdle(IClock::Duration timeout) const;

    struct State;

private:
    explicit TestRunner(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};

} // namespace frost::run
