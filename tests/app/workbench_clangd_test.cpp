// SPDX-License-Identifier: Apache-2.0
//
// The workbench with the real clangd (label "clangd"; W1, ADR 0044): a folder project with a
// compile_commands.json under build/ and a main.cpp that names an undeclared identifier. Opening
// main.cpp shows clangd's error as a Live row in Problems, a squiggle (range decoration) and a
// gutter marker; typing the declaration clears all three. A second folder checks the language
// controller: Ctrl+B and Ctrl+click open a declaration, Alt+F7 shows Find Usages.

#include "frost/lang/compile_commands.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/find/find_popup.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <QLineEdit>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

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
    EXPECT_TRUE(std::ranges::any_of(view->editor().lineMarkers(),
                                    [](const frost::ui::LineMarker& m) { return m.line == frost::LineIndex{1}; }));

    // The fix, typed into the editor: clangd gets the change and publishes no error.
    view->editor().goToLine(1, 1);
    ASSERT_TRUE(view->editor().typeText("int missing_value = 0;\n"));
    ASSERT_TRUE(
        waitFor([&] { return liveErrors(*rig.workbench).empty() && view->editor().diagnostics().empty(); }, kPatience));
    EXPECT_TRUE(view->editor().lineMarkers().empty());
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
