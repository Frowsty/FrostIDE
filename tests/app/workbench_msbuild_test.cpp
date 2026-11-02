// SPDX-License-Identifier: Apache-2.0
//
// The workbench with the real MSBuild (Windows, label "msbuild"; W1, ADR 0044): the tiny fixture
// solution with a planted warning (C4996, line 6) and error (C2065, line 7) in App/main.cpp, built
// from the Build menu. Both reach the Problems tool window (shown because of the error) and the
// Build output; activating the C2065 row opens main.cpp at line 7 with an error marker in the
// gutter. A machine without Visual Studio skips the test.

#include "frost/build/msbuild_locator.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_menus.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "workbench_rig.hpp"
#include <QAction>
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

} // namespace
