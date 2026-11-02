// SPDX-License-Identifier: Apache-2.0
//
// The workbench with the real MSBuild (Windows, label "msbuild"; W1, ADR 0044): the tiny fixture
// solution with a planted warning (C4996, line 6) and error (C2065, line 7) in App/main.cpp, built
// from the Build menu. Both reach the Problems tool window (shown because of the error) and the
// Build output; activating the C2065 row opens main.cpp at line 7 with an error marker in the
// gutter. The tiny fixture also runs from the Run action (Shift+F10): Run saves all and builds
// first, and the program's output and its echoed input appear in the Run tool window's console. A
// machine without Visual Studio skips the tests.

#include "frost/build/msbuild_locator.hpp"
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

#include "workbench_rig.hpp"
#include <QAction>
#include <QComboBox>
#include <QLineEdit>
#include <QTreeView>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

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
    EXPECT_EQ(rig.window->toolWindows().content(frost::ui::tool_windows::kRun), static_cast<QWidget*>(console));

    // The program reads a line (--echo): typed into the console's input.
    ASSERT_TRUE(waitFor([&] { return console->input()->isEnabled(); }));
    console->input()->setText(QStringLiteral("hi there"));
    emit console->input()->returnPressed();
    ASSERT_TRUE(
        waitFor([&] { return console->text().contains(QStringLiteral("echo: hi there")); }, std::chrono::seconds(30)))
        << console->text().toStdString();
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runController()->running(); }, std::chrono::seconds(30)));
    ASSERT_TRUE(rig.workbench->runService()->lastOutcome().has_value());
    EXPECT_EQ(rig.workbench->runService()->lastOutcome()->exitCode, 3);
}

} // namespace
