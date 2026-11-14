// SPDX-License-Identifier: Apache-2.0
//
// RUN-8 on the pinned FrostEngine corpus (label integration corpus; verify -Corpus): the engine's
// doctest suites in the Unit Tests window of a workbench that opened and configured the clone,
// against FrostTests itself (the clone's one doctest program, built in the shared build tree under
// the build lock: python tools/corpora.py engine --configure, then cmake --build build/<preset>
// --target FrostTests). Every expectation comes from doctest:
//   - each suite's case list equals what the suite's own command prints with --list-test-cases
//     (doctest's console reporter, not the XML one the window reads);
//   - running one suite's cases from the window gives each case the outcome doctest's JUnit
//     reporter gives it in a separate run of the same command, and a duration;
//   - a failing case injected into one of that suite's sources (the file is restored and
//     FrostTests rebuilt afterwards; the clone is git-clean at the end) is listed, fails when run
//     from the window with the location of its CHECK, opens the editor there, and a ctest run of
//     the suite names it as a failed case at the same location.
// The clone is configured in place under its build lock; only FrostTests is built.

#include "frost/build/cmake_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/run/doctest_parsers.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/test_results_tree.hpp"
#include "frost/vcs/git_locator.hpp"

#include "support/crash_dialogs.hpp"
#include "support/engine_lock.hpp"
#include "unit_tests_window.hpp"
#include "workbench_rig.hpp"
#include <QLabel>
#include <QPushButton>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
using frost::run::TestOutcome;

// <$FROST_CORPORA_ROOT>/frostengine-<sha12>, as corpus_unit_tests_test.cpp checks it; "" (with a
// failure added) when it is missing, so the corpus tier fails instead of skipping.
std::string pinnedCorpus() {
    const std::string root = qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString();
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus or "
                         "tools/verify.sh --corpus";
        return {};
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    std::string corpus = frost::io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
    const auto head = frost::io::realFileSystem().readFile(corpus + "/.git/HEAD");
    if (!head || head->substr(0, 40) != sha) {
        ADD_FAILURE() << corpus << " is missing or not detached at its pin (python tools/corpora.py verify)";
        return {};
    }
    return corpus;
}

frost::io::ProcessResult runTool(const std::string& program, std::vector<std::string> arguments,
                                 const std::string& directory, std::chrono::seconds timeout = 10min) {
    frost::io::ProcessSpec spec;
    spec.executable = program;
    spec.arguments = std::move(arguments);
    spec.workingDirectory = directory;
    auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, timeout);
    EXPECT_TRUE(result.has_value()) << program << ": " << (result ? "" : result.error().describe());
    // A process that did not start never passes as a clean exit.
    return result ? std::move(*result) : frost::io::ProcessResult{.exitCode = -1};
}

// The git the product resolves (frost::vcs::findGit, as corpus_test.cpp does); "" with a failure.
std::string gitProgram() {
    auto git = frost::vcs::findGit(frost::io::realFileSystem(), frost::vcs::defaultGitSearch());
    EXPECT_TRUE(git.has_value()) << (git ? "" : git.error().describe());
    return git ? git->path : std::string();
}

std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> out;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

bool isSeparator(const std::string& line) {
    return line.size() >= 20 && line.find_first_not_of('=') == std::string::npos;
}

/*
 * The oracle list: `<command> --list-test-cases` with doctest's console reporter, which writes
 * "[doctest] listing all test case names", a separator, one name per line, and a separator. It
 * writes into `listPath` (--out=, which also overrides the --out= of a command such as
 * FrostTests.SuccessOutput's).
 */
std::vector<std::string> consoleCaseList(const frost::run::DiscoveredTest& test, const std::string& listPath,
                                         std::string* error) {
    std::vector<std::string> arguments(test.command.begin() + 1, test.command.end());
    arguments.insert(arguments.end(), {"--list-test-cases", "--reporters=console", "--out=" + listPath});
    const auto listed = runTool(test.command.front(), arguments, test.workingDirectory, 5min);
    if (listed.exitCode != 0) {
        *error = std::format("{}: exit {}: {}{}", test.name, listed.exitCode, listed.stdoutBytes, listed.stderrBytes);
        return {};
    }
    std::vector<std::string> names;
    const std::string text = frost::io::realFileSystem().readFile(listPath).value_or("");
    const std::vector<std::string> all = lines(text);
    const auto header = std::ranges::find(all, "[doctest] listing all test case names");
    if (header == all.end() || header + 1 == all.end() || !isSeparator(*(header + 1))) {
        *error = test.name + ": no case listing in: " + text;
        return {};
    }
    for (auto it = header + 2; it != all.end() && !isSeparator(*it); ++it)
        names.push_back(*it);
    return names;
}

// The oracle verdicts: a JUnit report (doctest's other reporter) of `command`, case name -> failed.
std::map<std::string, bool> junitVerdicts(const frost::run::DiscoveredTest& test, const std::string& report) {
    std::vector<std::string> arguments(test.command.begin() + 1, test.command.end());
    arguments.insert(arguments.end(), {"--reporters=junit", "--out=" + report});
    static_cast<void>(runTool(test.command.front(), arguments, test.workingDirectory, 10min));
    const std::string text = frost::io::realFileSystem().readFile(report).value_or("");
    EXPECT_FALSE(text.empty()) << "doctest wrote no JUnit report " << report;
    std::map<std::string, bool> out;
    static const std::regex testcase(R"re(<testcase classname="[^"]*" name="([^"]*)"[^>]*?(/?)>)re");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), testcase); it != std::sregex_iterator(); ++it) {
        const std::size_t end = (*it)[2].str() == "/"
                                    ? std::string::npos
                                    : text.find("</testcase>", static_cast<std::size_t>(it->position()));
        const std::string body = end == std::string::npos ? std::string()
                                                          : text.substr(static_cast<std::size_t>(it->position()),
                                                                        end - static_cast<std::size_t>(it->position()));
        std::string name = (*it)[1].str();
        // The XML escapes doctest's writer applies to attribute values.
        for (const auto& [escaped, plain] :
             {std::pair{"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}}) {
            for (std::size_t at = name.find(escaped); at != std::string::npos; at = name.find(escaped, at + 1))
                name.replace(at, std::string_view(escaped).size(), plain);
        }
        out[name] = body.find("<failure") != std::string::npos || body.find("<error") != std::string::npos;
    }
    return out;
}

// Restores the injected source byte for byte and rebuilds FrostTests from it, even when the test
// stopped early, so the shared clone and its build stay as the pin has them.
class InjectionGuard {
public:
    InjectionGuard(std::string path, std::string original, std::function<void()> rebuild)
        : m_path(std::move(path)), m_original(std::move(original)), m_rebuild(std::move(rebuild)) {}
    ~InjectionGuard() {
        if (!m_armed)
            return;
        EXPECT_TRUE(frost::io::realFileSystem().writeFileAtomic(m_path, m_original).has_value()) << m_path;
        m_rebuild();
    }
    InjectionGuard(const InjectionGuard&) = delete ("restores once");
    InjectionGuard& operator=(const InjectionGuard&) = delete ("restores once");
    void arm() { m_armed = true; }

private:
    std::string m_path;
    std::string m_original;
    std::function<void()> m_rebuild;
    bool m_armed = false;
};

std::string sameSpelling(const std::string& path) {
    std::error_code error;
    return std::filesystem::weakly_canonical(frost::io::toStdPath(path).value_or(std::filesystem::path(path)), error)
        .generic_string();
}

TEST(CorpusFrostEngine, TheUnitTestsWindowListsRunsAndLocatesTheEnginesDoctestCases) {
    frost::test::disableCrashDialogs(); // cmake, ctest and FrostTests inherit the error mode.
    const std::string corpus = pinnedCorpus();
    ASSERT_FALSE(corpus.empty());
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const frost::test::EngineBuildLock buildLock(qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString() +
                                                 "/frostengine-" + sha.substr(0, 12) + ".build.lock");
    ASSERT_TRUE(buildLock.held()) << "the corpus build lock stayed taken (tools/corpora.py BuildLock)";
    const std::string git = gitProgram();
    ASSERT_FALSE(git.empty());
    // `git status --porcelain`, or a note when git did not run cleanly (never "" then).
    const auto gitStatus = [&]() -> std::string {
        const auto status = runTool(git, {"-C", corpus, "status", "--porcelain"}, corpus);
        return status.exitCode == 0 ? status.stdoutBytes
                                    : std::format("git status exited {}: {}", status.exitCode, status.stderrBytes);
    };
    ASSERT_EQ(gitStatus(), "") << "the clone is not clean before the test";

    WorkDir profileDir("corpus-frostengine-doctest");
    WorkbenchRig rig(profileDir);
    ASSERT_TRUE(rig.trust->trust(corpus).has_value());
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(corpus).has_value());
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_EQ(service->activeProfile()->name, FROST_CORPUS_FROSTENGINE_PRESET);
    const auto idle = [&] {
        return service->state() == frost::build::BuildState::Idle && service->lastConfigure().has_value() &&
               service->model() != nullptr;
    };
    ASSERT_TRUE(waitFor(idle, 40min)) << "the open's configure never ended";
    ASSERT_TRUE(service->lastConfigure()->succeeded())
        << (service->lastConfigure()->error ? service->lastConfigure()->error->describe() : "failed");

    const auto model = service->model(); // Kept: the index is into this model's targets.
    const auto frostTests = std::ranges::find(model->targets, "FrostTests", &frost::project::CMakeTarget::name);
    ASSERT_NE(frostTests, model->targets.end());
    const std::size_t frostTestsIndex = static_cast<std::size_t>(frostTests - model->targets.begin());
    const auto buildFrostTests = [&]() -> bool {
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        auto started = service->build(frost::build::BuildAction::Build, frostTestsIndex,
                                      [done](const frost::build::CMakeBuildOutcome& outcome) {
                                          done->set_value(outcome.result.success && !outcome.error);
                                      });
        EXPECT_TRUE(started.has_value()) << (started ? "" : started.error().describe());
        if (!started)
            return false;
        // The service reports on its own thread; the GUI thread keeps serving events meanwhile.
        const bool finished = waitFor([&] { return future.wait_for(0s) == std::future_status::ready; }, 90min);
        EXPECT_TRUE(finished) << "the FrostTests build never ended";
        return finished && future.get() && waitFor(idle, 5min);
    };
    /* corpora.py engine only configures ("the corpus tests build single small targets"), so a fresh
       tree lists FrostTests.NotBuilt and no doctest suite. Build it first; on a tree where it is
       current this is Ninja's no-op. Before this, the test passed only where an earlier run had
       left FrostTests built, and a fresh Linux tree waited out the 30 min listing limit. */
    ASSERT_TRUE(buildFrostTests()) << "FrostTests did not build in the configured engine tree";

    rig.window->toolWindows().show(frost::app::kUnitTestsToolWindow, false);
    QWidget* window = rig.window->toolWindows().content(frost::app::kUnitTestsToolWindow);
    ASSERT_NE(window, nullptr);
    auto* tree = window->findChild<frost::ui::TestResultsTree*>(QStringLiteral("unitTestsTreeWidget"));
    auto* summary = window->findChild<QLabel*>(QStringLiteral("unitTestsSummaryLabel"));
    auto* runSelected = window->findChild<QPushButton*>(QStringLiteral("unitTestsRunSelectedButton"));
    auto* refresh = window->findChild<QPushButton*>(QStringLiteral("unitTestsRefreshButton"));
    ASSERT_TRUE(tree && summary && runSelected && refresh);
    const auto listedDoctest = [&] { return summary->text().contains(QStringLiteral(" doctest case(s) in ")); };
    // ctest's listing comes first; without a doctest suite in it the window never lists cases, so
    // fail then with the listing's own words instead of waiting out the doctest limit.
    ASSERT_TRUE(
        waitFor([&] { return summary->text().contains(QStringLiteral(" test(s) listed by ctest in ")); }, 10min))
        << summary->text().toStdString();
    ASSERT_TRUE(std::ranges::any_of(tree->testNames(),
                                    [&](const std::string& name) {
                                        const auto* test = tree->test(name);
                                        return test && frost::run::isDoctestCommand(test->command);
                                    }))
        << "ctest lists no doctest suite after the FrostTests build (FrostTests.NotBuilt: "
        << (tree->test("FrostTests.NotBuilt") != nullptr) << "): " << summary->text().toStdString();
    ASSERT_TRUE(waitFor(listedDoctest, 30min)) << summary->text().toStdString();

    // The doctest suites ctest lists (FrostEngine's FrostTests.<suite> tests).
    std::vector<frost::run::DiscoveredTest> suites;
    for (const std::string& name : tree->testNames())
        if (const auto* test = tree->test(name); test && frost::run::isDoctestCommand(test->command))
            suites.push_back(*test);
    ASSERT_FALSE(suites.empty()) << "ctest lists no doctest suite; is FrostTests built? (FrostTests.NotBuilt: "
                                 << (tree->test("FrostTests.NotBuilt") != nullptr) << ")";

    // 1. Each suite's cases equal doctest's own console listing of the suite's command.
    const WorkDir scratch("corpus-frostengine-doctest-oracle");
    {
        std::vector<std::future<std::pair<std::vector<std::string>, std::string>>> oracles;
        std::size_t next = 0;
        const auto start = [&] {
            const frost::run::DiscoveredTest& test = suites[next];
            const std::string listPath = scratch.join(std::format("list-{}.txt", next++));
            oracles.push_back(std::async(std::launch::async, [test, listPath] {
                std::string error;
                auto names = consoleCaseList(test, listPath, &error);
                return std::pair{std::move(names), std::move(error)};
            }));
        };
        std::size_t total = 0;
        for (std::size_t i = 0; i < suites.size(); ++i) {
            while (next < suites.size() && next < i + 4)
                start(); // Four oracle listings at a time.
            auto [expected, error] = oracles[i].get();
            ASSERT_TRUE(error.empty()) << error;
            const auto* listed = tree->doctestCases(suites[i].name);
            ASSERT_NE(listed, nullptr) << suites[i].name << " has no doctest listing in the window";
            std::vector<std::string> names;
            for (const auto& testCase : *listed)
                names.push_back(testCase.name);
            EXPECT_EQ(names, expected) << suites[i].name;
            total += expected.size();
        }
        EXPECT_TRUE(
            summary->text().contains(QStringLiteral("%1 doctest case(s) in %2 test(s)").arg(total).arg(suites.size())))
            << summary->text().toStdString();
    }

    // 2. One small unit suite run from the window: every case's outcome equals doctest's JUnit verdict.
    const frost::run::DiscoveredTest* chosen = nullptr;
    std::size_t chosenCount = 0;
    for (const auto& suite : suites) {
        const auto* listed = tree->doctestCases(suite.name);
        // A plain FrostTests.<suite> test (FrostTests --no-intro=true --test-suite=<suite>) of label unit.
        if (suite.labels != std::vector<std::string>{"unit"} || suite.command.size() != 3 ||
            !suite.command.back().starts_with("--test-suite=") || !listed || listed->size() < 2 ||
            !listed->front().location)
            continue;
        if (!chosen || listed->size() < chosenCount || (listed->size() == chosenCount && suite.name < chosen->name)) {
            chosen = &suite;
            chosenCount = listed->size();
        }
    }
    ASSERT_NE(chosen, nullptr) << "no unit suite with two or more cases";
    const frost::run::DiscoveredTest suite = *chosen;
    const std::string suiteName = suite.command.back().substr(std::string_view("--test-suite=").size());
    ASSERT_TRUE(suite.command.back().starts_with("--test-suite=")) << suite.command.back();
    // Clicking Run Selected replaces the summary at once ("Running..." or "Listing..."); the run's end
    // writes "Ran ...", "Stopped ..." or "<kind> failed: ...".
    const auto awaitRun = [&](std::chrono::minutes timeout) {
        const auto ended = [&] {
            const QString text = summary->text();
            return text.startsWith(QStringLiteral("Ran ")) || text.startsWith(QStringLiteral("Stopped ")) ||
                   text.contains(QStringLiteral(" failed: ")) || text.contains(QStringLiteral("could not start"));
        };
        EXPECT_TRUE(waitFor(ended, timeout)) << summary->text().toStdString();
        EXPECT_TRUE(summary->text().startsWith(QStringLiteral("Ran "))) << summary->text().toStdString();
    };
    const auto runCases = [&](const std::vector<std::string>& cases) {
        tree->clearSelection();
        for (const std::string& testCase : cases) {
            QTreeWidgetItem* item = tree->itemFor(suite.name, testCase);
            EXPECT_NE(item, nullptr) << testCase;
            if (item)
                item->setSelected(true);
        }
        EXPECT_TRUE(runSelected->isEnabled());
        runSelected->click();
        EXPECT_FALSE(summary->text().startsWith(QStringLiteral("Ran "))) << "the run did not start";
        awaitRun(15min);
    };
    std::vector<std::string> caseNames;
    for (const auto& testCase : *tree->doctestCases(suite.name))
        if (!std::ranges::contains(caseNames, testCase.name))
            caseNames.push_back(testCase.name);
    runCases(caseNames);
    const auto* result = tree->result(suite.name);
    ASSERT_NE(result, nullptr) << summary->text().toStdString();
    const std::map<std::string, bool> verdicts = junitVerdicts(suite, scratch.join("junit.xml"));
    ASSERT_FALSE(verdicts.empty());
    for (const auto& [name, failed] : verdicts) {
        const auto found = std::ranges::find(result->cases, name, &frost::run::TestResult::name);
        ASSERT_NE(found, result->cases.end()) << name << " ran under doctest's JUnit reporter but not from the window";
        EXPECT_EQ(found->outcome, failed ? TestOutcome::Failed : TestOutcome::Passed) << name;
        EXPECT_TRUE(found->duration.has_value()) << name;
    }
    EXPECT_EQ(result->cases.size(), verdicts.size());

    // 3. An injected failing case in one of the suite's sources.
    const std::string source = tree->doctestCases(suite.name)->front().location->file;
    const std::string sourcePath = frost::io::normalizePath(source).value_or(source);
    auto original = frost::io::realFileSystem().readFile(sourcePath);
    ASSERT_TRUE(original.has_value()) << sourcePath;
    ASSERT_EQ(runTool(git, {"-C", corpus, "ls-files", "--error-unmatch", sourcePath}, corpus).exitCode, 0)
        << sourcePath << " is not a tracked file of the clone";
    const std::string newline = original->find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string injectedCase = "FrostIDE RUN-8 injected failure";
    std::string injected = *original;
    if (!injected.ends_with('\n'))
        injected += newline;
    // Lines 1..N, then a blank line, the TEST_CASE line, its brace and the CHECK.
    const int checkLine = static_cast<int>(std::ranges::count(injected, '\n')) + 4;
    injected += newline +
                std::format("TEST_CASE(\"{}\" * doctest::test_suite(\"{}\")){}{{{}", injectedCase, suiteName, newline,
                            newline) +
                "\tCHECK(1 == 2);" + newline + "}" + newline;
    InjectionGuard guard(sourcePath, *original, [&] {
        EXPECT_TRUE(buildFrostTests()) << "FrostTests did not rebuild from the restored source";
        EXPECT_EQ(gitStatus(), "") << "the clone is not clean after the test";
    });
    guard.arm();
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(sourcePath, injected).has_value());
    ASSERT_TRUE(buildFrostTests()) << "FrostTests did not build with the injected case";
    refresh->click();
    ASSERT_TRUE(waitFor([&] { return listedDoctest() && tree->itemFor(suite.name, injectedCase) != nullptr; }, 30min))
        << summary->text().toStdString();

    // The real doctest listing and its source location must produce one concrete gutter action;
    // clicking it runs only the injected case through FrostTests itself.
    const auto currentSource = frost::io::realFileSystem().readFile(sourcePath);
    ASSERT_TRUE(currentSource.has_value()) << sourcePath;
    const auto gutterCases = frost::app::unitTestsGutterCasesForFile(window, sourcePath, *currentSource);
    const auto gutterCase = std::ranges::find(gutterCases, injectedCase, &frost::run::GutterTestCase::caseName);
    ASSERT_NE(gutterCase, gutterCases.end()) << "the real doctest case listing should back this gutter marker";
    ASSERT_TRUE(frost::app::runGutterTestCase(window, *gutterCase).has_value());
    awaitRun(15min);
    result = tree->result(suite.name);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->cases.size(), 1u) << "one doctest gutter action runs exactly its selected case";
    EXPECT_EQ(result->cases.front().name, injectedCase);
    EXPECT_EQ(result->cases.front().outcome, TestOutcome::Failed);

    runCases({injectedCase});
    result = tree->result(suite.name);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->outcome, TestOutcome::Failed) << result->reason;
    ASSERT_EQ(result->cases.size(), 1u) << "only the selected case ran";
    const frost::run::TestResult& failedCase = result->cases.front();
    EXPECT_EQ(failedCase.name, injectedCase);
    EXPECT_EQ(failedCase.outcome, TestOutcome::Failed);
    ASSERT_EQ(failedCase.failures.size(), 1u);
    ASSERT_TRUE(failedCase.failures[0].location.has_value());
    EXPECT_EQ(sameSpelling(failedCase.failures[0].location->file), sameSpelling(sourcePath));
    EXPECT_EQ(failedCase.failures[0].location->line, checkLine);
    // Activating the failed case opens the editor at the CHECK.
    QTreeWidgetItem* item = tree->itemFor(suite.name, injectedCase);
    ASSERT_NE(item, nullptr);
    emit tree->itemActivated(item, 0);
    auto& tabs = rig.window->tabs();
    EXPECT_TRUE(waitFor(
        [&] {
            return tabs.currentEditor() && tabs.indexOf(sourcePath) == tabs.currentIndex() &&
                   tabs.currentEditor()->status().line == checkLine;
        },
        30s))
        << "the editor did not open " << sourcePath << ":" << checkLine;

    // A ctest run of the suite names the injected case as failed, at the same location.
    tree->clearSelection();
    tree->itemFor(suite.name)->setSelected(true);
    runSelected->click();
    awaitRun(30min);
    result = tree->result(suite.name);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->outcome, TestOutcome::Failed);
    const auto viaCTest = std::ranges::find(result->cases, injectedCase, &frost::run::TestResult::name);
    ASSERT_NE(viaCTest, result->cases.end()) << result->output;
    ASSERT_FALSE(viaCTest->failures.empty());
    ASSERT_TRUE(viaCTest->failures[0].location.has_value());
    EXPECT_EQ(sameSpelling(viaCTest->failures[0].location->file), sameSpelling(sourcePath));
    EXPECT_EQ(viaCTest->failures[0].location->line, checkLine);
    // The guard restores the source and rebuilds FrostTests; the clone is then clean.
}

} // namespace
