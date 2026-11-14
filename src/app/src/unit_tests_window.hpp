// SPDX-License-Identifier: Apache-2.0
//
// The Unit Tests tool window (RUN-7, PLAN.md Appendix C R2): the tests the real ctest lists for the
// open CMake folder's configured build directory, run with ctest, their outcomes and durations from
// ctest's and Google Test's own reports (frost::run::TestRunner), in a ui::TestResultsTree. It
// replaces R0's source-text scan, which listed declarations as "Discovered, not run".
//
/*
 * Where the tests come from. The open CMake folder's model (the File API reply) and active profile
 * give run::ctestSetupFor its ctest, build directory and configuration; the test preset of the
 * profile's configure preset (run::testPresetFor) adds its filters, as `ctest --preset` would.
 * Without a CMake folder, before its first configure, or before the folder is trusted (ctest runs
 * the project's test scripts and programs; PLAN.md 3.5 lists test discovery as trusted-only), the
 * window lists nothing, says why, and every run action is disabled. A ctest that fails is shown as
 * that tool error, with no results.
 *
 * Refresh. The window lists the tests when it is created and on Refresh, and again when it sees a
 * new reply, profile or trust state (it checks once a second while visible and idle, a cheap model
 * snapshot): a finished configure shows the new list without a click. A run started after such a
 * change lists the tests first.
 *
 * Actions: Run All (every test of the setup), Run Selected (the tests under the selection; a group
 * runs its tests, a case its test), Run Failed (the tests whose last result failed or did not run,
 * as ctest --rerun-failed), Stop (ends ctest's whole process tree), Refresh, and on a test without
 * a --gtest_filter "List Google Test Cases" (its program's --gtest_list_tests). Debug Selected
 * launches one selected test or listed case through the debugger (RUN-10). Double-click opens a failure's location,
 * else where the test is defined (DEF_SOURCE_LINE, a case's report entry), else its add_test call.
 *
 * doctest (RUN-8). After each listing, every test whose command runs a doctest program
 * (run::isDoctestCommand; FrostEngine's one test per doctest suite) gets its cases listed below it,
 * from its own command with doctest's --list-test-cases (TestRunner::listDoctestCases); "List
 * doctest Cases" does the same for any other test. Run All, Run Failed and Run Selected on tests run
 * ctest, which gives one result per test and, for a failed doctest test, the cases doctest's output
 * names as failed. Run Selected on a selection of doctest cases (and doctest tests), and "Run with
 * doctest (Every Case's Result)", start the tests' programs directly with doctest's XML reporter
 * (TestRunner::runDoctest): every case that ran gets its outcome, duration and failure locations.
 * Double-click on a listed case opens where doctest says it is defined.
 *
 * Threads: the GUI thread only; the runner's callbacks are posted to it. Destroying the widget
 * stops a running job and waits for it.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/run/test_runner.hpp"

#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QWidget;

namespace frost::build {
class CMakeService;
} // namespace frost::build
namespace frost::io {
class IFileSystem;
class IProcessLauncher;
} // namespace frost::io
namespace frost::project {
class TrustStore;
} // namespace frost::project

namespace frost::app {

// What the Unit Tests window uses; every member must outlive the widget.
struct UnitTestsWindowContext {
    io::IProcessLauncher* launcher = nullptr;
    io::IFileSystem* fs = nullptr;
    project::TrustStore* trust = nullptr;
    // The open CMake folder's service, or null.
    std::function<build::CMakeService*()> cmake{};
    // Whether an MSBuild solution is open (its tests are not listed: they have no CTest directory).
    std::function<bool()> solutionOpen{};
    // Opens `path` (absolute) at 1-based `line`.
    std::function<void(const std::string& path, int line)> openLocation{};
    // Starts a debug session for a single test or case (RUN-10).
    std::function<Result<void>(const run::SingleTestLaunch& launch)> debugTest{};
    // Called when the discovered tests or listed cases change (RUN-10).
    std::function<void()> testsChanged{};
};

// Where the window's tests come from, or why it has none (project_unit_tests.cpp).
struct UnitTestsSource {
    std::optional<run::CTestSetup> setup{};
    QString unavailable{};
    std::string trustRoot{};
    std::uint64_t trustRevision = 0;
};

struct UnitTestsSelectionDetails {
    QString text{};
    std::vector<std::pair<QString, run::TestLocation>> locations{};
};

[[nodiscard]] std::string unitTestsSourceKey(const UnitTestsWindowContext& context);
[[nodiscard]] UnitTestsSource unitTestsSource(const UnitTestsWindowContext& context);
[[nodiscard]] std::optional<run::TestLocation>
unitTestsLocationOf(const std::string& testName, const std::string& caseName, const run::DiscoveredTest* test,
                    const run::TestResult* result, const std::vector<run::DoctestCase>* doctestCases);
[[nodiscard]] UnitTestsSelectionDetails
unitTestsDescribeSelection(const std::string& testName, const std::string& caseName, const run::DiscoveredTest* test,
                           const run::TestResult* result, const std::vector<run::DoctestCase>* doctestCases);

// Creates the window's widget (header comment). Its object name is "unitTestsToolWindow".
[[nodiscard]] QWidget* createUnitTestsToolWindowWidget(QWidget* parent, UnitTestsWindowContext context);

// These GUI-thread helpers accept the concrete CTest widget or a provider QTabWidget containing
// exactly one concrete CTest page, regardless of its selected tab. Null, unrelated widgets and
// ambiguous containers return empty cases or NotFound; object names do not confer provider type.
// Returns the ctest-backed gutter test cases for `filePath` and `content` from `unitTestsWidget`.
[[nodiscard]] std::vector<run::GutterTestCase>
unitTestsGutterCasesForFile(QWidget* unitTestsWidget, std::string_view filePath, std::string_view content);

// Runs a single gutter test case in `unitTestsWidget` (RUN-10).
[[nodiscard]] Result<void> runGutterTestCase(QWidget* unitTestsWidget, const run::GutterTestCase& testCase);

// Debugs a single gutter test case via `unitTestsWidget` (RUN-10).
[[nodiscard]] Result<void> debugGutterTestCase(QWidget* unitTestsWidget, const run::GutterTestCase& testCase);

} // namespace frost::app
