// SPDX-License-Identifier: Apache-2.0
//
// The workbench with the real clangd (label "clangd"; W1, ADR 0044): a folder project with a
// compile_commands.json under build/ and a main.cpp that names an undeclared identifier. Opening
// main.cpp shows clangd's error as a Live row in Problems, a squiggle (range decoration) and a
// gutter marker; typing the declaration clears all three. A second folder checks the language
// controller: Ctrl+B and Ctrl+click open a declaration, Alt+F7 shows Find Usages.

#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/find/find_popup.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTreeView>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
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

} // namespace
