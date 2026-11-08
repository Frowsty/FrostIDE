// SPDX-License-Identifier: Apache-2.0
//
// RUN-7 on the pinned FrostEngine corpus (label integration corpus; verify -Corpus): the Unit
// Tests window of a workbench that opened and configured the clone lists exactly the tests
// `ctest --preset <pinned preset> -N` prints there, with the ctest of that configure. Nothing is
// built and no test runs; the clone is configured in place under its build lock.

#include "frost/build/cmake_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/test_results_tree.hpp"

#include "support/crash_dialogs.hpp"
#include "support/engine_lock.hpp"
#include "workbench_rig.hpp"
#include <QLabel>
#include <QPushButton>

#include <chrono>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

// <$FROST_CORPORA_ROOT>/frostengine-<sha12>, checked as corpus_test.cpp checks it; "" (with a
// failure added) when it is missing, so the corpus tier fails instead of skipping.
std::string pinnedCorpus() {
    const std::string root = qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString();
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus or "
                         "tools/verify.sh --corpus";
        return {};
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const std::string corpus = frost::io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
    const auto head = frost::io::realFileSystem().readFile(corpus + "/.git/HEAD");
    if (!head || head->substr(0, 40) != sha) {
        ADD_FAILURE() << corpus << " is missing or not detached at its pin (python tools/corpora.py verify)";
        return {};
    }
    return corpus;
}

// `ctest -N` prints "Test #n: name" (with " (Disabled)" after a disabled one).
std::vector<std::string> oracleList(const std::string& text) {
    std::vector<std::string> names;
    static const std::regex line(R"re(^\s*Test\s+#\d+: (.*?)(?: \(Disabled\))?\r?$)re");
    std::istringstream in(text);
    for (std::string row; std::getline(in, row);)
        if (std::smatch m; std::regex_match(row, m, line))
            names.push_back(m[1].str());
    return names;
}

TEST(CorpusFrostEngine, TheUnitTestsWindowListsWhatCTestPresetDashNPrints) {
    frost::test::disableCrashDialogs(); // cmake and ctest inherit the error mode.
    const std::string corpus = pinnedCorpus();
    ASSERT_FALSE(corpus.empty());
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const frost::test::EngineBuildLock buildLock(qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString() +
                                                 "/frostengine-" + sha.substr(0, 12) + ".build.lock");
    ASSERT_TRUE(buildLock.held()) << "the corpus build lock stayed taken (tools/corpora.py BuildLock)";

    WorkDir profileDir("corpus-frostengine-unit-tests");
    WorkbenchRig rig(profileDir);
    ASSERT_TRUE(rig.trust->trust(corpus).has_value());
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(corpus).has_value());
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_EQ(service->activeProfile()->name, FROST_CORPUS_FROSTENGINE_PRESET);
    ASSERT_TRUE(waitFor(
        [&] {
            return service->state() == frost::build::BuildState::Idle && service->lastConfigure().has_value() &&
                   service->model() != nullptr;
        },
        40min))
        << "the open's configure never ended";
    ASSERT_TRUE(service->lastConfigure()->succeeded())
        << (service->lastConfigure()->error ? service->lastConfigure()->error->describe() : "failed");
    const std::string ctest = service->model()->cacheValue("CMAKE_CTEST_COMMAND").value_or("");
    ASSERT_FALSE(ctest.empty()) << "the configure's cache names ctest";

    frost::io::ProcessSpec spec;
    spec.executable = frost::io::normalizePath(ctest).value_or(ctest);
    spec.arguments = {"--preset", FROST_CORPUS_FROSTENGINE_PRESET, "-N"};
    spec.workingDirectory = corpus;
    auto listed = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 5min);
    ASSERT_TRUE(listed.has_value()) << listed.error().describe();
    ASSERT_EQ(listed->exitCode, 0) << listed->stdoutBytes << listed->stderrBytes;
    const std::vector<std::string> expected = oracleList(listed->stdoutBytes);
    ASSERT_FALSE(expected.empty()) << listed->stdoutBytes;

    rig.window->toolWindows().show(frost::app::kUnitTestsToolWindow, false);
    QWidget* window = rig.window->toolWindows().content(frost::app::kUnitTestsToolWindow);
    ASSERT_NE(window, nullptr);
    auto* tree = window->findChild<frost::ui::TestResultsTree*>(QStringLiteral("unitTestsTreeWidget"));
    auto* summary = window->findChild<QLabel*>(QStringLiteral("unitTestsSummaryLabel"));
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(summary, nullptr);
    ASSERT_TRUE(waitFor([&] { return summary->text().contains(QStringLiteral("listed by ctest")); }, 5min))
        << summary->text().toStdString();
    EXPECT_EQ(tree->testNames(), expected);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("test preset %1").arg(FROST_CORPUS_FROSTENGINE_PRESET)))
        << summary->text().toStdString();
    // Only listed: nothing ran, so nothing shows an outcome.
    EXPECT_EQ(tree->counts().withoutResult, expected.size());
}

} // namespace
