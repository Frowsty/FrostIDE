// SPDX-License-Identifier: Apache-2.0
//
// Offscreen functional tests of the workbench (frost/app/workbench.hpp, W1, ADR 0044): opening a
// solution from the command line, the one open-file path, typing, Ctrl+S (Save All), save on
// close, Save All before a build (a fake MSBuild), the Welcome screen and recent projects, external
// changes, autosave on frame deactivation and the hot-exit recovery. No real tool runs here (the
// msbuild and clangd suites do that).

#include "frost/app/open_file.hpp"
#include "frost/app/welcome_screen.hpp"
#include "frost/build/build_session.hpp"
#include "frost/core/mutex.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_menus.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "workbench_rig.hpp"
#include <QAction>
#include <QListWidget>
#include <QPushButton>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

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
    ASSERT_TRUE(rig.workbench->files()
                    .open(dir.path() + (kWindows ? "\\Lib\\..\\App\\main.cpp" : "/Lib/../App/main.cpp"), {5, 1})
                    .has_value());
    EXPECT_EQ(rig.window->tabs().count(), 2);
    EXPECT_EQ(rig.window->tabs().currentEditor(), static_cast<frost::ui::IEditorView*>(view));
    EXPECT_EQ(view->status().line, 5);
    if constexpr (kWindows) {
        auto canonical = frost::app::canonicalPath(upperCased(main));
        ASSERT_TRUE(canonical.has_value()) << canonical.error().describe();
        EXPECT_EQ(*canonical, main); // The file's real case, from the file system.
        ASSERT_TRUE(rig.workbench->openPath(upperCased(main)).has_value());
        EXPECT_EQ(rig.window->tabs().count(), 2);
        EXPECT_EQ(rig.window->tabs().currentEditor(), static_cast<frost::ui::IEditorView*>(view));
    }
    EXPECT_EQ(rig.workbench->documents().documents().size(), 2U);

    // A missing file is refused with a notification, and no tab opens.
    const std::size_t before = rig.window->notifications().log().size();
    EXPECT_FALSE(rig.workbench->openPath(dir.join("App/missing.cpp")).has_value());
    EXPECT_EQ(rig.window->tabs().count(), 2);
    EXPECT_GT(rig.window->notifications().log().size(), before);
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
    ASSERT_EQ(note.actions.size(), 2U);
    EXPECT_EQ(note.actions[0].label, QStringLiteral("Keep my version"));
    EXPECT_EQ(note.actions[1].label, QStringLiteral("Take disk version"));
    note.actions[1].run();
    EXPECT_EQ(view->editor().document().text().text(), "third from disk\n");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
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

TEST(Workbench, UnsavedFilesOfAnEarlierSessionAreRestored) {
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
    EXPECT_EQ(rig.workbench->offerRecovery(), 1);
    EXPECT_EQ(rig.prompts.restoreCalls, 1);
    EXPECT_EQ(rig.prompts.lastRestoreCount, 1);
    ASSERT_EQ(rig.window->tabs().count(), 1);
    EXPECT_EQ(rig.view()->editor().document().text().text(), "unsaved saved\n");
    EXPECT_TRUE(rig.window->tabs().isDirty(0));
    EXPECT_EQ(rig.workbench->offerRecovery(), 0); // Once per start.
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
        second.prompts.restore = false;
        EXPECT_EQ(second.workbench->offerRecovery(), 0);
        EXPECT_EQ(second.prompts.restoreCalls, 1);
        EXPECT_EQ(second.window->tabs().count(), 0);
    }
    WorkbenchRig third(dir);
    EXPECT_EQ(third.workbench->offerRecovery(), 0);
    EXPECT_EQ(third.prompts.restoreCalls, 0); // Nothing left to ask about.
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
