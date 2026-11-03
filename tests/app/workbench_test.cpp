// SPDX-License-Identifier: Apache-2.0
//
// Offscreen functional tests of the workbench (frost/app/workbench.hpp, W1, ADR 0044), the one
// composition root over the navigation workbench (ADR 0042): opening a solution from the command
// line, the one open-file path (Go to File and the Solution Explorer reach the same tab), typing,
// Ctrl+S (Save All), save on close, Close Project keeping a tab whose save was cancelled, Save All
// before a build (a fake MSBuild), the Welcome screen and recent projects, the session of the last
// project, a folder project's File System view, external changes (Compare opens the merge view),
// autosave on frame deactivation, the one hot-exit restore prompt, and the Run tool window. No real
// tool runs here (the msbuild and clangd suites do that).

#include "frost/app/welcome_screen.hpp"
#include "frost/build/build_session.hpp"
#include "frost/core/mutex.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/navigation_bar.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_menus.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"
#include "frost/ui/workbench/merge_view.hpp"

#include "workbench_rig.hpp"
#include <QAction>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace {

using frost::app::DocumentEditorView;
using frost::app::IWorkbenchPrompts;
using frost::app::test::RigOptions;
using frost::app::test::sendKey;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
namespace tool_windows = frost::ui::tool_windows;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;

TEST(Workbench, NavigationBarUsesSolutionAndFolderModels) {
    WorkDir dir("navigation-bar", "tiny_sln");
    dir.write("folder/CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Example C)\n");
    dir.write("folder/first.cpp", "int first;\n");
    dir.write("folder/second.cpp", "int second;\n");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/main.cpp")));
    auto& bar = rig.window->navigationBar();
    ASSERT_TRUE(waitFor([&] {
        for (auto* button : bar.findChildren<QToolButton*>())
            if (button->text() == QStringLiteral("App"))
                return true;
        return false;
    }));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("folder")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("folder/first.cpp")));
    QToolButton* rootButton = nullptr;
    ASSERT_TRUE(waitFor([&] {
        for (auto* button : bar.findChildren<QToolButton*>()) {
            if (button->text() == QStringLiteral("folder") && button->menu()) {
                rootButton = button;
                return true;
            }
        }
        return false;
    }));
    QMetaObject::invokeMethod(rootButton->menu(), "aboutToShow", Qt::DirectConnection);
    QAction* second = nullptr;
    for (QAction* action : rootButton->menu()->actions())
        if (action->text() == QStringLiteral("second.cpp"))
            second = action;
    ASSERT_NE(second, nullptr);
    second->trigger();
    ASSERT_TRUE(waitFor([&] { return rig.window->tabs().indexOf(dir.join("folder/second.cpp")) >= 0; }));
    auto* editor = rig.window->tabs().currentEditor();
    ASSERT_NE(editor, nullptr);
    QWidget* focus = QApplication::focusWidget();
    EXPECT_TRUE(focus == editor->widget() || editor->widget()->isAncestorOf(focus));
    ASSERT_TRUE(rig.workbench->closeProject());
    EXPECT_TRUE(bar.isHidden());
}

TEST(Workbench, OpenDialogRecognizesCMakeProjectAndStillEditsItsLists) {
    WorkDir dir("cmake-open");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Example C)\n");
    dir.write("sub/CMakeLists.txt", "# nested\n");
    WorkbenchRig rig(dir);
    rig.prompts.openPath = dir.join("CMakeLists.txt");
    rig.window->menus().action("file.openFile")->trigger();
    ASSERT_TRUE(rig.workbench->hasProject());
    EXPECT_FALSE(rig.workbench->solution());
    EXPECT_EQ(rig.window->tabs().count(), 0);

    rig.window->menus().action("file.openFile")->trigger();
    EXPECT_EQ(rig.window->tabs().count(), 1);
    rig.prompts.openPath = dir.join("sub/CMakeLists.txt");
    rig.window->menus().action("file.openFile")->trigger();
    EXPECT_TRUE(rig.workbench->hasProject());
    EXPECT_EQ(rig.window->tabs().count(), 2);
}

TEST(Workbench, FindUsagesToolWindowHasDistinctLabel) {
    WorkDir dir("find-usages-label");
    WorkbenchRig rig(dir);
    const auto* button = rig.window->toolWindows().stripeButton(frost::app::kFindUsagesToolWindow);
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->text(), QStringLiteral("Find Usages"));
}

TEST(Workbench, StructureToolWindowOpensAndHidesThroughAction) {
    WorkDir dir("structure-window");
    WorkbenchRig rig(dir);
    rig.show();
    EXPECT_FALSE(rig.window->toolWindows().isVisible("structure"));
    ASSERT_EQ(rig.window->invoke("window.structure"), frost::input::InvokeResult::Ran);
    EXPECT_TRUE(rig.window->toolWindows().isVisible("structure"));
    ASSERT_NE(rig.window->toolWindows().content("structure"), nullptr);
    ASSERT_EQ(rig.window->invoke("window.structure"), frost::input::InvokeResult::Ran);
    EXPECT_FALSE(rig.window->toolWindows().isVisible("structure"));
}

TEST(Workbench, ReloadImmediatelyInvalidatesStructureRows) {
    WorkDir dir("structure-reload");
    dir.write("a.cpp", "int before;\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.cpp")));
    rig.window->toolWindows().show("structure");
    auto* tree = rig.window->findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    ASSERT_NE(tree, nullptr);
    new QTreeWidgetItem(tree, QStringList{QStringLiteral("stale")});
    dir.write("a.cpp", "int after;\n");
    rig.workbench->checkExternalChanges();
    EXPECT_EQ(tree->topLevelItemCount(), 0);
}

TEST(Workbench, SwitchingFromCMakeRestoresSolutionToolbar) {
    WorkDir dir("cmake-to-solution", "tiny_sln");
    dir.write("cmake/CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Example C)\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("cmake")));
    rig.show();
    rig.window->resize(1100, 700);
    QApplication::processEvents();
    ASSERT_NE(rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarProfiles")), nullptr);
    ASSERT_NE(rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarTargets")), nullptr);
    EXPECT_TRUE(rig.workbench->configurationCombo()->isHidden());
    EXPECT_TRUE(rig.window->mainToolbar().runConfigurations()->isHidden());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    rig.window->resize(1200, 750);
    QApplication::processEvents();
    EXPECT_EQ(rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarProfiles")), nullptr);
    EXPECT_EQ(rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarTargets")), nullptr);
    EXPECT_FALSE(rig.workbench->configurationCombo()->isHidden());
    EXPECT_FALSE(rig.window->mainToolbar().runConfigurations()->isHidden());
    EXPECT_EQ(rig.workbench->configurationCombo()->count(), 2);
    EXPECT_GT(rig.window->mainToolbar().runConfigurations()->count(), 0);
}

TEST(Workbench, ExplicitCMakeProjectWinsOverSiblingSolution) {
    WorkDir dir("explicit-cmake", "tiny_sln");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Example C)\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openCMakeProject(dir.join("CMakeLists.txt")));
    EXPECT_TRUE(rig.workbench->hasProject());
    EXPECT_EQ(rig.workbench->solution(), nullptr);
    EXPECT_EQ(rig.workbench->projectRoot(), dir.path());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("CMakeLists.txt")));
    EXPECT_EQ(rig.window->tabs().count(), 1);
}

TEST(Workbench, ExplicitCMakeIdentitySurvivesRestoreAndRecent) {
    WorkDir dir("explicit-cmake-restore", "tiny_sln");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Example C)\n");
    const auto lists = dir.join("CMakeLists.txt");
    {
        WorkbenchRig first(dir);
        ASSERT_TRUE(first.workbench->openCMakeProject(lists));
        ASSERT_EQ(frost::app::recentProjects(*first.settings->snapshot()).front(), lists);
    }
    {
        WorkbenchRig second(dir);
        ASSERT_TRUE(second.workbench->restoreLastProject());
        EXPECT_EQ(second.workbench->solution(), nullptr);
        EXPECT_EQ(second.workbench->projectRoot(), dir.path());
        ASSERT_TRUE(second.workbench->closeProject());
        auto* welcome = second.workbench->welcomeScreen();
        ASSERT_NE(welcome, nullptr);
        ASSERT_FALSE(welcome->recentProjects().empty());
        welcome->activateRow(0);
        ASSERT_TRUE(waitFor([&] { return second.workbench->hasProject(); }));
        EXPECT_EQ(second.workbench->solution(), nullptr);
        EXPECT_EQ(second.workbench->projectRoot(), dir.path());
    }
}

TEST(Workbench, CMakeOpenDialogKeepsStandaloneUnsavedFile) {
    WorkDir dir("cmake-open-cancel");
    dir.write("CMakeLists.txt", "project(Example C)\n");
    dir.write("dirty.cpp", "int main() {}\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("dirty.cpp")));
    ASSERT_TRUE(rig.view()->editor().typeText("edit "));
    rig.prompts.openPath = dir.join("CMakeLists.txt");
    rig.window->menus().action("file.openFile")->trigger();
    EXPECT_EQ(rig.prompts.discardCalls, 0);
    EXPECT_TRUE(rig.workbench->hasProject());
    EXPECT_EQ(rig.window->tabs().count(), 1);
    EXPECT_TRUE(rig.window->tabs().isDirty(0));
}

// The same path with its ASCII letters upper-cased (on Windows the same file; unused elsewhere).
[[maybe_unused]] std::string upperCased(std::string path) {
    std::ranges::transform(path, path.begin(),
                           [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return path;
}

TEST(Workbench, SolutionFromTheCommandLineFillsTheExplorerAndTheConfigurations) {
    WorkDir dir("sln", "tiny_sln");
    WorkbenchRig rig(dir);
    EXPECT_FALSE(rig.workbench->hasProject());
    EXPECT_EQ(rig.window->windowTitle(), QStringLiteral("FrostIDE"));

    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    EXPECT_TRUE(rig.workbench->hasProject());
    ASSERT_NE(rig.workbench->solution(), nullptr);
    EXPECT_EQ(rig.window->windowTitle(), QStringLiteral("Tiny - FrostIDE"));

    // The Solution Explorer (the 'project' tool window, shown) holds the solution and its projects.
    frost::ui::SolutionExplorer* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(explorer->solution(), nullptr);
    EXPECT_EQ(explorer->solution()->projects().size(), 2U);
    EXPECT_GT(explorer->model()->rowCount(), 0);
    EXPECT_TRUE(rig.window->toolWindows().isVisible(tool_windows::kProject));

    // The configuration combo in the main toolbar lists the solution's configurations.
    QComboBox* combo = rig.workbench->configurationCombo();
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->itemText(0).toStdString(), "Debug | x64");
    EXPECT_EQ(combo->currentText().toStdString(), "Debug | x64");
    EXPECT_EQ(combo->parentWidget(), static_cast<QWidget*>(&rig.window->mainToolbar()));

    // The solution is now the newest recent project.
    const std::vector<std::string> recent = frost::app::recentProjects(*rig.settings->snapshot());
    ASSERT_FALSE(recent.empty());
    EXPECT_EQ(recent.front(), dir.join("Tiny.sln"));

    rig.workbench->closeProject();
    EXPECT_FALSE(rig.workbench->hasProject());
    EXPECT_EQ(rig.window->windowTitle(), QStringLiteral("FrostIDE"));
    EXPECT_EQ(explorer->solution(), nullptr);
    EXPECT_EQ(rig.window->tabs().emptyStateWidget(), static_cast<QWidget*>(rig.workbench->welcomeScreen()));
}

TEST(Workbench, OneTabPerFileWhateverThePathSpelling) {
    WorkDir dir("tabs", "tiny_sln");
    WorkbenchRig rig(dir);
    const std::string main = dir.join("App/main.cpp");
    ASSERT_TRUE(rig.workbench->openPath(main, {3, 1}).has_value());
    ASSERT_EQ(rig.window->tabs().count(), 1);
    DocumentEditorView* view = rig.view();
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->path(), main);
    EXPECT_EQ(view->status().line, 3);
    EXPECT_EQ(rig.window->windowTitle(), QStringLiteral("main.cpp - FrostIDE"));

    // A path with "..", and on Windows a case variant, activate the same tab and document.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Lib/lib.cpp")).has_value());
    ASSERT_EQ(rig.window->tabs().count(), 2);
    ASSERT_TRUE(
        rig.workbench->openPath(dir.path() + (kWindows ? "\\Lib\\..\\App\\main.cpp" : "/Lib/../App/main.cpp"), {5, 1})
            .has_value());
    EXPECT_EQ(rig.window->tabs().count(), 2);
    EXPECT_EQ(rig.workbench->currentView(), view);
    EXPECT_EQ(view->status().line, 5);
    if constexpr (kWindows) {
        auto canonical = frost::io::realFileSystem().canonicalPath(upperCased(main));
        ASSERT_TRUE(canonical.has_value()) << canonical.error().describe();
        EXPECT_EQ(*canonical, main); // The file's real case, from the file system.
        ASSERT_TRUE(rig.workbench->openPath(upperCased(main)).has_value());
        EXPECT_EQ(rig.window->tabs().count(), 2);
        EXPECT_EQ(rig.workbench->currentView(), view);
    }
    EXPECT_EQ(rig.workbench->documents().documents().size(), 2U);

    // A missing file is refused with a notification, and no tab opens.
    const std::size_t before = rig.window->notifications().log().size();
    EXPECT_FALSE(rig.workbench->openPath(dir.join("App/missing.cpp")).has_value());
    EXPECT_EQ(rig.window->tabs().count(), 2);
    EXPECT_GT(rig.window->notifications().log().size(), before);
}

TEST(Workbench, SessionAndHistoryRestoreUnicodeStatusColumns) {
    WorkDir dir("unicode-positions");
    dir.write("a.cpp", "A\xF0\x9F\x98\x80"
                       "e\xCC\x81"
                       "\xC3\xA9\tZ\n");
    dir.write("b.cpp", "other\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.cpp")));
    ASSERT_NE(rig.view(), nullptr);
    rig.view()->editor().setState(frost::text::EditorState::caretAt(frost::ByteOffset{11}));
    const auto saved = rig.view()->viewState();
    EXPECT_EQ(saved.column, 6);
    rig.view()->editor().goToLine(1, 1);
    rig.view()->restoreViewState(saved);
    EXPECT_EQ(rig.view()->editor().editor().state().primary().head, frost::ByteOffset{11});
    ASSERT_TRUE(rig.nav().openFile(dir.join("b.cpp")));
    ASSERT_TRUE(rig.nav().goBack());
    ASSERT_NE(rig.view(), nullptr);
    EXPECT_EQ(rig.view()->editor().editor().state().primary().head, frost::ByteOffset{11});
}

TEST(Workbench, FoldStateIsIgnoredWhenDocumentContentChanges) {
    WorkDir dir("fold-state");
    dir.write("a.cpp", "one\ntwo\nthree\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.cpp")));
    ASSERT_NE(rig.view(), nullptr);
    auto& editor = rig.view()->editor();
    ASSERT_TRUE(editor.setCollapsedFolds({{frost::ByteOffset{3}, frost::ByteOffset{8}}}));
    const auto saved = rig.view()->viewState();
    ASSERT_TRUE(editor.setCollapsedFolds({}));
    ASSERT_TRUE(editor.typeText("x"));
    rig.view()->restoreViewState(saved);
    EXPECT_TRUE(editor.collapsedFolds().empty());
}

TEST(Workbench, ClosedDocumentRestoresFoldsWhenReopened) {
    WorkDir dir("fold-reopen");
    dir.write("a.cpp", "one\ntwo\nthree\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.cpp")));
    ASSERT_NE(rig.view(), nullptr);
    ASSERT_TRUE(rig.view()->editor().setCollapsedFolds({{frost::ByteOffset{3}, frost::ByteOffset{8}}}));
    ASSERT_TRUE(rig.window->tabs().close(0));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.cpp")));
    ASSERT_NE(rig.view(), nullptr);
    EXPECT_EQ(rig.view()->editor().collapsedFolds().size(), 1U);
}

TEST(Workbench, TypingMarksTheTabDirtyAndCtrlSWritesTheExactBytes) {
    WorkDir dir("type");
    dir.write("src/a.cpp", "// a\r\nint main() { return 0; }\r\n");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("src/a.cpp")).has_value());
    DocumentEditorView* view = rig.view();
    ASSERT_NE(view, nullptr);
    QWidget* editor = view->widget();
    editor->setFocus();
    QApplication::processEvents();
    EXPECT_FALSE(rig.window->tabs().isDirty(0));

    sendKey(editor, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    EXPECT_TRUE(rig.window->tabs().isDirty(0));
    EXPECT_TRUE(view->status().dirty);
    EXPECT_EQ(dir.read("src/a.cpp"), "// a\r\nint main() { return 0; }\r\n"); // Nothing saved yet.

    // Ctrl+S is IntelliJ's Save All, through the shell's shortcut dispatcher.
    sendKey(editor, Qt::Key_S, Qt::ControlModifier);
    EXPECT_EQ(dir.read("src/a.cpp"), "x// a\r\nint main() { return 0; }\r\n");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
    EXPECT_FALSE(view->status().dirty);
    EXPECT_EQ(view->status().lineSeparator, "CRLF");
}

TEST(Workbench, ClosingADirtyTabSavesIt) {
    WorkDir dir("close");
    dir.write("a.txt", "one\n");
    dir.write("b.txt", "two\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.txt")).has_value());
    ASSERT_TRUE(rig.view()->editor().typeText("1"));
    EXPECT_TRUE(rig.window->tabs().isDirty(0));
    EXPECT_TRUE(rig.window->tabs().close(0));
    EXPECT_EQ(rig.window->tabs().count(), 0);
    EXPECT_EQ(dir.read("a.txt"), "1one\n");
    EXPECT_EQ(rig.prompts.discardCalls, 0); // Saved, so nobody was asked.
    EXPECT_TRUE(rig.workbench->documents().documents().empty());

    // When the save fails (the folder became a file's place), the user decides; Cancel keeps the tab.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("b.txt")).has_value());
    ASSERT_TRUE(rig.view()->editor().typeText("2"));
    ASSERT_TRUE(frost::io::realFileSystem().removeFile(dir.join("b.txt")).has_value());
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join("b.txt/blocked")).has_value());
    rig.prompts.discard = false;
    EXPECT_FALSE(rig.window->tabs().close(0));
    EXPECT_EQ(rig.prompts.discardCalls, 1);
    EXPECT_EQ(rig.window->tabs().count(), 1);
    rig.prompts.discard = true;
    EXPECT_TRUE(rig.window->tabs().close(0));
    EXPECT_EQ(rig.window->tabs().count(), 0);
}

TEST(Workbench, SaveAllRunsBeforeABuild) {
    WorkDir dir("build", "tiny_sln");
    // A fake MSBuild that records what main.cpp held on disk when the build started.
    frost::Mutex mutex;
    std::optional<std::string> atStart;
    RigOptions options;
    options.workbench.msbuildPath = dir.join("tools/MSBuild.exe");
    const std::string mainPath = dir.join("App/main.cpp");
    options.workbench.startBuild =
        [&](const frost::build::MsBuildRequest&,
            frost::build::BuildCallbacks) -> frost::Result<std::unique_ptr<frost::build::BuildSession>> {
        {
            frost::LockGuard lock(mutex);
            atStart = frost::io::realFileSystem().readFile(mainPath).value_or("<unreadable>");
        }
        return frost::fail(frost::ErrorCode::Unsupported, "the test's MSBuild stops here");
    };
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    ASSERT_TRUE(rig.workbench->openPath(mainPath).has_value());
    ASSERT_TRUE(rig.view()->editor().typeText("// edited\n"));
    ASSERT_TRUE(rig.window->tabs().isDirty(0));

    rig.window->menus().refresh(); // As opening the Build menu does.
    QAction* build = rig.window->menus().action(frost::ui::build_actions::kBuildSolution);
    ASSERT_NE(build, nullptr);
    ASSERT_TRUE(build->isEnabled());
    build->trigger();
    ASSERT_TRUE(waitFor([&] {
        frost::LockGuard lock(mutex);
        return atStart.has_value();
    }));
    {
        frost::LockGuard lock(mutex);
        EXPECT_TRUE(atStart->starts_with("// edited\n")) << *atStart;
    }
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
    // The Build tool window showed when the build started.
    EXPECT_TRUE(rig.window->toolWindows().isVisible(frost::app::kBuildToolWindow));
    EXPECT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }));
}

TEST(Workbench, SwitchingProjectsDuringBuildKeepsTheCurrentProjectAndTabs) {
    WorkDir dir("busy-build", "tiny_sln");
    WorkDir other("other-project");
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    RigOptions options;
    options.workbench.msbuildPath = dir.join("tools/MSBuild.exe");
    options.workbench.startBuild =
        [&](const frost::build::MsBuildRequest&,
            frost::build::BuildCallbacks) -> frost::Result<std::unique_ptr<frost::build::BuildSession>> {
        started = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!release && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return frost::fail(frost::ErrorCode::Unsupported, "test build finished");
    };
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/main.cpp")).has_value());
    rig.window->menus().refresh();
    QAction* build = rig.window->menus().action(frost::ui::build_actions::kBuildSolution);
    ASSERT_NE(build, nullptr);
    build->trigger();
    const bool running = waitFor([&] { return started.load(); });
    EXPECT_TRUE(running);
    if (running) {
        EXPECT_FALSE(rig.workbench->openPath(other.path()).has_value());
        EXPECT_FALSE(rig.workbench->closeProject());
        ASSERT_NE(rig.workbench->solution(), nullptr);
        EXPECT_EQ(rig.workbench->solution()->path(), dir.join("Tiny.sln"));
        EXPECT_EQ(rig.window->tabs().count(), 1);
    }
    release = true;
    EXPECT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }));
}

TEST(Workbench, WelcomeScreenListsRecentProjectsAndOpensOne) {
    WorkDir dir("welcome", "tiny_sln");
    const std::string solution = dir.join("Tiny.slnx");
    {
        WorkbenchRig first(dir);
        ASSERT_TRUE(first.workbench->openPath(solution).has_value());
    }
    WorkbenchRig rig(dir); // The next start: the same settings file.
    frost::app::WelcomeScreen* welcome = rig.workbench->welcomeScreen();
    ASSERT_NE(welcome, nullptr);
    EXPECT_EQ(rig.window->tabs().emptyStateWidget(), static_cast<QWidget*>(welcome));
    ASSERT_EQ(welcome->recentProjects().size(), 1U);
    EXPECT_EQ(welcome->recentProjects().front(), solution);
    EXPECT_TRUE(welcome->recentList()->item(0)->text().contains(QStringLiteral("Tiny.slnx")));

    welcome->activateRow(0);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->hasProject(); }));
    EXPECT_EQ(rig.workbench->solution()->path(), solution);

    // File > Open goes through the injected prompt; Remove from Recent Projects edits the setting.
    rig.prompts.openPath = dir.join("Lib/lib.h");
    rig.window->menus().action("file.openFile")->trigger();
    EXPECT_EQ(rig.prompts.openCalls, 1);
    EXPECT_EQ(rig.window->tabs().count(), 1);
    rig.workbench->closeProject();
    welcome->removeRow(0);
    EXPECT_TRUE(frost::app::recentProjects(*rig.settings->snapshot()).empty());
    EXPECT_TRUE(welcome->recentProjects().empty());
}

// The external change notification the navigation workbench posts for `path`'s dirty buffer.
const frost::ui::Notification* externalChangeNote(const WorkbenchRig& rig) {
    const auto& log = rig.window->notifications().log();
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (it->notification.title.contains(QStringLiteral("changed on disk"))) {
            return &it->notification;
        }
    }
    return nullptr;
}

// The hot-exit restore prompt (Restore, Discard) in the log, or nullptr.
const frost::ui::Notification* restorePrompt(const WorkbenchRig& rig) {
    const auto& log = rig.window->notifications().log();
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (it->notification.title.contains(QStringLiteral("unsaved files"))) {
            return &it->notification;
        }
    }
    return nullptr;
}

std::size_t restorePromptCount(const WorkbenchRig& rig) {
    return static_cast<std::size_t>(
        std::ranges::count_if(rig.window->notifications().log(), [](const frost::ui::NotificationEntry& entry) {
            return entry.notification.title.contains(QStringLiteral("unsaved files"));
        }));
}

TEST(Workbench, ExternalChangesReloadACleanBufferAndNotifyForADirtyOne) {
    WorkDir dir("external");
    dir.write("a.txt", "first\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.txt")).has_value());
    DocumentEditorView* view = rig.view();

    dir.write("a.txt", "second version\n");
    rig.workbench->checkExternalChanges();
    EXPECT_EQ(view->editor().document().text().text(), "second version\n");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));

    view->editor().goToLine(1, 1);
    ASSERT_TRUE(view->editor().typeText("mine "));
    dir.write("a.txt", "third from disk\n");
    const std::size_t before = rig.window->notifications().log().size();
    rig.workbench->checkExternalChanges();
    EXPECT_EQ(view->editor().document().text().text(), "mine second version\n"); // Untouched until decided.
    ASSERT_EQ(rig.window->notifications().log().size(), before + 1);
    const frost::ui::Notification& note = rig.window->notifications().log().back().notification;
    EXPECT_TRUE(note.title.contains(QStringLiteral("changed on disk"))) << note.title.toStdString();
    ASSERT_EQ(note.actions.size(), 3U);
    EXPECT_EQ(note.actions[0].label, QStringLiteral("Keep my version"));
    EXPECT_EQ(note.actions[1].label, QStringLiteral("Take disk version"));
    EXPECT_EQ(note.actions[2].label, QStringLiteral("Compare..."));
    note.actions[1].run();
    EXPECT_EQ(view->editor().document().text().text(), "third from disk\n");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
}

TEST(Workbench, KeepMyVersionKeepsTheBufferAndCompareOpensTheMergeView) {
    WorkDir dir("merge");
    dir.write("a.txt", "a\nb\nc\n");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.txt")).has_value());
    DocumentEditorView* view = rig.view();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->editor().typeText("X"));
    dir.write("a.txt", "Y\nb\nC\n");
    rig.workbench->checkExternalChanges();
    const frost::ui::Notification* note = externalChangeNote(rig);
    ASSERT_NE(note, nullptr);
    ASSERT_EQ(note->actions.size(), 3U);

    // Compare... opens shell-nav's merge view over this document.
    note->actions[2].run();
    auto* merge = rig.window->findChild<frost::ui::MergeView*>(QStringLiteral("mergeView"));
    ASSERT_NE(merge, nullptr);
    EXPECT_GT(merge->changes().size(), 0U);

    // Keep my version: the buffer stays, the merge is resolved and the merge view goes.
    note->actions[0].run();
    QApplication::processEvents();
    EXPECT_EQ(view->editor().document().text().text(), "Xa\nb\nc\n");
    EXPECT_TRUE(rig.window->tabs().isDirty(0));
    frost::document::FileDocument* document = rig.workbench->documents().find(dir.join("a.txt"));
    ASSERT_NE(document, nullptr);
    EXPECT_EQ(document->pendingMerge(), nullptr);
    EXPECT_TRUE(
        waitFor([&] { return rig.window->findChild<frost::ui::MergeView*>(QStringLiteral("mergeView")) == nullptr; }));
    // The next Save All writes the user's version.
    static_cast<void>(rig.workbench->saveAllDocuments());
    EXPECT_EQ(dir.read("a.txt"), "Xa\nb\nc\n");
}

TEST(Workbench, CtrlSSavesEveryDirtyDocument) {
    WorkDir dir("saveall");
    dir.write("a.txt", "one\n");
    dir.write("b.txt", "two\n");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.txt")).has_value());
    ASSERT_TRUE(rig.view(0)->editor().typeText("1"));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("b.txt")).has_value());
    ASSERT_TRUE(rig.view(1)->editor().typeText("2"));
    ASSERT_TRUE(rig.window->tabs().isDirty(0));
    ASSERT_TRUE(rig.window->tabs().isDirty(1));

    // One Ctrl+S in b.txt's editor saves both files (IntelliJ's Save All; one registration).
    QWidget* editor = rig.view(1)->widget();
    editor->setFocus();
    QApplication::processEvents();
    sendKey(editor, Qt::Key_S, Qt::ControlModifier);
    EXPECT_EQ(dir.read("a.txt"), "1one\n");
    EXPECT_EQ(dir.read("b.txt"), "2two\n");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
    EXPECT_FALSE(rig.window->tabs().isDirty(1));
    EXPECT_TRUE(rig.workbench->registry().find("file.saveAll").has_value());
}

TEST(Workbench, FrameDeactivationAutosaves) {
    WorkDir dir("autosave");
    dir.write("a.txt", "text\n");
    RigOptions options;
    options.autosave = true;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.txt")).has_value());
    ASSERT_TRUE(rig.view()->editor().typeText("more "));
    rig.workbench->frameDeactivated();
    EXPECT_EQ(dir.read("a.txt"), "more text\n");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
}

TEST(Workbench, UnsavedFilesOfAnEarlierSessionAreRestoredFromOnePrompt) {
    WorkDir dir("recovery");
    dir.write("a.txt", "saved\n");
    {
        WorkbenchRig first(dir);
        ASSERT_TRUE(first.workbench->openPath(dir.join("a.txt")).has_value());
        ASSERT_TRUE(first.view()->editor().typeText("unsaved "));
        // The rig shuts the workbench down as main() does: the journal keeps the dirty buffer.
    }
    EXPECT_EQ(dir.read("a.txt"), "saved\n");

    WorkbenchRig rig(dir);
    rig.show();
    EXPECT_EQ(rig.workbench->offerRecovery(), 1U);
    // The restore prompt appears once, with Restore and Discard.
    EXPECT_EQ(restorePromptCount(rig), 1U);
    EXPECT_EQ(rig.workbench->offerRecovery(), 0U); // Once per start.
    EXPECT_EQ(restorePromptCount(rig), 1U);
    const frost::ui::Notification* prompt = restorePrompt(rig);
    ASSERT_NE(prompt, nullptr);
    ASSERT_EQ(prompt->actions.size(), 2U);
    EXPECT_EQ(prompt->actions[0].label, QStringLiteral("Restore"));
    EXPECT_EQ(prompt->actions[1].label, QStringLiteral("Discard"));
    prompt->actions[0].run();
    ASSERT_EQ(rig.window->tabs().count(), 1);
    EXPECT_EQ(rig.view()->editor().document().text().text(), "unsaved saved\n");
    EXPECT_TRUE(rig.window->tabs().isDirty(0));
}

TEST(Workbench, DeclinedRecoveryDiscardsTheJournal) {
    WorkDir dir("discard");
    dir.write("a.txt", "saved\n");
    {
        WorkbenchRig first(dir);
        ASSERT_TRUE(first.workbench->openPath(dir.join("a.txt")).has_value());
        ASSERT_TRUE(first.view()->editor().typeText("lost "));
    }
    {
        WorkbenchRig second(dir);
        EXPECT_EQ(second.workbench->offerRecovery(), 1U);
        const frost::ui::Notification* prompt = restorePrompt(second);
        ASSERT_NE(prompt, nullptr);
        ASSERT_EQ(prompt->actions.size(), 2U);
        prompt->actions[1].run(); // Discard.
        EXPECT_EQ(second.window->tabs().count(), 0);
    }
    WorkbenchRig third(dir);
    EXPECT_EQ(third.workbench->offerRecovery(), 0U);
    EXPECT_EQ(restorePromptCount(third), 0U); // Nothing left to ask about.
}

TEST(Workbench, GoToFileAndTheSolutionExplorerOpenOneTabForOneFile) {
    WorkDir dir("one-tab", "tiny_sln");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    ASSERT_TRUE(waitFor([&] { return !rig.nav().scanPending(); }));
    const std::string main = dir.join("App/main.cpp");

    // The Solution Explorer opens the file under another spelling (".." and, on Windows, upper case).
    frost::ui::SolutionExplorer* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    std::string spelled = dir.path() + (kWindows ? "\\Lib\\..\\App\\main.cpp" : "/Lib/../App/main.cpp");
    if constexpr (kWindows) {
        spelled = upperCased(spelled);
    }
    emit explorer->openFile(QString::fromStdString(spelled));
    QApplication::processEvents();
    ASSERT_EQ(rig.window->tabs().count(), 1);
    ASSERT_NE(rig.view(), nullptr);
    EXPECT_EQ(rig.view()->path(), main); // The file's real spelling.

    // Go to File (the navigation workbench's popup) reaches the same tab.
    frost::ui::ListPopup* popup = rig.nav().showGoToFile(QStringLiteral("main.cpp"));
    ASSERT_NE(popup, nullptr);
    ASSERT_FALSE(popup->items().empty());
    popup->acceptCurrent();
    QApplication::processEvents();
    EXPECT_EQ(rig.window->tabs().count(), 1);
    EXPECT_EQ(rig.workbench->currentView(), rig.view());
    EXPECT_EQ(rig.workbench->documents().documents().size(), 1U);
    // Recent files (Ctrl+E) hold it once.
    EXPECT_EQ(rig.nav().recentFiles().paths().size(), 1U);
}

TEST(Workbench, CloseProjectKeepsATabWhoseSaveWasCancelled) {
    WorkDir dir("keep", "tiny_sln");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/main.cpp")).has_value());
    const std::string path = rig.view()->path();
    ASSERT_TRUE(rig.view()->editor().typeText("edit "));
    // The save fails (the file's place became a folder) and the user cancels the discard.
    ASSERT_TRUE(frost::io::realFileSystem().removeFile(path).has_value());
    ASSERT_TRUE(
        frost::io::realFileSystem().createDirectories(path + (kWindows ? "\\blocked" : "/blocked")).has_value());
    rig.prompts.discard = false;
    EXPECT_FALSE(rig.workbench->closeProject());
    EXPECT_EQ(rig.prompts.discardCalls, 1);
    EXPECT_EQ(rig.window->tabs().count(), 1);
    EXPECT_TRUE(rig.window->tabs().isDirty(0));
    EXPECT_TRUE(rig.workbench->hasProject());
    EXPECT_NE(rig.workbench->solution(), nullptr);
    // Opening another project goes through the same close and is refused the same way.
    EXPECT_FALSE(rig.workbench->openPath(dir.join("Lib")).has_value());
    EXPECT_NE(rig.workbench->solution(), nullptr);
    // Discarding lets it close.
    rig.prompts.discard = true;
    EXPECT_TRUE(rig.workbench->closeProject());
    EXPECT_FALSE(rig.workbench->hasProject());
    EXPECT_EQ(rig.window->tabs().count(), 0);
}

TEST(Workbench, AFolderProjectShowsTheFileSystemView) {
    WorkDir dir("folder-view", "tiny_sln");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App")).has_value());
    EXPECT_EQ(rig.workbench->solution(), nullptr);
    EXPECT_TRUE(rig.window->toolWindows().isVisible(frost::ui::kFileSystemToolWindow));
    EXPECT_FALSE(rig.window->toolWindows().isVisible(tool_windows::kProject));
    // A solution brings the Solution Explorer back.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    EXPECT_TRUE(rig.window->toolWindows().isVisible(tool_windows::kProject));
}

/*
 * The last project's session (ADR 0042) on the real file watcher: a folder opened in one run, with
 * a file in a tab, comes back with its tab at the next start-up (moved here from main_window_test's
 * PersistentServicesRestoreTheLastProject when AppServices merged into the Workbench). A closed
 * project is not reopened: the Welcome screen shows.
 */
TEST(Workbench, TheLastProjectComesBackWithItsTabsAndAClosedOneShowsWelcome) {
    WorkDir dir("session");
    const std::string source = "int main() {\n  return 0;\n}\n";
    dir.write("proj/main.cpp", source);
    const std::string project = dir.join("proj");
    RigOptions options;
    options.workbench.watchFiles = true;
    {
        WorkbenchRig first(dir, options);
        ASSERT_TRUE(first.workbench->openPath(project).has_value());
        ASSERT_TRUE(first.workbench->openPath(dir.join("proj/main.cpp")).has_value());
        ASSERT_NE(first.view(), nullptr);
        ASSERT_TRUE(first.view()->editor().setCollapsedFolds(
            {{frost::ByteOffset{source.find('\n')}, frost::ByteOffset{source.find('}')}}}));
        QApplication::processEvents();
    }
    {
        WorkbenchRig second(dir, options);
        EXPECT_TRUE(second.workbench->restoreLastProject());
        EXPECT_TRUE(second.workbench->hasProject());
        ASSERT_EQ(second.window->tabs().count(), 1);
        EXPECT_TRUE(second.window->tabs().editor(0)->path().ends_with("main.cpp"))
            << second.window->tabs().editor(0)->path();
        ASSERT_NE(second.view(), nullptr);
        ASSERT_EQ(second.view()->editor().collapsedFolds().size(), 1U);
        EXPECT_EQ(second.view()->editor().collapsedFolds().front().begin, frost::ByteOffset{source.find('\n')});
        // No journal was left behind by the first run's clean exit.
        auto scan =
            frost::storage::findRecoverableJournals(dir.join("data/hot-exit"), std::chrono::milliseconds(500), {});
        ASSERT_TRUE(scan.has_value()) << scan.error().describe();
        EXPECT_TRUE(scan->journals.size() <= 1U); // At most this run's own, which it holds.
        EXPECT_TRUE(second.workbench->closeProject());
    }
    WorkbenchRig third(dir, options);
    EXPECT_FALSE(third.workbench->restoreLastProject());
    EXPECT_FALSE(third.workbench->hasProject());
    EXPECT_EQ(third.window->tabs().emptyStateWidget(), static_cast<QWidget*>(third.workbench->welcomeScreen()));
    // Recent projects still list it for File > Open Recent and the Welcome screen.
    ASSERT_FALSE(frost::app::recentProjects(*third.settings->snapshot()).empty());
    EXPECT_EQ(frost::app::recentProjects(*third.settings->snapshot()).front(), project);
}

TEST(Workbench, TheRunToolWindowIsTheRunConsole) {
    WorkDir dir("run", "tiny_sln");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_NE(rig.workbench->runConsole(), nullptr);
    rig.window->toolWindows().show(tool_windows::kRun, false);
    EXPECT_EQ(rig.window->toolWindows().content(tool_windows::kRun),
              static_cast<QWidget*>(rig.workbench->runConsole()));
    // The solution's projects fill the startup project combo; Run is an action with Shift+F10.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    EXPECT_GT(rig.window->mainToolbar().runConfigurations()->count(), 0);
    EXPECT_TRUE(rig.workbench->registry().find("run.run").has_value());
}

TEST(Workbench, FolderWithOneSolutionOpensIt) {
    WorkDir dir("folder", "tiny_sln");
    WorkbenchRig rig(dir);
    // Tiny.sln and Tiny.slnx are one solution in two formats: the .sln opens.
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_NE(rig.workbench->solution(), nullptr);
    EXPECT_EQ(rig.workbench->solution()->path(), dir.join("Tiny.sln"));

    // A folder without one is the project itself.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App")).has_value());
    EXPECT_TRUE(rig.workbench->hasProject());
    EXPECT_EQ(rig.workbench->solution(), nullptr);
    EXPECT_EQ(rig.window->windowTitle(), QStringLiteral("App - FrostIDE"));
}

TEST(Workbench, PathHelpers) {
    EXPECT_TRUE(frost::app::isCFamilySource("a/b/Main.CPP"));
    EXPECT_TRUE(frost::app::isCFamilySource("x.h"));
    EXPECT_FALSE(frost::app::isCFamilySource("notes.txt"));
    EXPECT_FALSE(frost::app::isCFamilySource(".cpp"));
    EXPECT_TRUE(frost::app::isSolutionFile("C:\\x\\Game.SLN"));
    EXPECT_TRUE(frost::app::isSolutionFile("/x/Game.slnx"));
    EXPECT_FALSE(frost::app::isSolutionFile("/x/Game.vcxproj"));
}

} // namespace
