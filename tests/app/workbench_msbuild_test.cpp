// SPDX-License-Identifier: Apache-2.0
//
// The workbench with the real MSBuild (Windows, label "msbuild"; W1, ADR 0044): the tiny fixture
// solution with a planted warning (C4996, line 6) and error (C2065, line 7) in App/main.cpp, built
// from the Build menu. Both reach the Problems tool window (shown because of the error) and the
// Build output; activating the C2065 row opens main.cpp at line 7 with an error marker in the
// gutter. The tiny fixture also runs from the Run action (Shift+F10): Run saves all and builds
// first, and the program's output and its echoed input appear in the Run tool window's console.
// SLN-9: Compile (Ctrl+F7) of the active editor's file, whose Problems rows must equal what
// MSBuild's own file loggers record for the same compile; SLN-11: debugger arguments and working
// directory saved on the Project Properties Debugging page reach the program F5 debugs. A machine
// without Visual Studio skips the tests.

#include "frost/build/msbuild_locator.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_menus.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "../../src/app/src/project_properties_dialog.hpp"
#include "build/msbuild_diagnostic_oracle.hpp"
#include "workbench_rig.hpp"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>
#include <QTreeView>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

// This machine's MSBuild.exe, or nullopt (no Visual Studio; the test skips).
std::optional<std::string> machineMsBuild() {
    auto found = frost::build::findMsBuild(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    if (!found) {
        if (found.error().code() != frost::ErrorCode::NotFound) {
            ADD_FAILURE() << found.error().describe();
        }
        return std::nullopt;
    }
    return found->msbuildPath;
}

TEST(WorkbenchMsBuild, BuildErrorsReachProblemsAndJumpToTheLine) {
    const std::optional<std::string> msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir dir("msbuild", "tiny_sln");
    dir.write("App/main.cpp", "#include \"lib.h\"\n"
                              "\n"
                              "[[deprecated(\"use tinyAnswer\")]] int oldAnswer();\n"
                              "\n"
                              "int main() {\n"
                              "    int value = oldAnswer();\n"
                              "    return tinyAnswer() + value + undeclared_thing;\n"
                              "}\n");
    RigOptions options;
    options.workbench.msbuildPath = *msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value()); // No trust dialog in a test.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());

    rig.window->menus().refresh(); // As opening the Build menu does.
    QAction* rebuild = rig.window->menus().action(frost::ui::build_actions::kRebuildSolution);
    ASSERT_NE(rebuild, nullptr);
    ASSERT_TRUE(rebuild->isEnabled());
    rebuild->trigger();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->build().isBuilding(); }));
    EXPECT_TRUE(rig.window->toolWindows().isVisible(frost::app::kBuildToolWindow));
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }, std::chrono::seconds(180)));
    ASSERT_TRUE(rig.workbench->build().lastOutcome().has_value());
    EXPECT_EQ(rig.workbench->build().lastOutcome()->counts.errors, 1U);
    EXPECT_EQ(rig.workbench->build().lastOutcome()->counts.warnings, 1U);

    // Problems shows both and is shown (the build had an error); the Build output has them too.
    EXPECT_TRUE(rig.window->toolWindows().isVisible(frost::ui::tool_windows::kProblems));
    frost::ui::ProblemsView* problems = rig.workbench->problems();
    ASSERT_NE(problems, nullptr);
    const auto rowOf = [&](std::string_view code) {
        const auto& rows = problems->model()->problems();
        const auto it = std::ranges::find_if(
            rows, [code](const frost::ui::Problem& p) { return p.diagnostic.diagnostic.code == code; });
        return it == rows.end() ? -1 : static_cast<int>(it - rows.begin());
    };
    EXPECT_GE(rowOf("C4996"), 0);
    ASSERT_GE(rowOf("C2065"), 0);
    const auto& output = rig.workbench->buildOutput()->log().diagnostics();
    EXPECT_TRUE(std::ranges::any_of(
        output, [](const frost::build::BuildDiagnostic& d) { return d.diagnostic.code == "C2065"; }));
    EXPECT_TRUE(std::ranges::any_of(
        output, [](const frost::build::BuildDiagnostic& d) { return d.diagnostic.code == "C4996"; }));

    // Activating (double-clicking) the C2065 row opens main.cpp at line 7 with its gutter marker.
    QAbstractItemModel* shown = problems->view()->model();
    QModelIndex target;
    for (int row = 0; row < shown->rowCount() && !target.isValid(); ++row) {
        const QModelIndex code = shown->index(row, frost::ui::ProblemsModel::CodeColumn);
        if (code.data().toString() == QStringLiteral("C2065")) {
            target = code;
        }
    }
    ASSERT_TRUE(target.isValid());
    ASSERT_TRUE(problems->activate(target));
    ASSERT_TRUE(waitFor([&] { return rig.window->tabs().count() == 1; }));
    frost::app::DocumentEditorView* view = rig.view();
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->path(), dir.join("App/main.cpp"));
    EXPECT_EQ(view->status().line, 7);
    const std::vector<frost::ui::LineMarker>& markers = view->editor().lineMarkers();
    EXPECT_TRUE(std::ranges::any_of(markers, [](const frost::ui::LineMarker& m) {
        return m.line == frost::LineIndex{6} && m.severity == frost::ui::MarkerSeverity::Error &&
               m.message.find("C2065") != std::string::npos;
    })) << markers.size();
    EXPECT_TRUE(std::ranges::any_of(markers, [](const frost::ui::LineMarker& m) {
        return m.line == frost::LineIndex{5} && m.severity == frost::ui::MarkerSeverity::Warning;
    }));
}

TEST(WorkbenchMsBuild, DebugBuildsStopsInspectsStepsAndStops) {
    const auto msbuild = machineMsBuild();
    if (!msbuild)
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    auto& fs = frost::io::realFileSystem();
    const auto lldb = frost::debug::findLldbDap(fs, frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(lldb.has_value()) << "the pinned lldb-dap is required (FND-6): " << lldb.error().describe();
    WorkDir dir("msbuild-debug", "tiny_sln");
    dir.write("App/main.cpp", "int main() {\n  int value = 41;\n  value += 1;\n  return value == 42 ? 0 : 1;\n}\n");
    RigOptions options;
    options.workbench.msbuildPath = *msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    auto* debug = rig.workbench->debugController();
    ASSERT_NE(debug, nullptr);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/main.cpp")).has_value());
    ASSERT_TRUE(rig.view()->editor().typeText("// saved before debug\n"));
    debug->toggleBreakpoint(dir.join("App/main.cpp"), 3);
    ASSERT_EQ(rig.window->invoke("run.debug"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(
        waitFor([&] { return debug->paused() && debug->selectedFrame().has_value(); }, std::chrono::seconds(240)))
        << rig.workbench->runConsole()->text().toStdString();
    ASSERT_EQ(debug->selectedFrame()->line, 4);
    EXPECT_TRUE(dir.read("App/main.cpp").starts_with("// saved before debug\n"));
    auto value = std::make_shared<std::optional<frost::Result<frost::debug::dap::Variable>>>();
    debug->evaluate(QStringLiteral("value"), QStringLiteral("watch"),
                    [value](auto result) { *value = std::move(result); });
    ASSERT_TRUE(waitFor([&] { return value->has_value(); }, std::chrono::seconds(60)));
    ASSERT_TRUE(value->value().has_value());
    EXPECT_EQ(value->value()->value, "41");
    ASSERT_TRUE(debug->stepOver());
    ASSERT_TRUE(waitFor([&] { return debug->paused() && debug->selectedFrame() && debug->selectedFrame()->line == 5; },
                        std::chrono::seconds(60)));
    EXPECT_FALSE(rig.workbench->closeProject());
    emit rig.workbench->runConsole()->rerunRequested();
    QCoreApplication::processEvents();
    EXPECT_TRUE(debug->paused());
    EXPECT_TRUE(rig.workbench->runService()->activeRuns().empty());
    auto* debugWindow = rig.window->findChild<frost::ui::DebugToolWindow*>();
    ASSERT_NE(debugWindow, nullptr);
    int stops = 0;
    QObject::connect(debug, &frost::ui::DebugController::stopped, [&] { ++stops; });
    emit debugWindow->console()->rerunRequested();
    ASSERT_TRUE(waitFor([&] { return stops >= 1 && debug->paused() && debug->selectedFrame().has_value(); },
                        std::chrono::seconds(240)));
    EXPECT_EQ(debug->selectedFrame()->line, 4);
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, std::chrono::seconds(60)));
    EXPECT_TRUE(rig.workbench->closeProject());
}

TEST(WorkbenchMsBuild, StopDropsTheQueuedDebugHandoff) {
    const auto msbuild = machineMsBuild();
    if (!msbuild)
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    WorkDir dir("msbuild-debug-pending-stop", "tiny_sln");
    dir.write("App/main.cpp", "int main() { return 0; }\n");
    RigOptions options;
    int adaptersStarted = 0;
    options.workbench.debugAdapterFactory =
        [&](const frost::debug::DapHandlers&) -> frost::Result<std::unique_ptr<frost::debug::IDebugAdapterClient>> {
        ++adaptersStarted;
        return frost::fail(frost::ErrorCode::IoError, "unexpected adapter launch");
    };
    options.workbench.msbuildPath = *msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    rig.workbench->buildService().setSaveAllHook([](const frost::build::SaveAllCompletion& done) { done({}); });
    auto* debug = rig.workbench->debugController();
    ASSERT_TRUE(debug->debug().has_value());
    ASSERT_TRUE(rig.workbench->runService()->waitForIdle(std::chrono::seconds(240)));
    ASSERT_TRUE(rig.workbench->runService()->lastOutcome());
    ASSERT_TRUE(rig.workbench->runService()->lastOutcome()->debugged);
    auto* debugWindow = rig.window->findChild<frost::ui::DebugToolWindow*>();
    ASSERT_NE(debugWindow, nullptr);
    debug->stop();
    QCoreApplication::processEvents();
    EXPECT_EQ(adaptersStarted, 0);
    EXPECT_FALSE(debug->debugging());
    EXPECT_EQ(debug->session(), nullptr);
    EXPECT_FALSE(debugWindow->console()->text().contains(QStringLiteral("with lldb-dap")));
}

TEST(WorkbenchMsBuild, AbortStopsAtTheUserCallSite) {
    const auto msbuild = machineMsBuild();
    if (!msbuild)
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    auto& fs = frost::io::realFileSystem();
    const auto lldb = frost::debug::findLldbDap(fs, frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(lldb.has_value()) << "the pinned lldb-dap is required (FND-6): " << lldb.error().describe();
    WorkDir dir("msbuild-debug-abort", "tiny_sln");
    dir.write("App/main.cpp", "#include <cstdlib>\n#include <windows.h>\nint main() {\n"
                              "  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);\n"
                              "  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);\n"
                              "  std::abort();\n}\n");
    RigOptions options;
    options.workbench.msbuildPath = *msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    auto* debug = rig.workbench->debugController();
    ASSERT_EQ(rig.window->invoke("run.debug"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(
        waitFor([&] { return debug->paused() && debug->selectedFrame().has_value(); }, std::chrono::seconds(240)))
        << rig.workbench->runConsole()->text().toStdString();
    ASSERT_TRUE(debug->stopInfo().has_value());
    EXPECT_TRUE(debug->stopInfo()->isCrash());
    EXPECT_EQ(debug->stopInfo()->reason, "abort");
    EXPECT_EQ(debug->selectedFrame()->path, dir.join("App/main.cpp"));
    EXPECT_EQ(debug->selectedFrame()->line, 6);
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, std::chrono::seconds(60)));
}

TEST(WorkbenchMsBuild, RunSavesBuildsAndShowsTheProgramInTheRunToolWindow) {
    const std::optional<std::string> msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir dir("run", "tiny_sln");
    RigOptions options;
    options.workbench.msbuildPath = *msbuild;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value()); // No trust dialog in a test.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    // The startup project is App (Lib is a library), shown in the toolbar's combo.
    EXPECT_EQ(rig.window->mainToolbar().runConfigurations()->currentText(), QStringLiteral("App"));

    // A dirty buffer: Run saves it before the build.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/main.cpp")).has_value());
    ASSERT_TRUE(rig.view()->editor().typeText("// saved by Run\n"));
    ASSERT_TRUE(rig.window->tabs().isDirty(0));

    ASSERT_EQ(rig.window->invoke("run.run"), frost::input::InvokeResult::Ran);
    frost::ui::RunConsole* console = rig.workbench->runConsole();
    ASSERT_NE(console, nullptr);
    ASSERT_TRUE(
        waitFor([&] { return console->text().contains(QStringLiteral("tiny answer 42")); }, std::chrono::seconds(240)))
        << console->text().toStdString();
    EXPECT_TRUE(dir.read("App/main.cpp").starts_with("// saved by Run\n"));
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
    EXPECT_TRUE(rig.window->toolWindows().isVisible(frost::ui::tool_windows::kRun));
    // RUN-3: the run has its tab in the Run tool window.
    EXPECT_EQ(rig.window->toolWindows().content(frost::ui::tool_windows::kRun),
              static_cast<QWidget*>(rig.workbench->runToolWindow()));
    EXPECT_TRUE(rig.workbench->runToolWindow()->isAncestorOf(console));

    // The program reads a line (--echo): typed into the console's input.
    ASSERT_TRUE(waitFor([&] { return console->input()->isEnabled(); }));
    WorkDir other("running-other");
    EXPECT_FALSE(rig.workbench->openPath(other.path()).has_value());
    EXPECT_FALSE(rig.workbench->closeProject());
    ASSERT_NE(rig.workbench->solution(), nullptr);
    EXPECT_EQ(rig.workbench->solution()->path(), dir.join("Tiny.sln"));
    EXPECT_EQ(rig.window->tabs().count(), 1);
    console->input()->setText(QStringLiteral("hi there"));
    emit console->input()->returnPressed();
    ASSERT_TRUE(
        waitFor([&] { return console->text().contains(QStringLiteral("echo: hi there")); }, std::chrono::seconds(30)))
        << console->text().toStdString();
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runController()->running(); }, std::chrono::seconds(30)));
    ASSERT_TRUE(rig.workbench->runService()->lastOutcome().has_value());
    EXPECT_EQ(rig.workbench->runService()->lastOutcome()->exitCode, 3);
}

// The gutter marker on main() runs the project whose .vcxproj builds the file (App), labelled with
// its name; the program's real output, input echo and exit code come back through the Run window.
TEST(WorkbenchMsBuild, RunMarkerRunsTheProjectThatBuildsTheFile) {
    const std::optional<std::string> msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir dir("run-marker", "tiny_sln");
    RigOptions options;
    options.workbench.msbuildPath = *msbuild;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/main.cpp")).has_value());
    ASSERT_NE(rig.view(), nullptr);
    const auto& markers = rig.view()->editor().lineMarkers();
    const auto marker =
        std::ranges::find_if(markers, [](const auto& m) { return m.severity == frost::ui::MarkerSeverity::RunTest; });
    ASSERT_NE(marker, markers.end());
    EXPECT_EQ(marker->message, "Run 'App'");

    emit rig.view()->editor().gutterRunMarkerClicked(static_cast<int>(marker->line.value()), QPoint());
    frost::ui::RunConsole* console = rig.workbench->runConsole();
    ASSERT_NE(console, nullptr);
    ASSERT_TRUE(
        waitFor([&] { return console->text().contains(QStringLiteral("tiny answer 42")); }, std::chrono::seconds(240)))
        << console->text().toStdString();
    ASSERT_TRUE(waitFor([&] { return console->input()->isEnabled(); }, std::chrono::seconds(30)));
    console->input()->setText(QStringLiteral("marker"));
    emit console->input()->returnPressed();
    ASSERT_TRUE(
        waitFor([&] { return console->text().contains(QStringLiteral("echo: marker")); }, std::chrono::seconds(30)))
        << console->text().toStdString();
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runController()->running(); }, std::chrono::seconds(30)));
    ASSERT_TRUE(rig.workbench->runService()->lastOutcome().has_value());
    EXPECT_EQ(rig.workbench->runService()->lastOutcome()->exitCode, 3);
}

// SLN-9: Ctrl+F7 compiles the active editor's file with its project (App) and nothing else; the
// Problems rows equal the diagnostics MSBuild's own errors-only and warnings-only file loggers
// record for the same single-file compile.
TEST(WorkbenchMsBuild, CompileTheCurrentFileShowsWhatMsBuildLogged) {
    const std::optional<std::string> msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir dir("msbuild-compile", "tiny_sln");
    dir.write("App/main.cpp", "#include \"lib.h\"\n"
                              "\n"
                              "[[deprecated(\"use tinyAnswer\")]] int oldAnswer();\n"
                              "\n"
                              "int main() {\n"
                              "    int value = oldAnswer();\n"
                              "    return tinyAnswer() + value + undeclared_thing;\n"
                              "}\n");
    RigOptions options;
    options.workbench.msbuildPath = *msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value()); // No trust dialog in a test.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());

    // Ctrl+F7 is Compile's shortcut in the workbench's keymap.
    const auto compileId = rig.workbench->registry().find(frost::ui::build_actions::kCompileFile);
    ASSERT_TRUE(compileId.has_value());
    const auto shortcut = rig.workbench->keymap().primaryShortcut(*compileId);
    ASSERT_TRUE(shortcut.has_value());
    EXPECT_EQ(frost::input::formatSequence(*shortcut), "Ctrl+F7");

    // Without an editor, or on a file no project compiles, there is nothing to compile.
    rig.window->menus().refresh();
    QAction* compile = rig.window->menus().action(frost::ui::build_actions::kCompileFile);
    ASSERT_NE(compile, nullptr);
    EXPECT_FALSE(compile->isEnabled());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Lib/lib.h")).has_value());
    rig.window->menus().refresh();
    EXPECT_FALSE(compile->isEnabled());

    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/main.cpp")).has_value());
    rig.window->menus().refresh(); // As opening the Build menu does.
    ASSERT_TRUE(compile->isEnabled());
    ASSERT_EQ(rig.window->invoke(frost::ui::build_actions::kCompileFile), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->build().isBuilding(); }));
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }, std::chrono::seconds(180)));
    const auto outcome = rig.workbench->build().lastOutcome();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->target.action, frost::build::BuildAction::Compile);
    EXPECT_FALSE(outcome->error.has_value()) << outcome->error->describe();
    EXPECT_FALSE(outcome->succeeded());

    frost::ui::ProblemsView* problems = rig.workbench->problems();
    ASSERT_NE(problems, nullptr);
    EXPECT_TRUE(rig.window->toolWindows().isVisible(frost::ui::tool_windows::kProblems));
    std::vector<frost::build::BuildDiagnostic> rows;
    for (const frost::ui::Problem& problem : problems->model()->problems()) {
        rows.push_back(problem.diagnostic);
    }

    // The oracle: the same compile, run by MSBuild itself (main.cpp has an error, so no up-to-date
    // object makes MSBuild skip it).
    const std::string solutionDir = dir.path() + "\\";
    const auto oracle = frost::test::runMsBuildOracle(
        *msbuild,
        {dir.join("App/App.vcxproj"), "-t:ClCompile", "-p:SelectedFiles=main.cpp", "-p:Configuration=Debug",
         "-p:Platform=x64", "-p:SolutionDir=" + solutionDir, "-p:SolutionPath=" + dir.join("Tiny.sln"),
         "-p:SolutionName=Tiny", "-p:SolutionFileName=Tiny.sln", "-p:SolutionExt=.sln"},
        dir.join("App"), dir.path());
    ASSERT_TRUE(oracle.has_value());
    EXPECT_NE(oracle->exitCode, 0);
    EXPECT_FALSE(oracle->diagnostics.empty());
    const auto shown = frost::test::errorListRows(rows);
    EXPECT_EQ(shown, oracle->diagnostics) << "Problems:" << frost::test::describe(shown)
                                          << "\nMSBuild logged:" << frost::test::describe(oracle->diagnostics);
    // Compile builds no referenced project: Lib was never built.
    EXPECT_FALSE(frost::io::realFileSystem().exists(dir.join("x64/Debug/Lib.lib")).value_or(true));
}

// SLN-11: the startup project's debugger arguments and working directory, saved on the Project
// Properties Debugging page (to App.vcxproj.user), are what the program F5 debugs gets: argv[1] and
// the current directory, read through lldb-dap at a breakpoint.
TEST(WorkbenchMsBuild, DebugUsesTheDebuggingPagesArgumentsAndWorkingDirectory) {
    const auto msbuild = machineMsBuild();
    if (!msbuild)
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    auto& fs = frost::io::realFileSystem();
    const auto lldb = frost::debug::findLldbDap(fs, frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(lldb.has_value()) << "the pinned lldb-dap is required (FND-6): " << lldb.error().describe();
    WorkDir dir("msbuild-debug-user", "tiny_sln");
    dir.write("App/main.cpp", "#include <direct.h>\n"
                              "int main(int argc, char** argv) {\n"
                              "  char cwd[1024] = {};\n"
                              "  const char* first = argc > 1 ? argv[1] : \"\";\n"
                              "  _getcwd(cwd, sizeof cwd);\n"
                              "  return first[0] == '-' && cwd[0] != 0 ? 0 : 1;\n"
                              "}\n");
    RigOptions options;
    options.workbench.msbuildPath = *msbuild;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    ASSERT_NE(rig.workbench->runService(), nullptr);
    ASSERT_TRUE(rig.workbench->runService()->startupProject().has_value());
    EXPECT_EQ(rig.workbench->solution()->project(*rig.workbench->runService()->startupProject()).name, "App");

    // The Debugging page: new arguments and a working directory the fixture's user file does not have.
    bool edited = false;
    QTimer::singleShot(0, rig.window.get(), [&] {
        auto* dlg = rig.window->findChild<QDialog*>(QStringLiteral("projectPropertiesDialog"));
        ASSERT_NE(dlg, nullptr);
        const auto closeOnExit = qScopeGuard([dlg] {
            if (dlg->isVisible())
                dlg->reject();
        });
        auto* args = dlg->findChild<QLineEdit*>(QStringLiteral("prop_Prop.LocalDebuggerCommandArguments"));
        auto* cwd = dlg->findChild<QLineEdit*>(QStringLiteral("prop_Prop.LocalDebuggerWorkingDirectory"));
        ASSERT_NE(args, nullptr);
        ASSERT_NE(cwd, nullptr);
        args->setText(QStringLiteral("--from-the-debugging-page second"));
        cwd->setText(QStringLiteral("$(SolutionDir)x64"));
        auto* buttons = dlg->findChild<QDialogButtonBox*>();
        QPushButton* ok = buttons != nullptr ? buttons->button(QDialogButtonBox::Ok) : nullptr;
        ASSERT_NE(ok, nullptr);
        ok->click();
        edited = true;
    });
    frost::app::showProjectPropertiesDialog(rig.window.get(), fs, dir.join("App/App.vcxproj"), "Debug|x64", {});
    ASSERT_TRUE(edited);
    ASSERT_TRUE(dir.read("App/App.vcxproj.user").contains("--from-the-debugging-page second"));

    auto* debug = rig.workbench->debugController();
    ASSERT_NE(debug, nullptr);
    debug->toggleBreakpoint(dir.join("App/main.cpp"), 5); // 0-based: the return statement.
    ASSERT_EQ(rig.window->invoke("run.debug"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(
        waitFor([&] { return debug->paused() && debug->selectedFrame().has_value(); }, std::chrono::seconds(240)))
        << rig.workbench->runConsole()->text().toStdString();
    ASSERT_EQ(debug->selectedFrame()->line, 6);
    const auto evaluate = [&](const QString& expression) {
        auto value = std::make_shared<std::optional<frost::Result<frost::debug::dap::Variable>>>();
        debug->evaluate(expression, QStringLiteral("watch"), [value](auto result) { *value = std::move(result); });
        EXPECT_TRUE(waitFor([&] { return value->has_value(); }, std::chrono::seconds(60))) << expression.toStdString();
        return value->has_value() && value->value().has_value() ? value->value()->value : std::string();
    };
    EXPECT_EQ(evaluate(QStringLiteral("argc")), "3");
    EXPECT_TRUE(evaluate(QStringLiteral("argv[1]")).contains("--from-the-debugging-page"));
    EXPECT_TRUE(evaluate(QStringLiteral("argv[2]")).contains("second"));
    // $(SolutionDir)x64 (the build's output directory), not the fixture's Lib nor MSBuild's default App.
    const std::string cwd = evaluate(QStringLiteral("cwd"));
    EXPECT_TRUE(cwd.contains("\\\\x64\"") || cwd.contains("\\x64\"")) << cwd;
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, std::chrono::seconds(60)));
    EXPECT_TRUE(rig.workbench->closeProject());
}

} // namespace
