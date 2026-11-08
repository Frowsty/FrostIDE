// SPDX-License-Identifier: Apache-2.0
//
// The workbench with the real clangd (label "clangd"; W1, ADR 0044): a folder project with a
// compile_commands.json under build/ and a main.cpp that names an undeclared identifier. Opening
// main.cpp shows clangd's error as a Live row in Problems, a squiggle (range decoration) and a
// gutter marker; typing the declaration clears all three. A second folder checks the language
// controller: Ctrl+B and Ctrl+click open a declaration, Alt+F7 shows Find Usages. A third checks
// the readiness that empty answers name (fallback flags, indexing) against clangd itself: its
// `--check` verdict on each file's command and the $/progress it writes.

#include "frost/io/async_process.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/find/find_popup.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QSet>
#include <QTemporaryDir>
#include <QTreeView>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
using namespace std::chrono_literals;

constexpr auto kPatience = 60s; // clangd parses this file in well under a second; this is slack.

// The Live rows of Problems that are errors.
std::vector<frost::ui::Problem> liveErrors(const frost::app::Workbench& workbench) {
    std::vector<frost::ui::Problem> rows;
    for (const frost::ui::Problem& problem : workbench.problems()->model()->problems()) {
        if (problem.source == frost::ui::DiagnosticSource::Live &&
            problem.diagnostic.diagnostic.severity == frost::build::Severity::Error) {
            rows.push_back(problem);
        }
    }
    return rows;
}

// The gutter markers that report diagnostics (errors and warnings), without bookmarks and run markers.
std::vector<frost::ui::LineMarker> diagnosticMarkers(const std::vector<frost::ui::LineMarker>& markers) {
    std::vector<frost::ui::LineMarker> rows;
    for (const frost::ui::LineMarker& marker : markers) {
        if (marker.severity == frost::ui::MarkerSeverity::Error ||
            marker.severity == frost::ui::MarkerSeverity::Warning) {
            rows.push_back(marker);
        }
    }
    return rows;
}

// The lines of the gutter's run markers (tests and entry points).
std::vector<frost::LineIndex> runMarkerLines(const std::vector<frost::ui::LineMarker>& markers) {
    std::vector<frost::LineIndex> lines;
    for (const frost::ui::LineMarker& marker : markers) {
        if (marker.severity == frost::ui::MarkerSeverity::RunTest) {
            lines.push_back(marker.line);
        }
    }
    return lines;
}

TEST(WorkbenchClangd, LiveDiagnosticsAppearAndClearAfterTheFix) {
    frost::test::disableCrashDialogs(); // clangd inherits the error mode: no WER box if it dies.
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << found.error().describe();

    WorkDir dir("clangd");
    dir.write("main.cpp", "int main() {\n"
                          "    return missing_value;\n"
                          "}\n"
                          "#if 0\nint disabled_name;\n#endif\n");
    const std::string main = dir.join("main.cpp");
    const std::vector<frost::lang::CompileCommand> commands = {frost::lang::CompileCommand{
        .directory = dir.path(), .file = main, .arguments = {"clang++", "-std=c++20", "-c", main}}};
    ASSERT_TRUE(
        frost::lang::writeCompileCommands(frost::io::realFileSystem(), dir.join("build"), commands).has_value());

    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value()); // The folder is the project.
    ASSERT_NE(rig.workbench->clangd(), nullptr);
    ASSERT_TRUE(rig.workbench->openPath(main).has_value());
    frost::app::DocumentEditorView* view = rig.view();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(waitFor([&] { return !view->editor().semanticTokens().empty(); }, kPatience));
    ASSERT_TRUE(waitFor([&] { return !view->editor().inactiveRanges().empty(); }, kPatience));

    ASSERT_TRUE(waitFor([&] { return !liveErrors(*rig.workbench).empty(); }, kPatience))
        << "no live diagnostic; clangd state " << static_cast<int>(rig.workbench->clangd()->state());
    const std::vector<frost::ui::Problem> errors = liveErrors(*rig.workbench);
    EXPECT_EQ(errors.front().diagnostic.diagnostic.line, 2);
    EXPECT_NE(errors.front().diagnostic.diagnostic.message.find("missing_value"), std::string::npos);
    ASSERT_TRUE(waitFor([&] { return !view->editor().diagnostics().empty(); }));
    const frost::ui::ActiveDiagnostic squiggle = view->editor().diagnostics().front();
    EXPECT_EQ(squiggle.severity, frost::ui::InsightSeverity::Error);
    EXPECT_EQ(view->editor().document().text().text(squiggle.range), "missing_value");
    const std::vector<frost::ui::LineMarker> before = diagnosticMarkers(view->editor().lineMarkers());
    ASSERT_EQ(before.size(), 1U);
    EXPECT_EQ(before.front().line, frost::LineIndex{1});
    EXPECT_EQ(before.front().severity, frost::ui::MarkerSeverity::Error);
    // The gutter's run marker for main() (the test/entry-point gutter) sits beside it, on line 1.
    EXPECT_EQ(runMarkerLines(view->editor().lineMarkers()), std::vector<frost::LineIndex>{frost::LineIndex{0}});

    // The fix, typed into the editor: clangd gets the change and publishes no error.
    view->editor().goToLine(1, 1);
    ASSERT_TRUE(view->editor().typeText("int missing_value = 0;\n"));
    ASSERT_TRUE(
        waitFor([&] { return liveErrors(*rig.workbench).empty() && view->editor().diagnostics().empty(); }, kPatience));
    // No diagnostic marker is left behind; the run marker followed main() down one line.
    EXPECT_TRUE(diagnosticMarkers(view->editor().lineMarkers()).empty());
    EXPECT_EQ(runMarkerLines(view->editor().lineMarkers()), std::vector<frost::LineIndex>{frost::LineIndex{1}});
}

TEST(WorkbenchClangd, FindReplaceUpdatesLiveDiagnostics) {
    frost::test::disableCrashDialogs();
    WorkDir dir("clangd-find");
    dir.write("main.cpp", "int main() { return missing_find_value; }\n");
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp")));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return !liveErrors(*rig.workbench).empty(); }, kPatience));
    auto* find = rig.window->findChild<frost::ui::FindFeature*>();
    ASSERT_NE(find, nullptr);
    auto* popup = find->showPopup(true);
    popup->queryField()->setText(QStringLiteral("missing_find_value"));
    popup->replacementField()->setText(QStringLiteral("0"));
    popup->searchNow();
    popup->session()->wait();
    ASSERT_EQ(popup->session()->matchCount(), 1U);
    auto result = find->replace(*popup, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->replacements, 1U);
    ASSERT_TRUE(waitFor([&] { return liveErrors(*rig.workbench).empty(); }, kPatience));
    EXPECT_TRUE(rig.view()->document().dirty());
}

TEST(WorkbenchClangd, ConfigurationSwitchRestartsInsightForOpenEditors) {
    frost::test::disableCrashDialogs();
    WorkDir dir("clangd-restart", "tiny_sln");
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/main.cpp")).has_value());
    auto* view = rig.view();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(waitFor([&] { return !view->editor().semanticTokens().empty(); }, kPatience));
    ASSERT_TRUE(rig.workbench->build().setActiveConfiguration(1).has_value());
    EXPECT_TRUE(view->editor().insightEnabled());
    ASSERT_TRUE(waitFor([&] { return !view->editor().semanticTokens().empty(); }, kPatience));
}

TEST(WorkbenchClangd, ClosingProjectCancelsHeldMsBuildEvaluation) {
    if constexpr (frost::io::kNativePathStyle != frost::io::PathStyle::Windows)
        GTEST_SKIP() << "MSBuild evaluation is Windows-only";
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    if (!found)
        GTEST_SKIP() << "clangd is unavailable";
    WorkDir dir("clangd-held-evaluation", "tiny_sln");
    frost::io::ScriptedProcessLauncher launcher;
    const auto msbuild = dir.join("held-msbuild.exe");
    launcher.expect({.expectExecutable = msbuild, .neverExits = true, .outputHeld = true});
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = true;
    options.workbench.workers = 1; // Cancellation must not wait for a second scheduler worker.
    options.workbench.msbuildPath = msbuild;
    WorkbenchRig rig(dir, std::move(options));
    // Ambient language servers share the launcher but are unrelated to this cancellation test.
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(waitFor([&] { return !launcher.launches().empty(); }, 10s));
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(rig.workbench->closeProject());
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
    const bool terminated = waitFor(
        [&] {
            const auto launches = launcher.launches();
            return launches.size() == 1 && launches.front().terminated;
        },
        10s);
    if (!terminated)
        for (const auto& launch : launcher.launches())
            ADD_FAILURE() << "recorded executable: " << launch.spec.executable << " terminated=" << launch.terminated;
    ASSERT_TRUE(terminated);
    EXPECT_EQ(launcher.launches().size(), 1U); // No fallback process or clangd launch after cancellation.
}

TEST(WorkbenchClangd, FirstEvaluatedProjectIsAvailableBeforeSecondCompletes) {
    if constexpr (frost::io::kNativePathStyle != frost::io::PathStyle::Windows)
        GTEST_SKIP() << "MSBuild evaluation is Windows-only";
    WorkDir dir("clangd-first-project-stream", "tiny_sln");
    frost::io::ScriptedProcessLauncher launcher;
    const auto msbuild = dir.join("held-msbuild.exe");
    launcher.expect(
        {.expectExecutable = msbuild,
         .stdoutBytes =
             R"({"Properties":{"Platform":"x64"},"Items":{"ClCompile":[{"FullPath":"C:\\fixture\\first.cpp"}],"ClInclude":[{"FullPath":"C:\\fixture\\first.hpp"}]}})"});
    launcher.expect({.expectExecutable = msbuild, .neverExits = true, .outputHeld = true});
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = false;
    options.workbench.workers = 1;
    options.workbench.msbuildPath = msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->streamedProjectContexts().size() == 2 && launcher.launches().size() == 2; }, 10s));
    const auto streamed = rig.workbench->streamedProjectContexts();
    ASSERT_EQ(streamed.size(), 2u);
    EXPECT_EQ(streamed[0]->state, frost::lang::ProjectEvaluationState::Evaluated);
    EXPECT_EQ(streamed[1]->state, frost::lang::ProjectEvaluationState::Pending);
    EXPECT_EQ(streamed[0]->projectPath, dir.join("Lib/Lib.vcxproj"));
    EXPECT_FALSE(streamed[0]->commands.empty());
    auto* explorer = rig.window->findChild<frost::ui::SolutionExplorer*>();
    ASSERT_NE(explorer, nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            return explorer->solution() && explorer->solution()->findFileNode("C:\\fixture\\first.cpp") &&
                   explorer->solution()->findFileNode("C:\\fixture\\first.hpp");
        },
        10s));
    EXPECT_TRUE(explorer->selectPath("C:\\fixture\\first.cpp"));
    ASSERT_TRUE(rig.workbench->closeProject());
    EXPECT_TRUE(rig.workbench->streamedProjectContexts().empty());
    ASSERT_TRUE(waitFor([&] { return launcher.launches().back().terminated; }, 10s));
}

TEST(WorkbenchClangd, RevokeAndRegrantCannotReviveHeldProjectContext) {
    if constexpr (frost::io::kNativePathStyle != frost::io::PathStyle::Windows)
        GTEST_SKIP() << "MSBuild evaluation is Windows-only";
    WorkDir dir("clangd-trust-generation", "tiny_sln");
    frost::io::ScriptedProcessLauncher launcher;
    const auto msbuild = dir.join("held-msbuild.exe");
    launcher.expect(
        {.expectExecutable = msbuild,
         .stdoutBytes =
             R"({"Properties":{"Platform":"x64"},"Items":{"ClCompile":[{"FullPath":"C:\\fixture\\old.cpp"}]}})"});
    launcher.expect({.expectExecutable = msbuild, .neverExits = true, .outputHeld = true});
    launcher.expect(
        {.expectExecutable = msbuild,
         .stdoutBytes =
             R"({"Properties":{"Platform":"x64"},"Items":{"ClCompile":[{"FullPath":"C:\\fixture\\fresh.cpp"}]}})"});
    launcher.expect({.expectExecutable = msbuild, .neverExits = true, .outputHeld = true});
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = false;
    options.workbench.workers = 1;
    options.workbench.msbuildPath = msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->streamedProjectContexts().size() == 2 && launcher.launches().size() == 2; }, 10s));
    auto* explorer = rig.window->findChild<frost::ui::SolutionExplorer*>();
    ASSERT_NE(explorer, nullptr);
    ASSERT_TRUE(waitFor(
        [&] { return explorer->solution() && explorer->solution()->findFileNode("C:\\fixture\\old.cpp"); }, 10s));
    ASSERT_TRUE(rig.trust->untrust(dir.path()));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    EXPECT_TRUE(rig.workbench->streamedProjectContexts().empty());
    ASSERT_TRUE(waitFor(
        [&] {
            const auto records = rig.workbench->streamedProjectContexts();
            return records.size() == 2 && !records[0]->commands.empty() &&
                   records[0]->commands[0].file.find("fresh.cpp") != std::string::npos &&
                   launcher.launches().size() == 4;
        },
        10s));
    EXPECT_TRUE(launcher.launches()[1].terminated);
    ASSERT_TRUE(waitFor(
        [&] {
            return explorer->solution() && !explorer->solution()->findFileNode("C:\\fixture\\old.cpp") &&
                   explorer->solution()->findFileNode("C:\\fixture\\fresh.cpp");
        },
        10s));
    ASSERT_TRUE(rig.workbench->closeProject());
    ASSERT_TRUE(waitFor([&] { return launcher.launches().back().terminated; }, 10s));
}

TEST(WorkbenchClangd, ConfigurationChangeRejectsHeldEvaluation) {
    if constexpr (frost::io::kNativePathStyle != frost::io::PathStyle::Windows)
        GTEST_SKIP() << "MSBuild evaluation is Windows-only";
    WorkDir dir("project-stream-configuration", "tiny_sln");
    frost::io::ScriptedProcessLauncher launcher;
    const auto msbuild = dir.join("held-msbuild.exe");
    launcher.expect({.expectExecutable = msbuild,
                     .stdoutBytes = R"({"Items":{"ClCompile":[{"FullPath":"C:\\fixture\\old-config.cpp"}]}})"});
    launcher.expect({.expectExecutable = msbuild, .neverExits = true, .outputHeld = true});
    launcher.expect({.expectExecutable = msbuild,
                     .stdoutBytes = R"({"Items":{"ClCompile":[{"FullPath":"C:\\fixture\\new-config.cpp"}]}})"});
    launcher.expect({.expectExecutable = msbuild, .neverExits = true, .outputHeld = true});
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = false;
    options.workbench.workers = 1;
    options.workbench.msbuildPath = msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(waitFor([&] { return launcher.launches().size() == 2; }, 10s));
    ASSERT_TRUE(rig.workbench->build().setActiveConfiguration(1));
    auto* explorer = rig.window->findChild<frost::ui::SolutionExplorer*>();
    ASSERT_NE(explorer, nullptr);
    const bool refreshed = waitFor(
        [&] {
            return launcher.launches().size() == 4 && explorer->solution() &&
                   explorer->solution()->findFileNode("C:\\fixture\\new-config.cpp") &&
                   !explorer->solution()->findFileNode("C:\\fixture\\old-config.cpp");
        },
        10s);
    ASSERT_TRUE(refreshed)
        << "launches=" << launcher.launches().size() << " pending=" << launcher.pendingScripts()
        << " model=" << static_cast<bool>(explorer->solution())
        << " new=" << (explorer->solution() && explorer->solution()->findFileNode("C:\\fixture\\new-config.cpp"))
        << " old=" << (explorer->solution() && explorer->solution()->findFileNode("C:\\fixture\\old-config.cpp"));
    EXPECT_TRUE(launcher.launches()[1].terminated);
    ASSERT_TRUE(rig.workbench->closeProject());
    ASSERT_TRUE(waitFor([&] { return launcher.launches().back().terminated; }, 10s));
}

TEST(WorkbenchClangd, OpeningAnotherRootRejectsHeldEvaluation) {
    if constexpr (frost::io::kNativePathStyle != frost::io::PathStyle::Windows)
        GTEST_SKIP() << "MSBuild evaluation is Windows-only";
    WorkDir dir("project-stream-root-switch", "tiny_sln");
    dir.write("Other/placeholder.txt", "other root");
    frost::io::ScriptedProcessLauncher launcher;
    const auto msbuild = dir.join("held-msbuild.exe");
    launcher.expect({.expectExecutable = msbuild,
                     .stdoutBytes = R"({"Items":{"ClCompile":[{"FullPath":"C:\\fixture\\old-root.cpp"}]}})"});
    launcher.expect({.expectExecutable = msbuild, .neverExits = true, .outputHeld = true});
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = false;
    options.workbench.workers = 1;
    options.workbench.msbuildPath = msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto* explorer = rig.window->findChild<frost::ui::SolutionExplorer*>();
    ASSERT_NE(explorer, nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            return launcher.launches().size() == 2 && explorer->solution() &&
                   explorer->solution()->findFileNode("C:\\fixture\\old-root.cpp");
        },
        10s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Other")));
    EXPECT_TRUE(rig.workbench->streamedProjectContexts().empty());
    ASSERT_TRUE(waitFor([&] { return launcher.launches()[1].terminated; }, 10s));
    EXPECT_FALSE(explorer->solution());
}

TEST(WorkbenchClangd, ProjectEvaluationStreamsWhenClangdDisabled) {
    if constexpr (frost::io::kNativePathStyle != frost::io::PathStyle::Windows)
        GTEST_SKIP() << "MSBuild evaluation is Windows-only";
    WorkDir dir("project-stream-no-clangd", "tiny_sln");
    frost::io::ScriptedProcessLauncher launcher;
    const auto msbuild = dir.join("held-msbuild.exe");
    launcher.expect(
        {.expectExecutable = msbuild,
         .stdoutBytes =
             R"({"Properties":{"Platform":"x64"},"Items":{"ClCompile":[{"FullPath":"C:\\fixture\\first.cpp"}]}})"});
    launcher.expect({.expectExecutable = msbuild, .neverExits = true, .outputHeld = true});
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = false;
    options.workbench.workers = 1;
    options.workbench.msbuildPath = msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->streamedProjectContexts().size() == 2 && launcher.launches().size() == 2; }, 10s));
    EXPECT_EQ(rig.workbench->clangd(), nullptr);
    ASSERT_TRUE(rig.workbench->closeProject());
    ASSERT_TRUE(waitFor([&] { return launcher.launches().back().terminated; }, 10s));
}

class ReleaseGatedProcess final : public frost::io::IProcess {
public:
    ReleaseGatedProcess(std::unique_ptr<frost::io::IProcess> inner, std::atomic<bool>& release)
        : m_inner(std::move(inner)), m_release(release) {}
    frost::io::ProcessId pid() const noexcept override { return m_inner->pid(); }
    frost::Result<void> writeStdin(std::string_view bytes) override { return m_inner->writeStdin(bytes); }
    void closeStdin() noexcept override { m_inner->closeStdin(); }
    frost::Result<std::size_t> readStdout(std::span<char> bytes) override { return m_inner->readStdout(bytes); }
    frost::Result<std::size_t> readStderr(std::span<char> bytes) override { return m_inner->readStderr(bytes); }
    frost::Result<frost::io::ExitStatus> wait(std::optional<frost::IClock::Duration> timeout) override {
        if (!m_release.load() && !m_terminated.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return frost::fail(frost::ErrorCode::Timeout, "held by test");
        }
        return m_inner->wait(timeout);
    }
    frost::Result<void> terminate() override {
        m_terminated.store(true);
        return m_inner->terminate();
    }
    frost::Result<frost::io::ProcessResult> communicate(std::string_view bytes,
                                                        frost::IClock::Duration timeout) override {
        return m_inner->communicate(bytes, timeout);
    }
    void abandonOutput() noexcept override { m_inner->abandonOutput(); }

private:
    std::unique_ptr<frost::io::IProcess> m_inner;
    std::atomic<bool>& m_release;
    std::atomic<bool> m_terminated{false};
};

class ReleaseGatedLauncher final : public frost::io::IProcessLauncher {
public:
    frost::io::ScriptedProcessLauncher scripted;
    std::atomic<bool> release{false};
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        auto result = scripted.launch(spec);
        if (!result || scripted.launches().size() != 2)
            return result;
        return std::unique_ptr<frost::io::IProcess>(new ReleaseGatedProcess(std::move(*result), release));
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        return scripted.launchAsync(spec, context);
    }
};

TEST(WorkbenchClangd, StreamingNextProjectPreservesExplorerSearchAndOpenDocument) {
    if constexpr (frost::io::kNativePathStyle != frost::io::PathStyle::Windows)
        GTEST_SKIP() << "MSBuild evaluation is Windows-only";
    WorkDir dir("project-stream-search", "tiny_sln");
    const auto first = dir.join("Lib/first_shared.cpp");
    const auto second = dir.join("App/second_shared.cpp");
    dir.write("Lib/first_shared.cpp", "int first_shared;\n");
    dir.write("App/second_shared.cpp", "int second_shared;\n");
    const auto json = [](const std::string& path) {
        return QJsonDocument(
                   QJsonObject{{"Items", QJsonObject{{"ClCompile", QJsonArray{QJsonObject{
                                                                       {"FullPath", QString::fromStdString(path)}}}}}}})
            .toJson(QJsonDocument::Compact)
            .toStdString();
    };
    ReleaseGatedLauncher launcher;
    const auto msbuild = dir.join("held-msbuild.exe");
    launcher.scripted.expect({.expectExecutable = msbuild, .stdoutBytes = json(first)});
    launcher.scripted.expect({.expectExecutable = msbuild, .stdoutBytes = json(second)});
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = false;
    options.workbench.workers = 1;
    options.workbench.msbuildPath = msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(rig.workbench->openPath(first));
    auto* document = rig.view();
    ASSERT_NE(document, nullptr);
    auto* explorer = rig.window->findChild<frost::ui::SolutionExplorer*>();
    ASSERT_NE(explorer, nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            return launcher.scripted.launches().size() == 2 && explorer->solution() &&
                   explorer->solution()->findFileNode(first);
        },
        10s));
    for (const QChar ch : QStringLiteral("shared")) {
        QKeyEvent key(QEvent::KeyPress, ch.toUpper().unicode(), Qt::NoModifier, QString(ch));
        QApplication::sendEvent(explorer->view(), &key);
    }
    ASSERT_TRUE(explorer->isSpeedSearchActive());
    ASSERT_EQ(explorer->currentPath().toStdString(), first);
    launcher.release.store(true);
    ASSERT_TRUE(waitFor([&] { return explorer->solution() && explorer->solution()->findFileNode(second); }, 10s));
    EXPECT_TRUE(explorer->isSpeedSearchActive());
    EXPECT_EQ(explorer->speedSearchText(), QStringLiteral("shared"));
    EXPECT_EQ(explorer->filterModel()->query(), QStringLiteral("shared"));
    EXPECT_EQ(explorer->currentPath().toStdString(), first);
    EXPECT_EQ(explorer->speedSearchMatches().size(), 2u);
    EXPECT_EQ(rig.view(), document);
}

#ifdef _WIN32
class ProjectEvaluationDispatchLauncher final : public frost::io::IProcessLauncher {
public:
    explicit ProjectEvaluationDispatchLauncher(std::string msbuild) : m_msbuild(std::move(msbuild)) {}
    frost::io::ScriptedProcessLauncher scripted;
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        return spec.executable == m_msbuild ? scripted.launch(spec) : frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        return frost::io::realProcessLauncher().launchAsync(spec, context);
    }

private:
    std::string m_msbuild;
};

TEST(WorkbenchClangd, RealClangdUsesFirstProjectWhileSecondIsHeld) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    if (!found)
        GTEST_SKIP() << "clangd is unavailable";
    WorkDir dir("project-stream-real-clangd", "tiny_sln");
    const auto source = dir.join("Lib/generated.cpp");
    dir.write("Lib/generated.cpp", "#ifndef STREAMED_DEFINE\n#error missing streamed define\n#endif\n"
                                   "int streamed() { return plantedUndeclared; }\n");
    const QJsonObject item{{"FullPath", QString::fromStdString(source)},
                           {"PreprocessorDefinitions", "STREAMED_DEFINE=1"}};
    const auto bytes = QJsonDocument(QJsonObject{{"Properties", QJsonObject{{"Platform", "x64"}}},
                                                 {"Items", QJsonObject{{"ClCompile", QJsonArray{item}}}}})
                           .toJson(QJsonDocument::Compact)
                           .toStdString();
    const auto msbuild = dir.join("held-msbuild.exe");
    ProjectEvaluationDispatchLauncher launcher(msbuild);
    launcher.scripted.expect({.expectExecutable = msbuild, .stdoutBytes = bytes});
    launcher.scripted.expect({.expectExecutable = msbuild, .neverExits = true, .outputHeld = true});
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = true;
    options.workbench.workers = 1;
    options.workbench.msbuildPath = msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(rig.workbench->openPath(source));
    auto* explorer = rig.window->findChild<frost::ui::SolutionExplorer*>();
    ASSERT_NE(explorer, nullptr);
    const bool ready = waitFor(
        [&] {
            auto* server = rig.workbench->clangd();
            return launcher.scripted.launches().size() == 2 && server &&
                   server->state() == frost::lang::ClangdState::Running &&
                   server->diagnostics().forFile(source).has_value() && explorer->solution() &&
                   explorer->solution()->findFileNode(source).has_value();
        },
        kPatience);
    if (!ready) {
        auto* server = rig.workbench->clangd();
        ADD_FAILURE() << "launches=" << launcher.scripted.launches().size() << " clangd=" << (server != nullptr)
                      << " state=" << (server ? static_cast<int>(server->state()) : -1)
                      << " document=" << (server && server->documentVersion(source).has_value())
                      << " database=" << (server && server->compileCommands().has_value())
                      << " diagnostics=" << (server && server->diagnostics().forFile(source).has_value())
                      << " tree=" << (explorer->solution() && explorer->solution()->findFileNode(source).has_value());
    }
    ASSERT_TRUE(ready);
    const auto diagnostics = rig.workbench->clangd()->diagnostics().forFile(source);
    ASSERT_TRUE(diagnostics);
    ASSERT_EQ(diagnostics->diagnostics.size(), 1u);
    EXPECT_NE(diagnostics->diagnostics[0].message.find("plantedUndeclared"), std::string::npos);
    EXPECT_FALSE(launcher.scripted.launches()[1].terminated);
    ASSERT_TRUE(rig.workbench->closeProject());
    ASSERT_TRUE(waitFor([&] { return launcher.scripted.launches().back().terminated; }, 10s));
}
#endif

#ifdef _WIN32
// Records a waitable handle to the real child while leaving all process ownership with FrostIDE.
class HeldChildLauncher final : public frost::io::IProcessLauncher {
public:
    explicit HeldChildLauncher(std::string executable) : m_executable(std::move(executable)) {}
    ~HeldChildLauncher() {
        if (auto handle = m_handle.load())
            CloseHandle(handle);
    }
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        auto child = frost::io::realProcessLauncher().launch(spec);
        if (child && spec.executable == m_executable) {
            HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>((*child)->pid()));
            m_handle.store(handle);
        }
        return child;
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        return frost::io::realProcessLauncher().launchAsync(spec, context);
    }
    [[nodiscard]] HANDLE handle() const { return m_handle.load(); }

private:
    std::string m_executable;
    std::atomic<HANDLE> m_handle{nullptr};
};

TEST(WorkbenchClangd, ClosingProjectTerminatesRealHeldEvaluation) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    if (!found)
        GTEST_SKIP() << "clangd is unavailable";
    WorkDir dir("clangd-real-held-evaluation", "tiny_sln");
    const auto msbuild = frost::io::normalizePath(FROST_HELD_MSBUILD);
    ASSERT_TRUE(msbuild);
    HeldChildLauncher launcher(*msbuild);
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = true;
    options.workbench.workers = 1;
    options.workbench.msbuildPath = *msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(waitFor([&] { return launcher.handle() != nullptr; }, 10s));
    EXPECT_EQ(WaitForSingleObject(launcher.handle(), 0), WAIT_TIMEOUT);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(rig.workbench->closeProject());
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
    EXPECT_EQ(WaitForSingleObject(launcher.handle(), 10000), WAIT_OBJECT_0);
}
#endif

/*
 * The language controller over the workbench's editors: Ctrl+B on a call in main.cpp opens the
 * declaration in lib.h (through the shortcut dispatcher and the keymap's navigate.declaration),
 * Ctrl+click does the same, and Alt+F7 (navigate.findUsages) shows the Find Usages tool window.
 */
TEST(WorkbenchClangd, CtrlBOpensTheDeclarationAndFindUsagesShowsItsWindow) {
    frost::test::disableCrashDialogs(); // clangd inherits the error mode: no WER box if it dies.
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << found.error().describe();

    WorkDir dir("clangd-nav");
    dir.write("lib.h", "#pragma once\n"
                       "int answer();\n");
    dir.write("main.cpp", "#include \"lib.h\"\n"
                          "int main() { return answer(); }\n");
    const std::string main = dir.join("main.cpp");
    const std::vector<frost::lang::CompileCommand> commands = {frost::lang::CompileCommand{
        .directory = dir.path(), .file = main, .arguments = {"clang++", "-std=c++20", "-I", dir.path(), "-c", main}}};
    ASSERT_TRUE(
        frost::lang::writeCompileCommands(frost::io::realFileSystem(), dir.join("build"), commands).has_value());

    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_NE(rig.workbench->clangd(), nullptr);
    ASSERT_NE(rig.workbench->language(), nullptr);

    // The caret inside "answer" (line 2, byte column 21 is its first letter).
    const auto atTheCall = [&] {
        EXPECT_TRUE(rig.workbench->openPath(main, {2, 23}).has_value());
        frost::app::DocumentEditorView* view = rig.workbench->currentView();
        EXPECT_NE(view, nullptr);
        return view;
    };
    const auto onLibH = [&] {
        const frost::app::DocumentEditorView* current = rig.workbench->currentView();
        return current != nullptr && current->path() == dir.join("lib.h");
    };
    // clangd answers once it has parsed main.cpp; until then a request may come back empty.
    const auto navigates = [&](const std::function<void(frost::app::DocumentEditorView&)>& go) {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while (std::chrono::steady_clock::now() < deadline) {
            frost::app::DocumentEditorView* view = atTheCall();
            if (view == nullptr) {
                return false;
            }
            go(*view);
            if (waitFor(onLibH, 3s)) {
                return true;
            }
        }
        return false;
    };

    // Ctrl+B, as the user presses it in the editor.
    ASSERT_TRUE(navigates([](frost::app::DocumentEditorView& view) {
        QWidget* editor = view.widget();
        editor->setFocus();
        QApplication::processEvents();
        frost::app::test::sendKey(editor, Qt::Key_B, Qt::ControlModifier);
    })) << "Ctrl+B did not open lib.h";
    EXPECT_EQ(rig.workbench->currentView()->status().line, 2); // int answer();

    // Ctrl+click on the call.
    ASSERT_TRUE(navigates([](frost::app::DocumentEditorView& view) {
        emit view.editor().ctrlClicked(view.editor().editor().state().primary().head);
    })) << "Ctrl+click did not open lib.h";

    // Alt+F7: Find Usages fills its tool window.
    ASSERT_NE(atTheCall(), nullptr);
    EXPECT_EQ(rig.window->invoke("navigate.findUsages"), frost::input::InvokeResult::Ran);
    EXPECT_TRUE(
        waitFor([&] { return rig.window->toolWindows().isVisible(frost::app::kFindUsagesToolWindow); }, kPatience));
}

// What clangd wrote about its "indexing" work-done progress ($/progress notifications on its stdout,
// R0 follow-up 4): the oracle for readiness Indexing. Fed on reactor threads.
class ProgressWire {
public:
    void feed(std::string_view bytes) {
        const std::scoped_lock lock(m_mutex);
        m_buffer.append(bytes);
        // LSP base protocol: "Content-Length: N\r\n...\r\n\r\n" then N bytes of JSON.
        for (;;) {
            const std::size_t headerEnd = m_buffer.find("\r\n\r\n");
            if (headerEnd == std::string::npos)
                return;
            const std::size_t at = m_buffer.find("Content-Length:");
            if (at == std::string::npos || at > headerEnd) {
                m_buffer.erase(0, headerEnd + 4); // Not a header we understand: skip it.
                continue;
            }
            const std::size_t length = std::stoul(m_buffer.substr(at + 15, headerEnd - at - 15));
            if (m_buffer.size() < headerEnd + 4 + length)
                return;
            onMessage(QJsonDocument::fromJson(QByteArray::fromStdString(m_buffer.substr(headerEnd + 4, length))));
            m_buffer.erase(0, headerEnd + 4 + length);
        }
    }
    // Indexing progress begun and not ended yet, as clangd wrote it.
    [[nodiscard]] bool indexing() const {
        const std::scoped_lock lock(m_mutex);
        return m_begins > m_ends;
    }
    [[nodiscard]] int begins() const {
        const std::scoped_lock lock(m_mutex);
        return m_begins;
    }
    [[nodiscard]] int ends() const {
        const std::scoped_lock lock(m_mutex);
        return m_ends;
    }

private:
    void onMessage(const QJsonDocument& message) {
        const QJsonObject object = message.object();
        if (object.value("method").toString() != QStringLiteral("$/progress"))
            return;
        const QJsonObject params = object.value("params").toObject();
        const QString token = params.value("token").toVariant().toString();
        const QJsonObject value = params.value("value").toObject();
        const QString kind = value.value("kind").toString();
        if (kind == QStringLiteral("begin") && value.value("title").toString().contains(QStringLiteral("index"))) {
            m_indexTokens.insert(token);
            ++m_begins;
        } else if (kind == QStringLiteral("end") && m_indexTokens.remove(token)) {
            ++m_ends;
        }
    }
    mutable std::mutex m_mutex;
    std::string m_buffer;
    QSet<QString> m_indexTokens;
    int m_begins = 0;
    int m_ends = 0;
};

// A real process whose stdout also goes to a ProgressWire.
class TeeProcess final : public frost::io::IAsyncProcess {
public:
    TeeProcess(std::unique_ptr<frost::io::IAsyncProcess> inner, std::shared_ptr<ProgressWire> wire)
        : m_inner(std::move(inner)), m_wire(std::move(wire)) {}
    frost::io::ProcessId pid() const noexcept override { return m_inner->pid(); }
    frost::sched::Task<frost::Result<std::size_t>> readStdout(std::span<char> buffer, std::stop_token token) override {
        // Only locals after the await: this object may be gone when a cancelled read resumes.
        const std::shared_ptr<ProgressWire> wire = m_wire;
        auto read = co_await m_inner->readStdout(buffer, std::move(token));
        if (read && *read > 0)
            wire->feed(std::string_view(buffer.data(), *read));
        co_return read;
    }
    frost::sched::Task<frost::Result<std::size_t>> readStderr(std::span<char> buffer, std::stop_token token) override {
        return m_inner->readStderr(buffer, std::move(token));
    }
    frost::sched::Task<frost::Result<void>> writeStdin(std::string bytes, std::stop_token token) override {
        return m_inner->writeStdin(std::move(bytes), std::move(token));
    }
    void closeStdin() noexcept override { m_inner->closeStdin(); }
    frost::sched::Task<frost::Result<frost::io::ExitStatus>> exited(std::stop_token token) override {
        return m_inner->exited(std::move(token));
    }
    frost::Result<void> terminate() override { return m_inner->terminate(); }
    frost::sched::Task<frost::Result<frost::io::ProcessResult>>
    communicate(std::string stdinData, frost::IClock::Duration timeout, std::stop_token token) override {
        return m_inner->communicate(std::move(stdinData), timeout, std::move(token));
    }

private:
    std::unique_ptr<frost::io::IAsyncProcess> m_inner;
    std::shared_ptr<ProgressWire> m_wire;
};

// The real launcher; clangd's stdout is teed to `wire`.
class TeeLauncher final : public frost::io::IProcessLauncher {
public:
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        return frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        auto process = frost::io::realProcessLauncher().launchAsync(spec, context);
        const std::string_view name = frost::io::fileName(spec.executable);
        if (!process || (name != "clangd" && name != "clangd.exe"))
            return process;
        return std::make_unique<TeeProcess>(std::move(*process), wire);
    }
    std::shared_ptr<ProgressWire> wire = std::make_shared<ProgressWire>();
};

/*
 * The oracle for fallback flags: `clangd --check=<file>` (the pinned clangd, with the
 * --compile-commands-dir the workbench's clangd got, if any) names the command it parses the file
 * with: "Compile command from CDB", "... inferred from <file>", or "Generic fallback command".
 * true for the fallback, false for a database command, nullopt (with `why`) when it says neither.
 */
std::optional<bool> clangdCheckUsesFallback(const std::string& clangd, const std::string& file,
                                            const std::optional<frost::lang::CompileCommandsLocation>& location,
                                            std::string& why, bool enableConfig = false) {
    frost::io::ProcessSpec spec;
    spec.executable = clangd;
    // With `enableConfig`, as a trusted workbench clangd runs: .clangd and the user's config apply.
    spec.arguments = {"--check=" + file, "--log=info", enableConfig ? "--enable-config" : "--enable-config=false"};
    if (location)
        spec.arguments.push_back("--compile-commands-dir=" + location->directory);
    spec.workingDirectory = std::string(frost::io::parentPath(file));
    auto process = frost::io::realProcessLauncher().launch(spec);
    if (!process) {
        why = process.error().describe();
        return std::nullopt;
    }
    auto result = (*process)->communicate({}, 120s);
    if (!result) {
        why = result.error().describe();
        return std::nullopt;
    }
    const std::string log = result->stdoutBytes + result->stderrBytes;
    if (log.find("Generic fallback command is") != std::string::npos)
        return true;
    if (log.find("Compile command from CDB is") != std::string::npos ||
        log.find("Compile command inferred from") != std::string::npos)
        return false;
    why = "clangd --check named no compile command:\n" + log;
    return std::nullopt;
}

// Readiness for `path` agrees with `clangd --check` about fallback flags (the database is read on
// a worker, so a NoCompileCommand answer may take a moment; a database command must stay Unknown).
::testing::AssertionResult fallbackAgreesWithClangd(WorkbenchRig& rig, const std::string& clangd,
                                                    const std::string& path, bool enableConfig = false) {
    using Readiness = frost::ui::lang::ServerReadiness;
    std::string why;
    const std::optional<bool> fallback =
        clangdCheckUsesFallback(clangd, path, rig.workbench->clangd()->compileCommands(), why, enableConfig);
    if (!fallback)
        return ::testing::AssertionFailure() << path << ": " << why;
    const auto said = [&] { return rig.workbench->languageReadiness(path) == Readiness::NoCompileCommand; };
    if (*fallback) {
        if (!waitFor(said, kPatience))
            return ::testing::AssertionFailure() << path << ": clangd uses fallback flags, readiness never said so";
    } else if (waitFor(said, 3s)) {
        return ::testing::AssertionFailure() << path << ": readiness claims fallback flags, clangd has a command";
    }
    return ::testing::AssertionSuccess() << path << (*fallback ? ": fallback" : ": database command");
}

/* R0 follow-up 4: the reason the language features give for an empty answer comes from the real
 * clangd. The oracles: clangd's own state (NotRunning before it runs); `clangd --check` for
 * whether a file is parsed with fallback flags (a database with entries gives every file a
 * command, a listed one or one clangd infers, so neither an unlisted source nor a header is
 * claimed; an empty database leaves the fallback); and the "indexing" $/progress notifications
 * clangd wrote on its stdout: readiness is Indexing only after clangd began that progress, and
 * follows its end. */
TEST(WorkbenchClangd, ReadinessFollowsCompileCommandsAndIndexing) {
    frost::test::disableCrashDialogs();
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << found.error().describe();
    using Readiness = frost::ui::lang::ServerReadiness;

    WorkDir dir("clangd-readiness");
    dir.write("lib.h", "#pragma once\nint compute();\n");
    dir.write("listed.cpp", "#include \"lib.h\"\nint main() { return compute(); }\n");
    dir.write("unlisted.cpp", "int other();\nint use() { return other(); }\n");
    std::vector<frost::lang::CompileCommand> commands;
    const auto listed = [&](const std::string& file) {
        commands.push_back(
            frost::lang::CompileCommand{.directory = dir.path(),
                                        .file = file,
                                        .arguments = {"clang++", "-std=c++20", "-I", dir.path(), "-c", file}});
    };
    listed(dir.join("listed.cpp"));
    // Enough code that clangd's background index takes a while to build (so its progress is seen).
    for (int unit = 0; unit < 4; ++unit) {
        std::string text;
        for (int i = 0; i < 20000; ++i)
            text += std::format("int unit{}_f{}(int x) {{ return x + {}; }}\n", unit, i, i);
        const std::string name = std::format("bulk{}.cpp", unit);
        dir.write(name, text);
        listed(dir.join(name));
    }
    ASSERT_TRUE(frost::lang::writeCompileCommands(frost::io::realFileSystem(), dir.join("build"), commands));

    TeeLauncher launcher;
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    EXPECT_EQ(rig.workbench->languageReadiness(dir.join("listed.cpp")), Readiness::NotRunning);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->clangd() && rig.workbench->clangd()->state() == frost::lang::ClangdState::Running;
        },
        kPatience));
    ASSERT_TRUE(rig.workbench->clangd()->compileCommands().has_value());
    EXPECT_EQ(rig.workbench->languageReadiness(dir.join("notes.txt")), Readiness::NotRunning);
    // clangd loads the database, and starts its background index, for the first open document.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("listed.cpp")).has_value());

    // Indexing follows clangd's own begin and end. clangd's message reaches the wire before the
    // workbench handles it, so readiness may lag the wire, never lead it.
    const ProgressWire& wire = *launcher.wire;
    bool ledTheWire = false;
    const auto readiness = [&] {
        const int begins = wire.begins();
        const Readiness now = rig.workbench->languageReadiness(dir.join("listed.cpp"));
        ledTheWire = ledTheWire || (now == Readiness::Indexing && begins == 0);
        return now;
    };
    ASSERT_TRUE(waitFor([&] { return readiness() == Readiness::Indexing; }, kPatience))
        << "readiness never said Indexing (clangd began " << wire.begins() << " indexing progress)";
    EXPECT_GE(wire.begins(), 1) << "Indexing without clangd's progress";
    ASSERT_TRUE(waitFor([&] { return !wire.indexing() && readiness() != Readiness::Indexing; }, 240s))
        << "clangd's background index never ended (begins " << wire.begins() << ", ends " << wire.ends() << ")";
    EXPECT_FALSE(ledTheWire) << "readiness said Indexing before clangd began its progress";
    EXPECT_GE(wire.ends(), 1);
    // Ended on the wire and handled: readiness stays off Indexing unless clangd begins again.
    const int settledBegins = wire.begins();
    EXPECT_FALSE(waitFor(
        [&] { return readiness() == Readiness::Indexing && !wire.indexing() && wire.begins() == settledBegins; }, 2s));

    // A database with entries: clangd has a command for the listed file, an unlisted source and a
    // header alike (`clangd --check`), so nothing is claimed.
    for (const char* file : {"listed.cpp", "unlisted.cpp", "lib.h"})
        EXPECT_TRUE(fallbackAgreesWithClangd(rig, found->path, dir.join(file)));
    EXPECT_EQ(rig.workbench->languageReadiness(dir.join("unlisted.cpp")), Readiness::Unknown);

    // An empty database (a configure that produced no entries): clangd falls back for every file.
    dir.write("build/compile_commands.json", "[]\n");
    for (const char* file : {"listed.cpp", "unlisted.cpp"})
        EXPECT_TRUE(fallbackAgreesWithClangd(rig, found->path, dir.join(file)));

    // The message of an empty answer for a file clangd parses with fallback flags names them.
    std::vector<QString> status;
    QObject::connect(rig.workbench->language(), &frost::ui::lang::LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });
    ASSERT_TRUE(rig.workbench->openPath(dir.join("unlisted.cpp"), {2, 22}).has_value());
    const auto said = [&](const QString& needle) {
        return std::ranges::any_of(status, [&](const QString& message) { return message.contains(needle); });
    };
    const auto deadline = std::chrono::steady_clock::now() + kPatience;
    while (!said(QStringLiteral("no compile command")) && std::chrono::steady_clock::now() < deadline) {
        rig.workbench->language()->gotoImplementation();
        (void)waitFor([&] { return said(QStringLiteral("no compile command")); }, 3s);
    }
    EXPECT_TRUE(said(QStringLiteral("no compile command")))
        << (status.empty() ? std::string("no status message") : status.back().toStdString());
}

/* R0 follow-up 4: a clangd started without --compile-commands-dir (the folder has no database the
 * workbench finds) searches each file's folders upward for compile_commands.json,
 * build/compile_commands.json or compile_flags.txt. Readiness follows that search, checked against
 * `clangd --check` without the flag: a file below a nested database or a flags file has a
 * command; a file with nothing above it (the oracle decides, the temporary folder's ancestors
 * included) falls back. */
TEST(WorkbenchClangd, FallbackFlagsFollowClangdsOwnDatabaseSearch) {
    frost::test::disableCrashDialogs();
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << found.error().describe();

    WorkDir dir("clangd-readiness-search");
    dir.write("top.cpp", "int top() { return 1; }\n");
    dir.write("nested/src/inner.cpp", "int inner() { return 2; }\n");
    dir.write("nested/src/other.cpp", "int other() { return 3; }\n");
    dir.write("flags/sub/flagged.cpp", "int flagged() { return 4; }\n");
    dir.write("flags/compile_flags.txt", "-std=c++20\n");
    const std::string inner = dir.join("nested/src/inner.cpp");
    const std::vector<frost::lang::CompileCommand> commands = {frost::lang::CompileCommand{
        .directory = dir.join("nested"), .file = inner, .arguments = {"clang++", "-std=c++20", "-c", inner}}};
    // nested/build/: clangd's search finds it from nested/src/; the workbench's root search does not.
    ASSERT_TRUE(frost::lang::writeCompileCommands(frost::io::realFileSystem(), dir.join("nested/build"), commands));

    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->clangd() && rig.workbench->clangd()->state() == frost::lang::ClangdState::Running;
        },
        kPatience));
    ASSERT_FALSE(rig.workbench->clangd()->compileCommands().has_value()) << "clangd got a --compile-commands-dir";
    for (const char* file : {"top.cpp", "nested/src/inner.cpp", "nested/src/other.cpp", "flags/sub/flagged.cpp"})
        EXPECT_TRUE(fallbackAgreesWithClangd(rig, found->path, dir.join(file)));
}

/* Truth rule (cross review of R0-close): a trusted clangd runs with its config enabled, and a
 * `CompileFlags: CompilationDatabase:` directive in a .clangd moves its database search to a folder
 * the readiness model never looks in. The oracle is `clangd --check` with config enabled, as the
 * workbench's trusted clangd runs: it finds a database command for the file under the directive,
 * so readiness must not claim "no compile command" there (it claimed it before). Without config
 * the same file falls back, which shows the directive is what decides. A trusted file without a
 * directive above it still gets the fallback claim, when the oracle agrees. The project lives in
 * the system's temporary folder: below the build tree, clangd's own search would find the build's
 * compile_commands.json above every file, and the directive would decide nothing. */
TEST(WorkbenchClangd, ACompilationDatabaseDirectiveInATrustedDotClangdIsNotClaimedAsFallback) {
    frost::test::disableCrashDialogs();
    auto& fs = frost::io::realFileSystem();
    auto found = frost::lang::findClangd(fs, frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << found.error().describe();
    using Readiness = frost::ui::lang::ServerReadiness;

    WorkDir profile("clangd-readiness-config");
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid()) << temp.errorString().toStdString();
    const std::string root = frost::io::normalizePath(QDir::toNativeSeparators(temp.path()).toStdString()).value_or("");
    ASSERT_FALSE(root.empty());
    const auto join = [&](std::string_view relative) {
        return frost::io::normalizePath(frost::io::joinPath(root, relative).value_or("")).value_or("");
    };
    const auto write = [&](std::string_view relative, std::string_view bytes) {
        const std::string file = join(relative);
        ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(file)).has_value()) << file;
        ASSERT_TRUE(fs.writeFileAtomic(file, bytes).has_value()) << file;
    };
    const std::string configured = join("configured/main.cpp");
    const std::string plain = join("plain/lone.cpp");
    write("configured/main.cpp", "int main() { return 0; }\n");
    write("plain/lone.cpp", "int lone() { return 1; }\n");
    // The directive's relative folder is resolved against the .clangd's own folder.
    write("configured/.clangd", "CompileFlags:\n  CompilationDatabase: cdb\n");
    const std::vector<frost::lang::CompileCommand> commands = {frost::lang::CompileCommand{
        .directory = join("configured"), .file = configured, .arguments = {"clang++", "-std=c++20", "-c", configured}}};
    ASSERT_TRUE(frost::lang::writeCompileCommands(fs, join("configured/cdb"), commands));

    // The two oracles for the configured file: config decides between a command and the fallback.
    std::string why;
    const std::optional<bool> withConfig = clangdCheckUsesFallback(found->path, configured, std::nullopt, why, true);
    ASSERT_TRUE(withConfig.has_value()) << why;
    ASSERT_FALSE(*withConfig) << "clangd --check ignored the .clangd CompilationDatabase directive";
    const std::optional<bool> withoutConfig = clangdCheckUsesFallback(found->path, configured, std::nullopt, why);
    ASSERT_TRUE(withoutConfig.has_value()) << why;
    ASSERT_TRUE(*withoutConfig) << "without config clangd still found a database; the directive decides nothing";

    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(profile, std::move(options));
    ASSERT_TRUE(rig.trust->trust(root));
    ASSERT_TRUE(rig.workbench->openPath(root).has_value());
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->clangd() && rig.workbench->clangd()->state() == frost::lang::ClangdState::Running;
        },
        kPatience));
    ASSERT_FALSE(rig.workbench->clangd()->compileCommands().has_value()) << "clangd got a --compile-commands-dir";
    EXPECT_TRUE(fallbackAgreesWithClangd(rig, found->path, configured, true));
    EXPECT_NE(rig.workbench->languageReadiness(configured), Readiness::NoCompileCommand);
    EXPECT_TRUE(fallbackAgreesWithClangd(rig, found->path, plain, true));
}

} // namespace
