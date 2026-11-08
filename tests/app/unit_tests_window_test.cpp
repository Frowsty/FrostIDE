// SPDX-License-Identifier: Apache-2.0
//
// RUN-7 acceptance for the Unit Tests window over a WorkbenchRig (label integration, tag cmake): a
// trusted copy of tests/run/fixtures/ctest_project with a configure preset and a test preset that
// excludes one test, configured by the workbench's own CMake integration (the pinned cmake and
// Ninja). Every expectation comes from an oracle, the same folder's ctest run from this file:
//   - the window's list equals `ctest --preset fixture -N`;
//   - after Run All each test's status equals the verdict `ctest --preset fixture` prints;
//   - Run Failed runs exactly the tests whose verdict was a failure or "Not Run";
//   - a stub ctest that exits 1 and prints nothing gives that tool error and no test or result.

#include "frost/build/cmake_service.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/test_results_tree.hpp"

#include "workbench_rig.hpp"
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>

#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

// The fixture project, and presets whose test preset leaves out its 1 s timeout test.
void writeFixture(const WorkDir& dir) {
    std::ifstream in(std::string(FROST_RUN_FIXTURES) + "/ctest_project/CMakeLists.txt", std::ios::binary);
    ASSERT_TRUE(in) << FROST_RUN_FIXTURES;
    std::ostringstream text;
    text << in.rdbuf();
    dir.write("CMakeLists.txt", text.str());
    dir.write("CMakePresets.json", R"({"version": 3,
        "configurePresets": [{"name": "fixture", "generator": "Ninja", "binaryDir": "${sourceDir}/build"}],
        "testPresets": [{"name": "fixture", "configurePreset": "fixture",
                         "filter": {"exclude": {"name": "^slow$"}}}]})");
}

// Starts ctest through a launcher that sends every ctest(.exe) to the RUN-7 stub.
class StubCTestLauncher final : public frost::io::IProcessLauncher {
public:
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        return frost::io::realProcessLauncher().launch(redirect(spec));
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        return frost::io::realProcessLauncher().launchAsync(redirect(spec), context);
    }
    [[nodiscard]] int stubLaunches() const {
        const std::scoped_lock lock(m_mutex);
        return m_stubLaunches;
    }

private:
    frost::io::ProcessSpec redirect(const frost::io::ProcessSpec& spec) {
        std::string name(frost::io::fileName(spec.executable));
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name != "ctest" && name != "ctest.exe")
            return spec;
        frost::io::ProcessSpec stub = spec;
        stub.executable = frost::io::normalizePath(FROST_RUN_CTEST_STUB).value_or(FROST_RUN_CTEST_STUB);
        const std::scoped_lock lock(m_mutex);
        ++m_stubLaunches;
        return stub;
    }

    mutable std::mutex m_mutex;
    int m_stubLaunches = 0;
};

// What the window shows, found by object name.
struct Window {
    QWidget* root = nullptr;
    frost::ui::TestResultsTree* tree = nullptr;
    QLabel* summary = nullptr;
    QPlainTextEdit* details = nullptr;
    QPushButton* runAll = nullptr;
    QPushButton* runFailed = nullptr;
    QPushButton* refresh = nullptr;
    QPushButton* debug = nullptr;
};

Window showWindow(WorkbenchRig& rig) {
    rig.window->toolWindows().show(frost::app::kUnitTestsToolWindow, false);
    Window w;
    w.root = rig.window->toolWindows().content(frost::app::kUnitTestsToolWindow);
    if (!w.root)
        return w;
    w.tree = w.root->findChild<frost::ui::TestResultsTree*>(QStringLiteral("unitTestsTreeWidget"));
    w.summary = w.root->findChild<QLabel*>(QStringLiteral("unitTestsSummaryLabel"));
    w.details = w.root->findChild<QPlainTextEdit*>(QStringLiteral("unitTestsOutputView"));
    w.runAll = w.root->findChild<QPushButton*>(QStringLiteral("unitTestsRunAllButton"));
    w.runFailed = w.root->findChild<QPushButton*>(QStringLiteral("unitTestsRunFailedButton"));
    w.refresh = w.root->findChild<QPushButton*>(QStringLiteral("unitTestsRefreshButton"));
    w.debug = w.root->findChild<QPushButton*>(QStringLiteral("unitTestsDebugSelectedButton"));
    return w;
}

// The workbench configured the folder: its model names the ctest beside its cmake.
std::string configuredCTest(WorkbenchRig& rig) {
    auto* service = rig.workbench->cmakeService();
    if (!service || !waitFor([&] { return service->model() != nullptr; }, 180s))
        return {};
    return service->model()->cacheValue("CMAKE_CTEST_COMMAND").value_or("");
}

frost::io::ProcessResult runCTest(const std::string& ctest, const std::string& directory,
                                  std::vector<std::string> arguments) {
    frost::io::ProcessSpec spec;
    spec.executable = frost::io::normalizePath(ctest).value_or(ctest);
    spec.arguments = std::move(arguments);
    spec.workingDirectory = directory;
    auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, std::chrono::minutes(5));
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().describe());
    return result ? std::move(*result) : frost::io::ProcessResult{};
}

// The oracle list: `ctest -N` prints "Test #n: name" (with " (Disabled)" after a disabled one).
std::vector<std::string> oracleList(const std::string& text) {
    std::vector<std::string> names;
    static const std::regex line(R"re(^\s*Test\s+#\d+: (.*?)(?: \(Disabled\))?\r?$)re");
    std::istringstream in(text);
    for (std::string row; std::getline(in, row);)
        if (std::smatch m; std::regex_match(row, m, line))
            names.push_back(m[1].str());
    return names;
}

// The oracle verdicts: ctest's console line "i/n Test #k: name ....***Failed  0.01 sec", mapped to
// the status the window shows (run::toString of ctest's outcome): a timeout is a failure, "Not Run
// (Disabled)" is Disabled and a plain "Not Run" is "Not run".
std::map<std::string, QString> oracleVerdicts(const std::string& text) {
    std::map<std::string, QString> verdicts;
    static const std::regex line(
        R"re(^\s*\d+/\d+\s+Test\s+#\d+: (.*?) \.*\s*(?:\*\*\*)?(Passed|Failed|Skipped|Timeout|Not Run \(Disabled\)|Not Run)(?:\s.*)?$)re");
    std::istringstream in(text);
    for (std::string row; std::getline(in, row);) {
        if (!row.empty() && row.back() == '\r')
            row.pop_back();
        std::smatch m;
        if (!std::regex_match(row, m, line))
            continue;
        const std::string verdict = m[2].str();
        QString status = verdict == "Timeout"              ? QStringLiteral("Failed")
                         : verdict == "Not Run (Disabled)" ? QStringLiteral("Disabled")
                         : verdict == "Not Run"            ? QStringLiteral("Not run")
                                                           : QString::fromStdString(verdict);
        verdicts.emplace(m[1].str(), status);
    }
    return verdicts;
}

// The outcome part of a status cell ("Skipped (SKIP_RETURN_CODE=1)" -> "Skipped").
QString outcomeOf(const QString& status) { return status.section(QStringLiteral(" ("), 0, 0); }

TEST(UnitTestsWindow, ListsCTestsTestsAndShowsTheVerdictsCTestPrints) {
    WorkDir dir("unit-tests-window");
    writeFixture(dir);
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    const std::string ctest = configuredCTest(rig);
    ASSERT_FALSE(ctest.empty()) << "the workbench configured the folder and its cache names ctest";

    Window w = showWindow(rig);
    ASSERT_NE(w.tree, nullptr);
    ASSERT_NE(w.summary, nullptr);
    ASSERT_NE(w.runAll, nullptr);
    ASSERT_NE(w.runFailed, nullptr);
    ASSERT_NE(w.debug, nullptr);
    const std::vector<std::string> expected =
        oracleList(runCTest(ctest, dir.path(), {"--preset", "fixture", "-N"}).stdoutBytes);
    ASSERT_FALSE(expected.empty()) << "ctest --preset fixture -N lists the fixture's tests";
    ASSERT_TRUE(waitFor([&] { return !w.tree->testNames().empty(); }, 60s)) << w.summary->text().toStdString();
    EXPECT_EQ(w.tree->testNames(), expected) << "the window lists what ctest --preset lists";
    EXPECT_TRUE(w.summary->text().contains(QStringLiteral("test preset fixture"))) << w.summary->text().toStdString();
    // Nothing ran yet: no test shows an outcome.
    for (const std::string& name : w.tree->testNames()) {
        ASSERT_NE(w.tree->itemFor(name), nullptr) << name;
        EXPECT_TRUE(w.tree->itemFor(name)->text(1).isEmpty()) << name;
    }
    EXPECT_FALSE(w.debug->isEnabled());
    EXPECT_TRUE(w.debug->toolTip().contains(QStringLiteral("RUN-10")));
    EXPECT_FALSE(w.runFailed->isEnabled()) << "no test failed yet";

    ASSERT_TRUE(w.runAll->isEnabled()) << w.runAll->toolTip().toStdString();
    w.runAll->click();
    ASSERT_TRUE(waitFor([&] { return w.summary->text().startsWith(QStringLiteral("Ran ")); }, 120s))
        << w.summary->text().toStdString();
    const std::map<std::string, QString> verdicts =
        oracleVerdicts(runCTest(ctest, dir.path(), {"--preset", "fixture"}).stdoutBytes);
    ASSERT_EQ(verdicts.size(), expected.size()) << "ctest printed a verdict for every listed test";
    std::vector<std::string> failed;
    for (const auto& [name, status] : verdicts) {
        ASSERT_NE(w.tree->itemFor(name), nullptr) << name;
        EXPECT_EQ(outcomeOf(w.tree->itemFor(name)->text(1)), status) << name;
        if (status == QStringLiteral("Failed") || status == QStringLiteral("Not run"))
            failed.push_back(name);
    }
    EXPECT_TRUE(w.summary->text().contains(QStringLiteral("%1 failed").arg(failed.size())))
        << w.summary->text().toStdString();
    ASSERT_FALSE(failed.empty()) << "the fixture has failing tests";

    // Run Failed runs exactly the failures again; the other tests keep their results.
    ASSERT_TRUE(w.runFailed->isEnabled());
    w.runFailed->click();
    EXPECT_FALSE(w.runAll->isEnabled()) << "a run is going on";
    ASSERT_TRUE(waitFor([&] { return w.runAll->isEnabled(); }, 120s)) << w.summary->text().toStdString();
    EXPECT_TRUE(w.summary->text().startsWith(QStringLiteral("Ran %1 test(s):").arg(failed.size())))
        << w.summary->text().toStdString();
    for (const auto& [name, status] : verdicts)
        EXPECT_EQ(outcomeOf(w.tree->itemFor(name)->text(1)), status) << name;
}

TEST(UnitTestsWindow, AStubCTestThatExitsOneGivesTheToolErrorAndNoResults) {
    WorkDir dir("unit-tests-window-stub");
    writeFixture(dir);
    StubCTestLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_FALSE(configuredCTest(rig).empty());

    Window w = showWindow(rig);
    ASSERT_NE(w.tree, nullptr);
    ASSERT_NE(w.summary, nullptr);
    ASSERT_NE(w.details, nullptr);
    ASSERT_TRUE(waitFor([&] { return w.summary->text().startsWith(QStringLiteral("ctest could not list")); }, 60s))
        << w.summary->text().toStdString();
    EXPECT_GE(launcher.stubLaunches(), 1);
    EXPECT_TRUE(w.details->toPlainText().contains(QStringLiteral("code 1"))) << w.details->toPlainText().toStdString();
    EXPECT_TRUE(w.tree->testNames().empty());
    EXPECT_EQ(w.tree->topLevelItemCount(), 0);

    // Run All runs the stub too: the error, and still no result.
    ASSERT_TRUE(w.runAll->isEnabled());
    const int before = launcher.stubLaunches();
    w.runAll->click();
    ASSERT_TRUE(waitFor([&] { return w.summary->text().startsWith(QStringLiteral("ctest failed")); }, 60s))
        << w.summary->text().toStdString();
    EXPECT_GT(launcher.stubLaunches(), before);
    EXPECT_TRUE(w.details->toPlainText().contains(QStringLiteral("code 1"))) << w.details->toPlainText().toStdString();
    EXPECT_TRUE(w.tree->testNames().empty());
    EXPECT_EQ(w.tree->counts().passed + w.tree->counts().failed + w.tree->counts().skipped, 0U);
    EXPECT_EQ(w.tree->topLevelItemCount(), 0);
}

} // namespace
