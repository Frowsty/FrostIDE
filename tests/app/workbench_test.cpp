// SPDX-License-Identifier: Apache-2.0
//
// Offscreen functional tests of the workbench (frost/app/workbench.hpp, W1, ADR 0044), the one
// composition root over the navigation workbench (ADR 0042): opening a solution from the command
// line, the one open-file path (Go to File and the Solution Explorer reach the same tab), typing,
// Ctrl+S (Save All), save on close, Close Project keeping a tab whose save was cancelled, Save All
// before a build (a fake MSBuild), the Welcome screen and recent projects, the session of the last
// project, a folder project's File System view, external changes (Compare opens the merge view),
// autosave on frame deactivation, the one hot-exit restore prompt, the Run tool window, and the
// read-only or refusing project dialogs and project-file edits (FND-3). No real tool runs here (the
// msbuild and clangd suites do that).

#include "frost/app/welcome_screen.hpp"
#include "frost/build/build_session.hpp"
#include "frost/build/vcxproj_user.hpp"
#include "frost/core/build_info.hpp"
#include "frost/core/mutex.hpp"
#include "frost/input/keymap.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/navigation_bar.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_menus.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/ui/workbench/merge_view.hpp"

#include "../../src/app/src/language_bridge.hpp"
#include "../../src/app/src/project_properties_dialog.hpp"
#include "workbench_rig.hpp"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class SaveWarningFs final : public frost::io::IFileSystem {
public:
    frost::Result<std::string> readFile(std::string_view path) override { return inner.readFile(path); }
    frost::Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        return inner.writeFileAtomic(path, data);
    }
    frost::Result<frost::io::SafeWriteResult> safeWrite(std::string_view path, std::string_view data,
                                                        const frost::io::SafeWriteOptions& options) override {
        auto result = inner.safeWrite(path, data, options);
        if (result && warn)
            result->warning = frost::Error(frost::ErrorCode::IoError, "recovery metadata needs attention");
        return result;
    }
    frost::Result<frost::io::FileStatus> status(std::string_view path) override { return inner.status(path); }
    frost::Result<std::vector<frost::io::DirEntry>> listDirectory(std::string_view path) override {
        return inner.listDirectory(path);
    }
    frost::Result<frost::io::DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        return inner.listDirectoryWithDiagnostics(path);
    }
    frost::Result<void> createDirectories(std::string_view path) override { return inner.createDirectories(path); }
    frost::Result<void> removeFile(std::string_view path) override { return inner.removeFile(path); }
    frost::Result<void> removeDirectory(std::string_view path) override { return inner.removeDirectory(path); }
    frost::Result<bool> exists(std::string_view path) override { return inner.exists(path); }
    frost::Result<std::string> canonicalPath(std::string_view path) override { return inner.canonicalPath(path); }
    bool warn = false;

private:
    frost::io::IFileSystem& inner = frost::io::realFileSystem();
};

using frost::app::DocumentEditorView;
using frost::app::IWorkbenchPrompts;
using frost::app::test::RigOptions;
using frost::app::test::sendKey;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
namespace tool_windows = frost::ui::tool_windows;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;

TEST(Workbench, ExplicitEditorSaveReportsCommittedWarning) {
    WorkDir dir("save-warning");
    dir.write("main.cpp", "int value;\n");
    SaveWarningFs fs;
    RigOptions options;
    options.fileSystem = &fs;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp")));
    ASSERT_NE(rig.view(), nullptr);
    ASSERT_TRUE(rig.view()->editor().typeText("// edit\n"));
    fs.warn = true;
    ASSERT_TRUE(rig.workbench->save(*rig.view()));
    EXPECT_FALSE(rig.view()->document().dirty());
    EXPECT_TRUE(dir.read("main.cpp").contains("// edit"));
    const auto& log = rig.window->notifications().log();
    ASSERT_FALSE(log.empty());
    EXPECT_EQ(log.back().notification.kind, frost::ui::NotificationKind::Warning);
    EXPECT_TRUE(log.back().notification.text.contains(QStringLiteral("recovery metadata needs attention")));
}

TEST(Workbench, TopLevelMenusReceiveClicksWithoutOpeningAnotherMenu) {
    WorkDir dir("menu-hit-testing");
    // The FrostEngine tool window is registered only for FrostEngine projects.
    dir.write("Game.frostproj", "{}");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    rig.show();
    auto* bar = rig.window->menuBar();
    const auto checkMenus = [&] {
        for (auto* action : bar->actions()) {
            if (!action->isVisible())
                continue;
            const auto local = bar->actionGeometry(action).center();
            auto* hit = rig.window->childAt(bar->mapTo(rig.window.get(), local));
            ASSERT_NE(hit, nullptr);
            ASSERT_EQ(hit, bar) << action->text().toStdString() << " intercepted by " << hit->metaObject()->className()
                                << " " << hit->objectName().toStdString();
            const auto global = bar->mapToGlobal(local);
            QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(hit, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QApplication::sendEvent(hit, &release);
            ASSERT_NE(action->menu(), nullptr);
            EXPECT_TRUE(action->menu()->isVisible()) << action->text().toStdString();
            action->menu()->close();
            QApplication::processEvents();
        }
    };
    checkMenus();
    auto* panel = rig.window->findChild<QWidget*>(QStringLiteral("frostEnginePanel"));
    ASSERT_NE(panel, nullptr);
    EXPECT_FALSE(panel->isVisible());
    rig.window->toolWindows().show("frostEngine");
    QApplication::processEvents();
    EXPECT_TRUE(panel->isVisible());
    checkMenus();
    rig.window->toolWindows().hide("frostEngine");
    QApplication::processEvents();
    EXPECT_FALSE(panel->isVisible());
    checkMenus();
}

TEST(Workbench, DirtyTabCloseReportsCommittedWarningWithoutDiscardPrompt) {
    WorkDir dir("close-save-warning");
    dir.write("main.cpp", "int value;\n");
    SaveWarningFs fs;
    RigOptions options;
    options.fileSystem = &fs;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp")));
    ASSERT_NE(rig.view(), nullptr);
    ASSERT_TRUE(rig.view()->editor().typeText("// edit\n"));
    fs.warn = true;
    ASSERT_TRUE(rig.window->tabs().close(0));
    EXPECT_EQ(rig.window->tabs().count(), 0);
    EXPECT_EQ(rig.prompts.discardCalls, 0);
    EXPECT_TRUE(dir.read("main.cpp").contains("// edit"));
    const auto& log = rig.window->notifications().log();
    ASSERT_FALSE(log.empty());
    EXPECT_EQ(log.back().notification.kind, frost::ui::NotificationKind::Warning);
    EXPECT_TRUE(log.back().notification.text.contains(QStringLiteral("recovery metadata needs attention")));
}

TEST(Workbench, ShutdownDoesNotAutosaveWhenFrameAutosaveIsDisabled) {
    WorkDir dir("shutdown-autosave-disabled");
    dir.write("main.cpp", "int value;\n");
    SaveWarningFs fs;
    RigOptions options;
    options.fileSystem = &fs;
    options.autosave = false;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp")));
    ASSERT_NE(rig.view(), nullptr);
    ASSERT_TRUE(rig.view()->editor().typeText("// unsaved\n"));
    fs.warn = true;
    rig.workbench->shutdown();
    EXPECT_EQ(dir.read("main.cpp"), "int value;\n");
    for (const auto& entry : rig.window->notifications().log())
        EXPECT_FALSE(entry.notification.title.contains(QStringLiteral("Saved")));
}

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
    // RUN-3: the run configuration combo replaces the CMake target combo on the toolbar.
    EXPECT_FALSE(rig.window->mainToolbar().runConfigurations()->isHidden());
    EXPECT_TRUE(rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarTargets"))->isHidden());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    rig.window->resize(1200, 750);
    QApplication::processEvents();
    EXPECT_EQ(rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarProfiles")), nullptr);
    EXPECT_EQ(rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarTargets")), nullptr);
    EXPECT_FALSE(rig.workbench->configurationCombo()->isHidden());
    EXPECT_FALSE(rig.window->mainToolbar().runConfigurations()->isHidden());
    EXPECT_EQ(rig.workbench->configurationCombo()->count(), 3);
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
        ASSERT_TRUE(second.settings->set(frost::settings::Scope::Application,
                                         frost::settings::key::generalReopenLastProject, true)); // Opt-in.
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
    ASSERT_EQ(combo->count(), 3);
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

TEST(Workbench, SplitViewsShareUndoAndClosingOneKeepsDirtyDocument) {
    WorkDir dir("split-shared");
    dir.write("a.txt", "one\n");
    dir.write("b.txt", "two\n");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.txt")));
    auto& tabs = rig.window->tabs();
    auto* firstGroup = tabs.activeGroup();
    DocumentEditorView* first = rig.workbench->editors().views().front();
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    ASSERT_EQ(rig.workbench->editors().views().size(), 2U);
    DocumentEditorView* second = rig.workbench->editors().views().back();
    ASSERT_EQ(&first->document(), &second->document());
    ASSERT_TRUE(second->editor().typeText("x"));
    EXPECT_EQ(first->editor().editor().display().version(), first->document().document().version());
    EXPECT_TRUE(first->document().dirty());
    tabs.setActiveGroup(firstGroup);
    first->widget()->setFocus();
    QApplication::processEvents();
    sendKey(first->widget(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(first->document().document().text().text(), "one\n");
    sendKey(first->widget(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(first->document().document().text().text(), "xone\n");
    ASSERT_TRUE(rig.workbench->openPath(dir.join("b.txt")));
    ASSERT_EQ(firstGroup->count(), 2);
    tabs.setActiveGroup(tabs.groups().back());
    ASSERT_TRUE(tabs.closeGroup());
    EXPECT_TRUE(first->document().dirty());
    EXPECT_EQ(dir.read("a.txt"), "one\n");
    tabs.setActiveGroup(firstGroup);
    ASSERT_TRUE(tabs.close(tabs.indexOf(dir.join("a.txt"))));
    EXPECT_EQ(dir.read("a.txt"), "xone\n");
}

TEST(Workbench, LanguageLocationUsesActiveSplitAndDocumentsAreUnique) {
    WorkDir dir("split-language-location");
    dir.write("a.cpp", "one\ntwo\nthree\n");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("a.cpp")));
    auto& tabs = rig.window->tabs();
    auto* first = rig.workbench->editors().views().front();
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    auto* second = rig.workbench->editors().views().back();
    frost::app::LanguageBridge bridge(*rig.workbench);
    ASSERT_EQ(bridge.openDocuments().size(), 1U);
    bridge.openLocation(dir.join("a.cpp"), frost::lang::LspPosition{2, 0});
    EXPECT_EQ(second->status().line, 3);
    EXPECT_EQ(first->status().line, 1);
}

// R0 follow-up 4: the language features get the profile's metadata cache (never the workspace),
// and a metadata source opens in an editor that stays read-only through a refresh; a normal
// location of the same workbench stays editable.
TEST(Workbench, MetadataSourcesOpenReadOnlyFromTheProfileCache) {
    WorkDir dir("metadata-read-only");
    dir.write("proj/a.cs", "class A {}\n");
    dir.write("data/caches/metadata/0123-Console.cs", "namespace System\n{\n    public static class Console {}\n}\n");
    WorkbenchRig rig(dir);
    rig.show();
    EXPECT_EQ(rig.workbench->metadataCacheDirectory(), dir.join("data/caches/metadata"));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("proj")));
    frost::app::LanguageBridge bridge(*rig.workbench);
    bridge.openLocation(dir.join("proj/a.cs"), frost::lang::LspPosition{0, 0});
    ASSERT_NE(rig.workbench->currentView(), nullptr);
    EXPECT_FALSE(rig.workbench->currentView()->editor().isReadOnly());

    const std::string cached = dir.join("data/caches/metadata/0123-Console.cs");
    bridge.openReadOnlyLocation(cached, frost::lang::LspPosition{2, 24});
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->path(), cached);
    EXPECT_EQ(view->status().line, 3);
    EXPECT_TRUE(view->forcedReadOnly());
    EXPECT_TRUE(view->editor().isReadOnly());
    view->refresh(); // A refresh (save, reload) re-reads the file facts: still read-only.
    EXPECT_TRUE(view->editor().isReadOnly());
    EXPECT_FALSE(view->editor().typeText("x"));
    EXPECT_EQ(dir.read("data/caches/metadata/0123-Console.cs"),
              "namespace System\n{\n    public static class Console {}\n}\n");
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
    ASSERT_TRUE(frost::app::addRecentProject(*rig.settings, other.path()).has_value());
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
        QMenu* switcher = rig.window->mainToolbar().projectButton()->menu();
        ASSERT_NE(switcher, nullptr);
        QMetaObject::invokeMethod(switcher, "aboutToShow", Qt::DirectConnection);
        QAction* otherAction = nullptr;
        for (QAction* action : switcher->actions()) {
            if (action->data().toString().toUtf8().toStdString() == other.path())
                otherAction = action;
        }
        ASSERT_NE(otherAction, nullptr);
        otherAction->trigger();
        QApplication::processEvents();
        QApplication::processEvents();
        EXPECT_FALSE(rig.workbench->openPath(other.path()).has_value());
        EXPECT_FALSE(rig.workbench->closeProject());
        ASSERT_NE(rig.workbench->solution(), nullptr);
        EXPECT_EQ(rig.workbench->solution()->path(), dir.join("Tiny.sln"));
        EXPECT_EQ(rig.window->tabs().count(), 1);
    }
    release = true;
    EXPECT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }));
}

TEST(Workbench, ProjectSwitcherOpensRecentIdentityAndKeepsCurrentOnMissingPath) {
    WorkDir dir("project-switcher", "tiny_sln");
    WorkDir other("project-switcher-folder");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(other.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.slnx")));
    auto* button = rig.window->mainToolbar().projectButton();
    ASSERT_EQ(button->focusPolicy(), Qt::TabFocus);
    ASSERT_NE(button->menu(), nullptr);
    EXPECT_EQ(button->accessibleName(), QStringLiteral("Project: Tiny"));
    auto* menu = button->menu();
    rig.show();
    rig.window->invoke(frost::ui::actions::kFocusToolbar);
    EXPECT_EQ(QApplication::focusWidget(), button);
    bool keyboardOpened = false;
    QTimer::singleShot(0, menu, [&] {
        keyboardOpened = menu->isVisible();
        menu->close();
    });
    sendKey(button, Qt::Key_Space, Qt::NoModifier);
    QApplication::processEvents();
    EXPECT_TRUE(keyboardOpened);
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    bool currentMarked = false;
    for (QAction* action : menu->actions()) {
        if (action->data().toString().toUtf8().toStdString() == dir.join("Tiny.slnx"))
            currentMarked = action->isChecked();
    }
    EXPECT_TRUE(currentMarked);
    const auto recentAction = [&](const std::string& path) -> QAction* {
        QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
        for (QAction* action : menu->actions()) {
            if (action->data().toString().toUtf8().toStdString() == path)
                return action;
        }
        return nullptr;
    };
    QAction* folder = recentAction(other.path());
    ASSERT_NE(folder, nullptr);
    folder->trigger();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->projectRoot() == other.path(); }));
    EXPECT_FALSE(rig.workbench->solution());
    ASSERT_EQ(frost::app::recentProjects(*rig.settings->snapshot()).front(), other.path());
    const std::string missing = dir.join("missing.sln");
    ASSERT_TRUE(frost::app::addRecentProject(*rig.settings, missing).has_value());
    QAction* stale = recentAction(missing);
    ASSERT_NE(stale, nullptr);
    stale->trigger();
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_EQ(rig.workbench->projectRoot(), other.path());
    QAction* solution = recentAction(dir.join("Tiny.slnx"));
    ASSERT_NE(solution, nullptr);
    solution->trigger();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->solution() != nullptr; }));
    EXPECT_EQ(rig.workbench->solution()->path(), dir.join("Tiny.slnx"));
}

TEST(Workbench, ProjectSwitcherHonorsCancelledDirtyClose) {
    WorkDir dir("switcher-dirty");
    WorkDir other("switcher-dirty-other");
    dir.write("source.cpp", "int value;\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("source.cpp")));
    ASSERT_TRUE(rig.view()->editor().typeText("x"));
    ASSERT_TRUE(frost::io::realFileSystem().removeFile(dir.join("source.cpp")).has_value());
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join("source.cpp/blocked")).has_value());
    ASSERT_TRUE(frost::app::addRecentProject(*rig.settings, other.path()).has_value());
    rig.prompts.discard = false;
    QMenu* menu = rig.window->mainToolbar().projectButton()->menu();
    ASSERT_NE(menu, nullptr);
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    QAction* target = nullptr;
    for (QAction* action : menu->actions()) {
        if (action->data().toString().toUtf8().toStdString() == other.path())
            target = action;
    }
    ASSERT_NE(target, nullptr);
    target->trigger();
    ASSERT_TRUE(waitFor([&] { return rig.prompts.discardCalls > 0; }));
    EXPECT_EQ(rig.workbench->projectRoot(), dir.path());
    EXPECT_TRUE(rig.window->tabs().isDirty(0));
    rig.prompts.discard = true;
}

TEST(Workbench, ProjectSwitcherOpenTreatsCMakeListsAsProjectIdentity) {
    WorkDir dir("switcher-cmake", "tiny_sln");
    dir.write("cmake/CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Example C)\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    rig.prompts.openPath = dir.join("cmake/CMakeLists.txt");
    QMenu* menu = rig.window->mainToolbar().projectButton()->menu();
    ASSERT_NE(menu, nullptr);
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    ASSERT_FALSE(menu->actions().empty());
    menu->actions().front()->trigger();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->projectRoot() == dir.join("cmake"); }));
    EXPECT_FALSE(rig.workbench->solution());
    EXPECT_EQ(rig.window->tabs().count(), 0);
    ASSERT_FALSE(frost::app::recentProjects(*rig.settings->snapshot()).empty());
    EXPECT_EQ(frost::app::recentProjects(*rig.settings->snapshot()).front(), dir.join("cmake/CMakeLists.txt"));
}

TEST(Workbench, SettingsDialogAppliesResetsAndDetectsExternalChanges) {
    WorkDir dir("settings-dialog");
    WorkbenchRig rig(dir);
    rig.show();
    rig.window->refreshActions();
    auto* gear = rig.window->mainToolbar().button(frost::ui::MainToolbar::kSettings);
    ASSERT_NE(gear, nullptr);
    ASSERT_TRUE(gear->isEnabled());
    gear->click();
    auto* dialog = rig.window->findChild<QDialog*>(QStringLiteral("settingsDialog"));
    ASSERT_NE(dialog, nullptr);
    auto* dialogHeader = dialog->findChild<QWidget*>(QStringLiteral("dialogTitleBar"));
    ASSERT_NE(dialogHeader, nullptr);
    EXPECT_TRUE((dialog->windowFlags() & Qt::FramelessWindowHint) != 0);
    auto* tabSize = dialog->findChild<QSpinBox*>(QStringLiteral("setting.editor.tabSize"));
    auto* search = dialog->findChild<QLineEdit*>(QStringLiteral("settingsSearch"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    ASSERT_NE(tabSize, nullptr);
    ASSERT_NE(search, nullptr);
    ASSERT_NE(buttons, nullptr);
    search->setText(QStringLiteral("no matching setting exists"));
    auto* empty = dialog->findChild<QLabel*>(QStringLiteral("settingsNoResults"));
    ASSERT_NE(empty, nullptr);
    EXPECT_TRUE(empty->isVisible());
    // No indentation detector exists, so its toggle is not offered (neither as a control nor in search).
    EXPECT_NE(dialog->findChild<QWidget*>(QStringLiteral("setting.editor.insertSpaces")), nullptr);
    EXPECT_EQ(dialog->findChild<QWidget*>(QStringLiteral("setting.editor.detectIndentation")), nullptr);
    search->setText(QStringLiteral("Detect and use existing file indents"));
    EXPECT_TRUE(empty->isVisible());
    search->setText(QStringLiteral("tab size"));
    EXPECT_TRUE(tabSize->isVisible());
    tabSize->setValue(6);
    // Another setting changed while this dialog was open survives Apply.
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::uiZoom, std::int64_t{125}));
    buttons->button(QDialogButtonBox::Apply)->click();
    EXPECT_EQ(rig.settings->snapshot()->get(frost::settings::key::editorTabSize), 6);
    EXPECT_EQ(rig.settings->snapshot()->get(frost::settings::key::uiZoom), 125);
    tabSize->setValue(7);
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::editorTabSize, std::int64_t{8}));
    buttons->button(QDialogButtonBox::Apply)->click();
    EXPECT_EQ(rig.settings->snapshot()->get(frost::settings::key::editorTabSize), 8);
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("settingsStatus"));
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("changed"), Qt::CaseInsensitive));
    QPushButton* reload = nullptr;
    for (auto* button : buttons->findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("Reload"))
            reload = button;
    ASSERT_NE(reload, nullptr);
    reload->click();
    EXPECT_EQ(tabSize->value(), 8);
    auto* reset = dialog->findChild<QPushButton*>(QStringLiteral("reset.editor.tabSize"));
    ASSERT_NE(reset, nullptr);
    EXPECT_EQ(reset->accessibleName(), QStringLiteral("Reset Tab size"));
    reset->click();
    buttons->button(QDialogButtonBox::Apply)->click();
    EXPECT_EQ(rig.settings->snapshot()->get(frost::settings::key::editorTabSize), 4);
    tabSize->setValue(9);
    buttons->button(QDialogButtonBox::Cancel)->click();
    EXPECT_EQ(rig.settings->snapshot()->get(frost::settings::key::editorTabSize), 4);
}

TEST(Workbench, KeymapSettingsStagesAppliesAndInvokesShortcut) {
    WorkDir dir("settings-keymap");
    WorkbenchRig rig(dir);
    int invocations = 0;
    frost::input::ActionSpec spec;
    spec.id = "test.fixtureShortcut";
    spec.name = "Fixture Shortcut";
    spec.group = "Tests";
    spec.handler = [&](const frost::input::ActionContext&, const frost::input::InvokeInfo&) { ++invocations; };
    const auto action = rig.workbench->registry().add(std::move(spec));
    ASSERT_TRUE(action);
    rig.show();
    rig.window->refreshActions();
    auto* gear = rig.window->mainToolbar().button(frost::ui::MainToolbar::kSettings);
    ASSERT_NE(gear, nullptr);
    gear->click();
    auto* dialog = rig.window->findChild<QDialog*>(QStringLiteral("settingsDialog"));
    ASSERT_NE(dialog, nullptr);
    auto* search = dialog->findChild<QLineEdit*>(QStringLiteral("settingsSearch"));
    auto* actions = dialog->findChild<QTreeWidget*>(QStringLiteral("keymapActions"));
    auto* shortcut = dialog->findChild<QLineEdit*>(QStringLiteral("keymapShortcut"));
    auto* set = dialog->findChild<QPushButton*>(QStringLiteral("keymapSet"));
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("keymapRemove"));
    auto* reset = dialog->findChild<QPushButton*>(QStringLiteral("keymapReset"));
    auto* message = dialog->findChild<QLabel*>(QStringLiteral("keymapMessage"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    ASSERT_TRUE(search && actions && shortcut && set && remove && reset && message && buttons);
    auto* offline = dialog->findChild<QCheckBox*>(QStringLiteral("setting.network.offline"));
    ASSERT_NE(offline, nullptr);
    search->setText(QStringLiteral("Work offline"));
    ASSERT_TRUE(offline->isVisible());
    search->setText(QStringLiteral("no such action"));
    search->setText(QStringLiteral("Fixture Shortcut"));
    QTreeWidgetItem* fixture = nullptr;
    for (int i = 0; i < actions->topLevelItemCount(); ++i)
        if (actions->topLevelItem(i)->text(0) == QStringLiteral("Fixture Shortcut"))
            fixture = actions->topLevelItem(i);
    ASSERT_NE(fixture, nullptr);
    EXPECT_FALSE(fixture->isHidden());
    EXPECT_EQ(actions->currentItem(), fixture);
    actions->setCurrentItem(fixture);
    shortcut->setText(QStringLiteral("a"));
    set->click();
    EXPECT_TRUE(message->text().contains(QStringLiteral("valid")));
    EXPECT_FALSE(buttons->button(QDialogButtonBox::Apply)->isEnabled());
    shortcut->setText(QStringLiteral("Ctrl+S"));
    set->click();
    EXPECT_TRUE(message->text().contains(QStringLiteral("Conflicts")));
    shortcut->setText(QStringLiteral("Ctrl+Alt+F12"));
    set->click();
    EXPECT_TRUE(buttons->button(QDialogButtonBox::Apply)->isEnabled());
    offline->setChecked(true);
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::uiZoom, std::int64_t{125}));
    buttons->button(QDialogButtonBox::Apply)->click();
    EXPECT_EQ(rig.settings->snapshot()->get(frost::settings::key::uiZoom), 125);
    EXPECT_TRUE(rig.settings->snapshot()->get(frost::settings::key::networkOffline));
    ASSERT_TRUE(waitFor([&] {
        const auto shortcuts = rig.workbench->keymap().shortcutsFor(*action);
        return !shortcuts.empty() && frost::input::formatSequence(shortcuts.front()) == "Ctrl+Alt+F12";
    }));
    remove->click();
    buttons->button(QDialogButtonBox::Cancel)->click();
    QApplication::processEvents();
    sendKey(rig.window.get(), Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_EQ(invocations, 1);
    const auto shortcuts = rig.workbench->keymap().shortcutsFor(*action);
    ASSERT_EQ(shortcuts.size(), 1U);
    EXPECT_EQ(frost::input::formatSequence(shortcuts.front()), "Ctrl+Alt+F12");
}

TEST(Workbench, KeymapSettingsRejectsConcurrentOverrideChangesAndRestoresDefaults) {
    WorkDir dir("settings-keymap-conflict");
    WorkbenchRig rig(dir);
    rig.show();
    rig.window->refreshActions();
    rig.window->mainToolbar().button(frost::ui::MainToolbar::kSettings)->click();
    auto* dialog = rig.window->findChild<QDialog*>(QStringLiteral("settingsDialog"));
    ASSERT_NE(dialog, nullptr);
    auto* actions = dialog->findChild<QTreeWidget*>(QStringLiteral("keymapActions"));
    auto* search = dialog->findChild<QLineEdit*>(QStringLiteral("settingsSearch"));
    auto* shortcut = dialog->findChild<QLineEdit*>(QStringLiteral("keymapShortcut"));
    auto* set = dialog->findChild<QPushButton*>(QStringLiteral("keymapSet"));
    auto* reset = dialog->findChild<QPushButton*>(QStringLiteral("keymapReset"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("settingsStatus"));
    ASSERT_TRUE(actions && search && shortcut && set && reset && buttons && status);
    search->setText(QStringLiteral("Save All"));
    QTreeWidgetItem* save = nullptr;
    for (int i = 0; i < actions->topLevelItemCount(); ++i)
        if (actions->topLevelItem(i)->data(0, Qt::UserRole).toString() == QStringLiteral("file.saveAll"))
            save = actions->topLevelItem(i);
    ASSERT_NE(save, nullptr);
    actions->setCurrentItem(save);
    shortcut->setText(QStringLiteral("Ctrl+Alt+F12"));
    set->click();
    frost::settings::RecordList external{frost::settings::makeRecord(
        {{"action", frost::settings::FieldValue(std::string("file.saveAll"))},
         {"shortcuts", frost::settings::FieldValue(frost::settings::StringList{"Ctrl+Alt+F11"})}})};
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::keymapOverrides, external));
    buttons->button(QDialogButtonBox::Apply)->click();
    EXPECT_TRUE(status->text().contains(QStringLiteral("changed"), Qt::CaseInsensitive));
    EXPECT_EQ(rig.settings->snapshot()->get(frost::settings::key::keymapOverrides), external);
    QPushButton* reload = nullptr;
    for (auto* button : buttons->findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("Reload"))
            reload = button;
    ASSERT_NE(reload, nullptr);
    reload->click();
    for (int i = 0; i < actions->topLevelItemCount(); ++i)
        if (actions->topLevelItem(i)->data(0, Qt::UserRole).toString() == QStringLiteral("file.saveAll"))
            actions->setCurrentItem(actions->topLevelItem(i));
    reset->click();
    buttons->button(QDialogButtonBox::Apply)->click();
    EXPECT_TRUE(rig.settings->snapshot()->get(frost::settings::key::keymapOverrides).empty());
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
        ASSERT_TRUE(second.settings->set(frost::settings::Scope::Application,
                                         frost::settings::key::generalReopenLastProject, true)); // Opt-in.
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

// RUN-3: the Run tool window holds one tab per run; runConsole() is the current tab's console.
TEST(Workbench, TheRunToolWindowHoldsTheRunConsoles) {
    WorkDir dir("run", "tiny_sln");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_NE(rig.workbench->runConsole(), nullptr);
    ASSERT_NE(rig.workbench->runToolWindow(), nullptr);
    rig.window->toolWindows().show(tool_windows::kRun, false);
    EXPECT_EQ(rig.window->toolWindows().content(tool_windows::kRun),
              static_cast<QWidget*>(rig.workbench->runToolWindow()));
    EXPECT_TRUE(rig.workbench->runToolWindow()->isAncestorOf(rig.workbench->runConsole()));
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

TEST(Workbench, NewProjectCreatesCMakeExecutableAndOpensStarterMain) {
    WorkDir dir("new-project");
    WorkbenchRig rig(dir);
    rig.show();

    auto* welcome = rig.workbench->welcomeScreen();
    ASSERT_NE(welcome, nullptr);
    ASSERT_NE(welcome->newProjectButton(), nullptr);
    ASSERT_NE(rig.window->menus().action("file.newProject"), nullptr);

    // Refuses invalid names (path traversal, Windows reserved device names) without touching disk.
    EXPECT_FALSE(
        rig.workbench
            ->createNewProject(
                {.name = "../escape", .location = dir.path(), .kind = frost::app::NewProjectTemplate::CMakeExecutable})
            .has_value());
    EXPECT_FALSE(
        rig.workbench
            ->createNewProject(
                {.name = "CON", .location = dir.path(), .kind = frost::app::NewProjectTemplate::CMakeExecutable})
            .has_value());
    EXPECT_FALSE(rig.workbench->hasProject());

    // Refuses a target directory that already exists and is non-empty, leaving existing files intact.
    dir.write("Existing/keep.txt", "untouched\n");
    EXPECT_FALSE(
        rig.workbench
            ->createNewProject(
                {.name = "Existing", .location = dir.path(), .kind = frost::app::NewProjectTemplate::CMakeExecutable})
            .has_value());
    EXPECT_EQ(dir.read("Existing/keep.txt"), "untouched\n");
    EXPECT_FALSE(frost::io::realFileSystem().exists(dir.join("Existing/CMakeLists.txt")).value_or(true));

    // Welcome screen's New Project button drives chooseNewProject and creates + opens a CMake executable project.
    rig.prompts.newProject = frost::app::NewProjectSpec{
        .name = "StarterApp",
        .location = dir.path(),
        .kind = frost::app::NewProjectTemplate::CMakeExecutable,
    };
    welcome->newProjectButton()->click();
    EXPECT_EQ(rig.prompts.newProjectPrompts, 1U);

    const std::string projectRoot = dir.join("StarterApp");
    const std::string cmakeLists = dir.join("StarterApp/CMakeLists.txt");
    const std::string mainCpp = dir.join("StarterApp/main.cpp");
    ASSERT_TRUE(rig.workbench->hasProject());
    EXPECT_EQ(rig.workbench->projectRoot(), projectRoot);
    EXPECT_NE(rig.workbench->cmakeService(), nullptr);
    EXPECT_TRUE(frost::io::realFileSystem().exists(cmakeLists).value_or(false));
    EXPECT_TRUE(frost::io::realFileSystem().exists(mainCpp).value_or(false));
    EXPECT_TRUE(dir.read("StarterApp/CMakeLists.txt").contains("add_executable(StarterApp"));
    EXPECT_TRUE(dir.read("StarterApp/main.cpp").contains("int main("));

    ASSERT_NE(rig.workbench->currentView(), nullptr);
    EXPECT_EQ(rig.workbench->currentView()->path(), mainCpp);
    ASSERT_NE(rig.nav().fileSystemView(), nullptr);
    EXPECT_TRUE(waitFor([&] { return rig.nav().fileSystemView()->selectedPath() == mainCpp; }));

    // Also supports creating and opening an EmptyFolder project via file.newProject.
    rig.prompts.newProject = frost::app::NewProjectSpec{
        .name = "PlainFolder",
        .location = dir.path(),
        .kind = frost::app::NewProjectTemplate::EmptyFolder,
    };
    rig.window->menus().action("file.newProject")->trigger();
    EXPECT_EQ(rig.prompts.newProjectPrompts, 2U);
    EXPECT_TRUE(rig.workbench->hasProject());
    EXPECT_EQ(rig.workbench->projectRoot(), dir.join("PlainFolder"));
    EXPECT_EQ(rig.workbench->cmakeService(), nullptr);
    EXPECT_EQ(rig.window->tabs().count(), 0);
}

TEST(Workbench, NewFileAndNewFolderInAppWorkbenchAndCMakeTargetMembership) {
    WorkDir dir("new-file-cmake");
    WorkbenchRig rig(dir);
    rig.show();

    auto createdProject = rig.workbench->createNewProject({
        .name = "Demo",
        .location = dir.path(),
        .kind = frost::app::NewProjectTemplate::CMakeExecutable,
    });
    ASSERT_TRUE(createdProject.has_value()) << createdProject.error().describe();
    const std::string originalLists = dir.read("Demo/CMakeLists.txt");

    // Creating a new .cpp file via file.new creates it on disk and opens it in a tab,
    // without silently mutating CMakeLists.txt.
    rig.prompts.newItemName = "helper.cpp";
    rig.window->menus().refresh();
    ASSERT_NE(rig.window->menus().action("file.new"), nullptr);
    rig.window->menus().action("file.new")->trigger();
    EXPECT_EQ(rig.prompts.newItemPrompts, 1U);
    EXPECT_FALSE(rig.prompts.lastNewItemFolder);

    const std::string helperPath = dir.join("Demo/helper.cpp");
    EXPECT_TRUE(frost::io::realFileSystem().exists(helperPath).value_or(false));
    ASSERT_NE(rig.workbench->currentView(), nullptr);
    EXPECT_EQ(rig.workbench->currentView()->path(), helperPath);
    EXPECT_EQ(dir.read("Demo/CMakeLists.txt"), originalLists);

    // addSourceToCMakeLists shows the edit first; declined, CMakeLists.txt keeps every byte.
    const auto noticesBefore = rig.window->notifications().log().size();
    auto declined = rig.workbench->addSourceToCMakeLists(helperPath);
    ASSERT_FALSE(declined.has_value());
    EXPECT_EQ(declined.error().code(), frost::ErrorCode::Cancelled);
    EXPECT_EQ(rig.prompts.cmakeEditPrompts, 1U);
    EXPECT_EQ(rig.prompts.lastCMakeEditPath, dir.join("Demo/CMakeLists.txt"));
    EXPECT_EQ(rig.prompts.lastCMakeEditBefore, originalLists);
    EXPECT_TRUE(rig.prompts.lastCMakeEditAfter.contains("helper.cpp")) << rig.prompts.lastCMakeEditAfter;
    EXPECT_EQ(dir.read("Demo/CMakeLists.txt"), originalLists);
    ASSERT_GT(rig.window->notifications().log().size(), noticesBefore);
    EXPECT_EQ(rig.window->notifications().log().back().notification.title,
              QStringLiteral("CMakeLists.txt not changed"));

    // Applied, the file on disk is exactly the previewed text, with helper.cpp added to add_executable(...).
    rig.prompts.cmakeEditAnswer = true;
    auto added = rig.workbench->addSourceToCMakeLists(helperPath);
    ASSERT_TRUE(added.has_value()) << added.error().describe();
    EXPECT_EQ(rig.prompts.cmakeEditPrompts, 2U);
    const std::string updatedLists = dir.read("Demo/CMakeLists.txt");
    EXPECT_EQ(updatedLists, rig.prompts.lastCMakeEditAfter);
    EXPECT_TRUE(updatedLists.contains("helper.cpp")) << updatedLists;
    EXPECT_TRUE(updatedLists.contains("main.cpp")) << updatedLists;

    // Creating a nested folder creates the directory on disk.
    auto folder = rig.workbench->createNewFolder("", "include/sub");
    ASSERT_TRUE(folder.has_value()) << folder.error().describe();
    EXPECT_EQ(*folder, dir.join("Demo/include/sub"));
    const auto folderStatus = frost::io::realFileSystem().status(*folder);
    ASSERT_TRUE(folderStatus.has_value());
    EXPECT_EQ(folderStatus->type, frost::io::FileType::Directory);

    // Refuses addSourceToCMakeLists when CMakeLists.txt has unsaved editor changes.
    auto extraFile = rig.workbench->createNewFile("", "extra.cpp");
    ASSERT_TRUE(extraFile.has_value()) << extraFile.error().describe();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Demo/CMakeLists.txt")).has_value());
    ASSERT_NE(rig.workbench->currentView(), nullptr);
    ASSERT_TRUE(rig.workbench->currentView()->editor().typeText("# dirty\n"));
    ASSERT_TRUE(rig.workbench->currentView()->document().dirty());
    EXPECT_FALSE(rig.workbench->addSourceToCMakeLists(*extraFile).has_value());
    EXPECT_FALSE(dir.read("Demo/CMakeLists.txt").contains("extra.cpp"));

    // Save/revert CMakeLists.txt and verify non-literal / conditional target declarations are refused cleanly.
    ASSERT_TRUE(rig.window->tabs().close(rig.window->tabs().indexOf(dir.join("Demo/CMakeLists.txt"))));
    const std::string conditionalLists = "cmake_minimum_required(VERSION 3.21)\n"
                                         "project(Demo LANGUAGES CXX)\n"
                                         "if(WIN32)\n"
                                         "    add_executable(Demo main.cpp)\n"
                                         "endif()\n";
    dir.write("Demo/CMakeLists.txt", conditionalLists);
    rig.workbench->checkExternalChanges();
    EXPECT_FALSE(rig.workbench->addSourceToCMakeLists(*extraFile).has_value());
    EXPECT_EQ(dir.read("Demo/CMakeLists.txt"), conditionalLists);
    // Refused edits are never previewed.
    EXPECT_EQ(rig.prompts.cmakeEditPrompts, 2U);
}

TEST(Workbench, SaveAsUpdatesAppWorkbenchEditorLspAndTabsWithoutDuplicates) {
    WorkDir dir("save-as");
    dir.write("a.cpp", "int a = 1;\n");
    WorkbenchRig rig(dir);
    rig.show();

    const std::string aPath = dir.join("a.cpp");
    const std::string bPath = dir.join("b.cpp");
    ASSERT_TRUE(rig.workbench->openPath(aPath).has_value());
    ASSERT_EQ(rig.window->tabs().count(), 1);
    DocumentEditorView* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->editor().typeText("// header\n"));
    ASSERT_TRUE(view->document().dirty());

    // Cancelling Save As (empty path) leaves the dirty buffer and path unchanged.
    rig.prompts.saveAsPath.clear();
    rig.window->menus().refresh();
    ASSERT_NE(rig.window->menus().action("file.saveAs"), nullptr);
    rig.window->menus().action("file.saveAs")->trigger();
    EXPECT_EQ(rig.prompts.saveAsPrompts, 1U);
    EXPECT_EQ(rig.prompts.lastSaveAsCurrentPath, aPath);
    EXPECT_EQ(rig.window->tabs().count(), 1);
    EXPECT_EQ(rig.workbench->currentView()->path(), aPath);
    EXPECT_TRUE(rig.workbench->currentView()->document().dirty());
    EXPECT_FALSE(frost::io::realFileSystem().exists(bPath).value_or(true));

    // Confirming Save As writes b.cpp, leaves a.cpp untouched on disk, and rebinds the single tab and registry entry.
    rig.prompts.saveAsPath = bPath;
    rig.window->menus().refresh();
    rig.window->menus().action("file.saveAs")->trigger();
    EXPECT_EQ(rig.prompts.saveAsPrompts, 2U);
    EXPECT_EQ(dir.read("a.cpp"), "int a = 1;\n");
    EXPECT_EQ(dir.read("b.cpp"), "// header\nint a = 1;\n");
    EXPECT_EQ(rig.window->tabs().count(), 1);
    ASSERT_NE(rig.workbench->currentView(), nullptr);
    EXPECT_EQ(rig.workbench->currentView()->path(), bPath);
    EXPECT_FALSE(rig.workbench->currentView()->document().dirty());
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
    EXPECT_EQ(rig.workbench->documentRegistry().viewCount(aPath), 0U);
    EXPECT_EQ(rig.workbench->documentRegistry().viewCount(bPath), 1U);
    EXPECT_EQ(rig.workbench->documents().find(aPath), nullptr);
    EXPECT_NE(rig.workbench->documents().find(bPath), nullptr);
}

TEST(Workbench, KeymapSettingsSwitchesBetweenIntelliJAndVisualStudioModes) {
    WorkDir dir("settings-keymap-vs");
    WorkbenchRig rig(dir);
    rig.show();
    rig.window->refreshActions();
    rig.window->mainToolbar().button(frost::ui::MainToolbar::kSettings)->click();
    auto* dialog = rig.window->findChild<QDialog*>(QStringLiteral("settingsDialog"));
    ASSERT_NE(dialog, nullptr);
    auto* keymapCombo = dialog->findChild<QComboBox*>(QStringLiteral("setting.keymap.id"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    ASSERT_NE(keymapCombo, nullptr);
    ASSERT_NE(buttons, nullptr);
    EXPECT_EQ(keymapCombo->currentData().toString(), QStringLiteral("intellij"));

    const int vsIndex = keymapCombo->findData(QStringLiteral("visual-studio"));
    ASSERT_GE(vsIndex, 0);
    keymapCombo->setCurrentIndex(vsIndex);
    EXPECT_TRUE(buttons->button(QDialogButtonBox::Apply)->isEnabled());
    buttons->button(QDialogButtonBox::Apply)->click();

    EXPECT_EQ(rig.settings->snapshot()->get(frost::settings::key::keymapId), "visual-studio");
    ASSERT_TRUE(waitFor([&] { return rig.workbench->keymap().keymap().id() == "visual-studio"; }));

    const auto buildId = rig.workbench->registry().find("build.buildSolution");
    ASSERT_TRUE(buildId.has_value());
    const auto buildShortcuts = rig.workbench->keymap().shortcutsFor(*buildId);
    ASSERT_FALSE(buildShortcuts.empty());
    EXPECT_EQ(frost::input::formatSequence(buildShortcuts.front()), "Ctrl+Shift+B");

    const auto debugId = rig.workbench->registry().find("run.debug");
    ASSERT_TRUE(debugId.has_value());
    const auto debugShortcuts = rig.workbench->keymap().shortcutsFor(*debugId);
    ASSERT_FALSE(debugShortcuts.empty());
    EXPECT_EQ(frost::input::formatSequence(debugShortcuts.front()), "F5");

    const auto saveAllId = rig.workbench->registry().find("file.saveAll");
    ASSERT_TRUE(saveAllId.has_value());
    const auto saveAllShortcuts = rig.workbench->keymap().shortcutsFor(*saveAllId);
    ASSERT_FALSE(saveAllShortcuts.empty());
    EXPECT_EQ(frost::input::formatSequence(saveAllShortcuts.front()), "Ctrl+Shift+S");

    buttons->button(QDialogButtonBox::Cancel)->click();
}

/*
 * UX-21's keymap picker is Settings > Keymap (never the launcher, never a first-run step): it lists
 * every built-in keymap, the default stays IntelliJ, and choosing the ReSharper or Visual Studio 2022
 * keymap there applies it live, with their documented shortcuts on the workbench's real actions.
 * Compile (Ctrl+F7, SLN-9) comes from the build module's defaults, which neither keymap overrides.
 */
TEST(Workbench, TheKeymapPickerOffersEveryBuiltinKeymapAndAppliesReSharperAndVisualStudio2022) {
    WorkDir dir("settings-keymap-picker");
    WorkbenchRig rig(dir);
    rig.show();
    EXPECT_EQ(rig.workbench->keymap().keymap().id(), "intellij") << "the default keymap is unchanged";
    const auto primary = [&](std::string_view action) -> std::string {
        const auto id = rig.workbench->registry().find(action);
        if (!id)
            return "(no action " + std::string(action) + ")";
        const auto list = rig.workbench->keymap().shortcutsFor(*id);
        return list.empty() ? std::string("(unbound)") : frost::input::formatSequence(list.front());
    };
    const struct {
        const char* keymap;
        std::vector<std::pair<std::string_view, std::string_view>> shortcuts;
    } choices[] = {
        {"resharper",
         {{"refactor.this", "Ctrl+Shift+R"},
          {"code.showIntentions", "Alt+Enter"},
          {"navigate.gotoFile", "Ctrl+Shift+T"},
          {"build.buildSolution", "Ctrl+Shift+B"},
          {"build.compileFile", "Ctrl+F7"}}},
        {"visual-studio-2022",
         {{"navigate.declaration", "F12"},
          {"refactor.rename", "Ctrl+R, Ctrl+R"},
          {"file.saveDocument", "Ctrl+S"},
          {"build.buildSolution", "Ctrl+Shift+B"},
          {"build.compileFile", "Ctrl+F7"}}},
    };
    for (const auto& choice : choices) {
        rig.window->refreshActions();
        rig.window->mainToolbar().button(frost::ui::MainToolbar::kSettings)->click();
        auto* dialog = rig.window->findChild<QDialog*>(QStringLiteral("settingsDialog"));
        ASSERT_NE(dialog, nullptr);
        auto* keymapCombo = dialog->findChild<QComboBox*>(QStringLiteral("setting.keymap.id"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        ASSERT_NE(keymapCombo, nullptr);
        ASSERT_NE(buttons, nullptr);
        std::set<std::string> listed;
        for (int i = 0; i < keymapCombo->count(); ++i)
            listed.insert(keymapCombo->itemData(i).toString().toStdString());
        for (const std::string_view id : frost::input::builtinKeymapIds())
            EXPECT_TRUE(listed.contains(std::string(id))) << "the picker does not offer " << id;
        EXPECT_EQ(listed.size(), frost::input::builtinKeymapIds().size());

        const int index = keymapCombo->findData(QString::fromLatin1(choice.keymap));
        ASSERT_GE(index, 0) << choice.keymap;
        keymapCombo->setCurrentIndex(index);
        buttons->button(QDialogButtonBox::Ok)->click();
        EXPECT_EQ(rig.settings->snapshot()->get(frost::settings::key::keymapId), choice.keymap);
        ASSERT_TRUE(waitFor([&] { return rig.workbench->keymap().keymap().id() == choice.keymap; })) << choice.keymap;
        for (const auto& [action, shortcut] : choice.shortcuts)
            EXPECT_EQ(primary(action), shortcut) << choice.keymap << " " << action;
    }
}

TEST(Workbench, NewProjectSupportsVisualStudioCppLibrariesAndDotNetTemplates) {
    WorkDir dir("new-project-all-templates");
    WorkbenchRig rig(dir);
    rig.show();

    // 1. Visual Studio C++ Console Application (.sln + .vcxproj + .vcxproj.filters)
    {
        auto created = rig.workbench->createNewProject({
            .name = "VsConsoleApp",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::VsCppConsole,
        });
        ASSERT_TRUE(created.has_value()) << created.error().describe();
        EXPECT_EQ(*created, dir.join("VsConsoleApp"));
        ASSERT_NE(rig.workbench->solution(), nullptr);
        EXPECT_TRUE(rig.workbench->solution()->warnings().empty());
        ASSERT_EQ(rig.workbench->solution()->projects().size(), 1U);
        ASSERT_TRUE(rig.workbench->solution()->projects().front().vcxproj.has_value());
        EXPECT_EQ(rig.workbench->solution()->projects().front().vcxproj->configurationType, "Application");
        EXPECT_TRUE(rig.window->toolWindows().isVisible(frost::ui::tool_windows::kProject));
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("VsConsoleApp/main.cpp"));
    }

    // 2. Visual Studio C++ Console Application (.slnx + .vcxproj + .vcxproj.filters)
    {
        auto created = rig.workbench->createNewProject({
            .name = "SlnxConsoleApp",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::SlnxCppConsole,
        });
        ASSERT_TRUE(created.has_value()) << created.error().describe();
        ASSERT_NE(rig.workbench->solution(), nullptr);
        EXPECT_TRUE(rig.workbench->solution()->warnings().empty());
        EXPECT_EQ(rig.workbench->solution()->path(), dir.join("SlnxConsoleApp/SlnxConsoleApp.slnx"));
        ASSERT_EQ(rig.workbench->solution()->projects().size(), 1U);
        ASSERT_TRUE(rig.workbench->solution()->projects().front().vcxproj.has_value());
        EXPECT_EQ(rig.workbench->solution()->projects().front().vcxproj->configurationType, "Application");
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("SlnxConsoleApp/main.cpp"));
    }

    // 3. Visual Studio C++ Static Library (.sln + .vcxproj)
    {
        auto created = rig.workbench->createNewProject({
            .name = "VsStaticLib",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::VsCppStaticLibrary,
        });
        ASSERT_TRUE(created.has_value()) << created.error().describe();
        ASSERT_NE(rig.workbench->solution(), nullptr);
        EXPECT_TRUE(rig.workbench->solution()->warnings().empty());
        ASSERT_EQ(rig.workbench->solution()->projects().size(), 1U);
        ASSERT_TRUE(rig.workbench->solution()->projects().front().vcxproj.has_value());
        EXPECT_EQ(rig.workbench->solution()->projects().front().vcxproj->configurationType, "StaticLibrary");
        EXPECT_TRUE(frost::io::realFileSystem().exists(dir.join("VsStaticLib/VsStaticLib.h")).value_or(false));
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("VsStaticLib/VsStaticLib.cpp"));
    }

    // 4. Visual Studio C++ Dynamic-Link Library (.sln + .vcxproj)
    {
        auto created = rig.workbench->createNewProject({
            .name = "VsDynamicLib",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::VsCppDynamicLibrary,
        });
        ASSERT_TRUE(created.has_value()) << created.error().describe();
        ASSERT_NE(rig.workbench->solution(), nullptr);
        EXPECT_TRUE(rig.workbench->solution()->warnings().empty());
        ASSERT_EQ(rig.workbench->solution()->projects().size(), 1U);
        ASSERT_TRUE(rig.workbench->solution()->projects().front().vcxproj.has_value());
        EXPECT_EQ(rig.workbench->solution()->projects().front().vcxproj->configurationType, "DynamicLibrary");
        const std::string header = dir.read("VsDynamicLib/VsDynamicLib.h");
        EXPECT_TRUE(header.contains("__declspec(dllexport)")) << header;
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("VsDynamicLib/VsDynamicLib.cpp"));
    }

    // 5. CMake Static & Shared Libraries
    {
        auto staticLib = rig.workbench->createNewProject({
            .name = "CMakeStatic",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::CMakeStaticLibrary,
        });
        ASSERT_TRUE(staticLib.has_value()) << staticLib.error().describe();
        EXPECT_NE(rig.workbench->cmakeService(), nullptr);
        EXPECT_TRUE(dir.read("CMakeStatic/CMakeLists.txt").contains("add_library(CMakeStatic STATIC"));
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("CMakeStatic/CMakeStatic.cpp"));

        auto sharedLib = rig.workbench->createNewProject({
            .name = "CMakeShared",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::CMakeSharedLibrary,
        });
        ASSERT_TRUE(sharedLib.has_value()) << sharedLib.error().describe();
        EXPECT_NE(rig.workbench->cmakeService(), nullptr);
        EXPECT_TRUE(dir.read("CMakeShared/CMakeLists.txt").contains("add_library(CMakeShared SHARED"));
        EXPECT_TRUE(dir.read("CMakeShared/CMakeShared.hpp").contains("CMAKESHARED_API"));
    }

    // 6. .NET / C# Console App & Class Library
    {
        auto dotnetConsole = rig.workbench->createNewProject({
            .name = "ManagedApp",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::DotNetConsole,
        });
        ASSERT_TRUE(dotnetConsole.has_value()) << dotnetConsole.error().describe();
        EXPECT_EQ(rig.workbench->projectRoot(), dir.join("ManagedApp"));
        ASSERT_NE(rig.workbench->solution(), nullptr);
        EXPECT_EQ(rig.workbench->solution()->path(), dir.join("ManagedApp/ManagedApp.slnx"));
        ASSERT_EQ(rig.workbench->solution()->projects().size(), 1U);
        EXPECT_EQ(rig.workbench->solution()->projects()[0].kind, frost::project::ProjectKind::CSharp);
        EXPECT_EQ(rig.workbench->solution()->projects()[0].nodeKind, frost::project::NodeKind::Project);
        EXPECT_TRUE(rig.workbench->solution()->findFileNode(dir.join("ManagedApp/Program.cs")).has_value());
        const std::string csproj = dir.read("ManagedApp/ManagedApp.csproj");
        EXPECT_TRUE(csproj.contains("<OutputType>Exe</OutputType>")) << csproj;
        EXPECT_TRUE(csproj.contains("<TargetFramework>net10.0</TargetFramework>")) << csproj;
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("ManagedApp/Program.cs"));

        auto dotnetLib = rig.workbench->createNewProject({
            .name = "ManagedLib",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::DotNetClassLibrary,
        });
        ASSERT_TRUE(dotnetLib.has_value()) << dotnetLib.error().describe();
        EXPECT_EQ(rig.workbench->projectRoot(), dir.join("ManagedLib"));
        ASSERT_NE(rig.workbench->solution(), nullptr);
        EXPECT_EQ(rig.workbench->solution()->path(), dir.join("ManagedLib/ManagedLib.slnx"));
        ASSERT_EQ(rig.workbench->solution()->projects().size(), 1U);
        EXPECT_EQ(rig.workbench->solution()->projects()[0].kind, frost::project::ProjectKind::CSharp);
        EXPECT_EQ(rig.workbench->solution()->projects()[0].nodeKind, frost::project::NodeKind::Project);
        EXPECT_TRUE(rig.workbench->solution()->findFileNode(dir.join("ManagedLib/Class1.cs")).has_value());
        const std::string libCsproj = dir.read("ManagedLib/ManagedLib.csproj");
        EXPECT_FALSE(libCsproj.contains("<OutputType>Exe</OutputType>")) << libCsproj;
        EXPECT_TRUE(libCsproj.contains("<TargetFramework>net10.0</TargetFramework>")) << libCsproj;
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("ManagedLib/Class1.cs"));
    }
}

TEST(Workbench, RenameMoveAndDeleteEditCMakeListsOnlyAfterAPreviewAndNeverMsBuildProjects) {
    WorkDir dir("rename-delete-project-items");
    WorkbenchRig rig(dir);
    rig.show();
    rig.prompts.cmakeEditAnswer = true; // Part 1 applies the previewed CMakeLists.txt edits.
    const auto lastNotice = [&]() -> const frost::ui::Notification& {
        return rig.window->notifications().log().back().notification;
    };

    // 1. CMake project: rename dirty main.cpp -> entry.cpp updates CMakeLists.txt and preserves dirty tab.
    {
        auto created = rig.workbench->createNewProject({
            .name = "CMakeRenameDemo",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::CMakeExecutable,
        });
        ASSERT_TRUE(created.has_value()) << created.error().describe();
        const std::string mainCpp = dir.join("CMakeRenameDemo/main.cpp");
        const std::string entryCpp = dir.join("CMakeRenameDemo/entry.cpp");
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        EXPECT_EQ(rig.workbench->currentView()->path(), mainCpp);
        ASSERT_TRUE(rig.workbench->currentView()->editor().typeText("// dirty edit\n"));
        EXPECT_TRUE(rig.workbench->currentView()->document().dirty());

        auto renamed = rig.workbench->renameItem(mainCpp, "entry.cpp");
        ASSERT_TRUE(renamed.has_value()) << renamed.error().describe();
        EXPECT_EQ(*renamed, entryCpp);
        EXPECT_FALSE(frost::io::realFileSystem().exists(mainCpp).value_or(true));
        EXPECT_TRUE(frost::io::realFileSystem().exists(entryCpp).value_or(false));
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        EXPECT_EQ(rig.workbench->currentView()->path(), entryCpp);
        EXPECT_TRUE(rig.workbench->currentView()->document().dirty());
        EXPECT_TRUE(
            rig.workbench->currentView()->document().document().committedText().text().starts_with("// dirty edit\n"));
        const std::string cmakeLists = dir.read("CMakeRenameDemo/CMakeLists.txt");
        EXPECT_EQ(rig.prompts.cmakeEditPrompts, 1U);
        EXPECT_EQ(cmakeLists, rig.prompts.lastCMakeEditAfter);
        EXPECT_TRUE(cmakeLists.contains("entry.cpp")) << cmakeLists;
        EXPECT_FALSE(cmakeLists.contains("main.cpp")) << cmakeLists;

        // Add a second file, add to CMakeLists.txt, then delete it: closes tab without discard prompt and removes from
        // CMakeLists.txt.
        auto helper = rig.workbench->createNewFile("", "helper.cpp");
        ASSERT_TRUE(helper.has_value()) << helper.error().describe();
        ASSERT_TRUE(rig.workbench->addSourceToCMakeLists(*helper).has_value());
        EXPECT_TRUE(dir.read("CMakeRenameDemo/CMakeLists.txt").contains("helper.cpp"));
        ASSERT_TRUE(rig.workbench->currentView()->editor().typeText("// unsaved helper\n"));
        const int discardBefore = rig.prompts.discardCalls;
        auto deleted = rig.workbench->deleteItem(*helper);
        ASSERT_TRUE(deleted.has_value()) << deleted.error().describe();
        EXPECT_EQ(rig.prompts.discardCalls, discardBefore);
        EXPECT_FALSE(frost::io::realFileSystem().exists(*helper).value_or(true));
        EXPECT_FALSE(dir.read("CMakeRenameDemo/CMakeLists.txt").contains("helper.cpp"));
        EXPECT_EQ(rig.prompts.cmakeEditPrompts, 3U); // Add and remove were previewed too.

        // Declined: the file is renamed on disk, CMakeLists.txt keeps every byte and the user is told.
        rig.prompts.cmakeEditAnswer = false;
        const std::string listsBefore = dir.read("CMakeRenameDemo/CMakeLists.txt");
        auto declined = rig.workbench->renameItem(entryCpp, "start.cpp");
        ASSERT_TRUE(declined.has_value()) << declined.error().describe();
        EXPECT_EQ(rig.prompts.cmakeEditPrompts, 4U);
        EXPECT_TRUE(frost::io::realFileSystem().exists(dir.join("CMakeRenameDemo/start.cpp")).value_or(false));
        EXPECT_EQ(dir.read("CMakeRenameDemo/CMakeLists.txt"), listsBefore);
        EXPECT_EQ(lastNotice().title, QStringLiteral("CMakeLists.txt not changed"));
        EXPECT_TRUE(lastNotice().text.contains(QStringLiteral("entry.cpp"))) << lastNotice().text.toStdString();
        // Save start.cpp so the project can close cleanly for the next subtest.
        ASSERT_NE(rig.workbench->currentView(), nullptr);
        ASSERT_TRUE(rig.workbench->documents().save(rig.workbench->currentView()->document()).has_value());
    }

    /*
     * 2. Visual Studio C++ project (.sln + .vcxproj): rename, move and delete happen on disk. This
     * rig has no ProjectHost helper, so the project edit that would follow (SLN-6; with the real
     * helper in WorkbenchProjectEdit.RenameAndDeleteUpdateTheProjectThroughProjectHost) cannot
     * start: .vcxproj and .vcxproj.filters keep every byte and the user is told which still list
     * the file, and why.
     */
    {
        auto created = rig.workbench->createNewProject({
            .name = "VsRenameDemo",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::VsCppConsole,
        });
        ASSERT_TRUE(created.has_value()) << created.error().describe();
        const std::string vcxprojBefore = dir.read("VsRenameDemo/VsRenameDemo.vcxproj");
        const std::string filtersBefore = dir.read("VsRenameDemo/VsRenameDemo.vcxproj.filters");
        const auto expectProjectsUntouched = [&](const char* what) {
            EXPECT_EQ(dir.read("VsRenameDemo/VsRenameDemo.vcxproj"), vcxprojBefore) << what;
            EXPECT_EQ(dir.read("VsRenameDemo/VsRenameDemo.vcxproj.filters"), filtersBefore) << what;
            EXPECT_EQ(lastNotice().title, QStringLiteral("Project files not updated")) << what;
            EXPECT_EQ(lastNotice().kind, frost::ui::NotificationKind::Warning) << what;
            EXPECT_TRUE(lastNotice().text.contains(QStringLiteral("No project file was changed"))) << what;
            EXPECT_TRUE(lastNotice().text.contains(QStringLiteral("VsRenameDemo.vcxproj"))) << what;
        };
        const std::string mainCpp = dir.join("VsRenameDemo/main.cpp");
        const std::string appCpp = dir.join("VsRenameDemo/app.cpp");
        auto renamed = rig.workbench->renameItem(mainCpp, "app.cpp");
        ASSERT_TRUE(renamed.has_value()) << renamed.error().describe();
        EXPECT_EQ(*renamed, appCpp);
        EXPECT_TRUE(frost::io::realFileSystem().exists(appCpp).value_or(false));
        expectProjectsUntouched("rename");
        EXPECT_TRUE(lastNotice().text.contains(QStringLiteral("renamed"))) << lastNotice().text.toStdString();

        // The project still lists main.cpp, so moving app.cpp names nothing the project lists: no notice.
        const auto noticesBefore = rig.window->notifications().log().size();
        auto* explorer = rig.workbench->solutionExplorer();
        ASSERT_NE(explorer, nullptr);
        ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join("VsRenameDemo/src")).has_value());
        emit explorer->movePhysicalPathsRequested({QString::fromStdString(appCpp)},
                                                  QString::fromStdString(dir.join("VsRenameDemo/src")), QString());
        EXPECT_TRUE(frost::io::realFileSystem().exists(dir.join("VsRenameDemo/src/app.cpp")).value_or(false));
        EXPECT_EQ(rig.window->notifications().log().size(), noticesBefore);
        EXPECT_EQ(dir.read("VsRenameDemo/VsRenameDemo.vcxproj"), vcxprojBefore);

        // Back to main.cpp, which the project lists: moving it is reported, deleting it too.
        dir.write("VsRenameDemo/main.cpp", "int main() { return 0; }\n");
        rig.workbench->checkExternalChanges();
        emit explorer->movePhysicalPathsRequested({QString::fromStdString(mainCpp)},
                                                  QString::fromStdString(dir.join("VsRenameDemo/src")), QString());
        EXPECT_TRUE(frost::io::realFileSystem().exists(dir.join("VsRenameDemo/src/main.cpp")).value_or(false));
        expectProjectsUntouched("move");
        EXPECT_TRUE(lastNotice().text.contains(QStringLiteral("moved"))) << lastNotice().text.toStdString();
        dir.write("VsRenameDemo/main.cpp", "int main() { return 0; }\n");
        ASSERT_TRUE(rig.workbench->deleteItem(mainCpp).has_value());
        EXPECT_FALSE(frost::io::realFileSystem().exists(mainCpp).value_or(true));
        expectProjectsUntouched("delete");
        EXPECT_TRUE(lastNotice().text.contains(QStringLiteral("deleted"))) << lastNotice().text.toStdString();
    }

    // 3. C# .slnx/.csproj project: rename Program.cs -> Startup.cs updates SolutionModel hierarchy.
    {
        auto created = rig.workbench->createNewProject({
            .name = "CsRenameDemo",
            .location = dir.path(),
            .kind = frost::app::NewProjectTemplate::DotNetConsole,
        });
        ASSERT_TRUE(created.has_value()) << created.error().describe();
        const std::string programCs = dir.join("CsRenameDemo/Program.cs");
        const std::string startupCs = dir.join("CsRenameDemo/Startup.cs");
        const std::string csprojBefore = dir.read("CsRenameDemo/CsRenameDemo.csproj");
        const auto noticesBefore = rig.window->notifications().log().size();
        auto renamed = rig.workbench->renameItem(programCs, "Startup.cs");
        ASSERT_TRUE(renamed.has_value()) << renamed.error().describe();
        EXPECT_EQ(*renamed, startupCs);
        // An SDK-style project includes its folder implicitly: nothing to edit, nothing to report.
        EXPECT_EQ(dir.read("CsRenameDemo/CsRenameDemo.csproj"), csprojBefore);
        for (auto i = noticesBefore; i < rig.window->notifications().log().size(); ++i)
            EXPECT_NE(rig.window->notifications().log()[i].notification.title,
                      QStringLiteral("Project files not updated"));
        ASSERT_NE(rig.workbench->solution(), nullptr);
        EXPECT_FALSE(rig.workbench->solution()->findFileNode(programCs).has_value());
        EXPECT_TRUE(rig.workbench->solution()->findFileNode(startupCs).has_value());
    }
}

TEST(Workbench, WelcomeScreenTwoPaneLayoutAndLiveProjectSearch) {
    WorkDir dir("welcome-two-pane");
    WorkbenchRig rig(dir);
    rig.show();

    auto* welcome = rig.workbench->welcomeScreen();
    ASSERT_NE(welcome, nullptr);
    ASSERT_NE(welcome->navigationList(), nullptr);
    EXPECT_EQ(welcome->navigationList()->count(), 3);
    ASSERT_NE(welcome->searchField(), nullptr);
    ASSERT_NE(welcome->cloneRepositoryButton(), nullptr);
    ASSERT_NE(welcome->checkForUpdatesButton(), nullptr);
    ASSERT_NE(welcome->updateStatusLabel(), nullptr);
    EXPECT_FALSE(welcome->updateStatus().isEmpty());
    EXPECT_TRUE(welcome->updateStatus().contains(QString::fromUtf8(frost::build_info::kVersion)))
        << welcome->updateStatus().toStdString();
    auto* topVersionLabel = welcome->findChild<QLabel*>(QStringLiteral("welcomeVersion"));
    ASSERT_NE(topVersionLabel, nullptr);
    EXPECT_EQ(topVersionLabel->text(), QString::fromUtf8(frost::build_info::kMajorVersion));
    welcome->setUpdateStatus(QStringLiteral("FrostIDE 2026.11 • Update available"));
    EXPECT_EQ(welcome->updateStatus(), QStringLiteral("FrostIDE 2026.11 • Update available"));

    welcome->setRecentProjects({dir.join("AlphaEngine"), dir.join("BetaGame"), dir.join("GammaTools")});
    ASSERT_EQ(welcome->recentList()->count(), 3);
    EXPECT_FALSE(welcome->recentList()->item(0)->isHidden());
    EXPECT_FALSE(welcome->recentList()->item(1)->isHidden());
    EXPECT_FALSE(welcome->recentList()->item(2)->isHidden());

    welcome->searchField()->setText(QStringLiteral("Beta"));
    EXPECT_TRUE(welcome->recentList()->item(0)->isHidden());
    EXPECT_FALSE(welcome->recentList()->item(1)->isHidden());
    EXPECT_TRUE(welcome->recentList()->item(2)->isHidden());

    welcome->searchField()->clear();
    EXPECT_FALSE(welcome->recentList()->item(0)->isHidden());
    EXPECT_FALSE(welcome->recentList()->item(1)->isHidden());
    EXPECT_FALSE(welcome->recentList()->item(2)->isHidden());

    // Standalone LauncherWindow closes when a project opens and reopens when the project closes.
    auto* launcher = rig.workbench->launcherWindow();
    ASSERT_NE(launcher, nullptr);
    ASSERT_NE(launcher->welcomeScreen(), nullptr);
    rig.workbench->showLauncherWindow();
    EXPECT_TRUE(launcher->isVisible());
    EXPECT_FALSE(rig.window->isVisible());

    const std::string projDir = dir.join("LauncherSample");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(projDir).has_value());
    ASSERT_TRUE(rig.workbench->openPath(projDir).has_value());
    EXPECT_FALSE(launcher->isVisible());
    EXPECT_TRUE(rig.window->isVisible());

    ASSERT_TRUE(rig.workbench->closeProject());
    EXPECT_TRUE(launcher->isVisible());
    EXPECT_FALSE(rig.window->isVisible());
}

TEST(Workbench, VcxprojCsprojNuGetReferencesAndSolutionXmlRoundTripAndDialogs) {
    WorkDir dir("project-properties-roundtrip", "tiny_sln");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    ASSERT_NE(rig.workbench->solution(), nullptr);

    // 1. Vcxproj compiler, linker, directories, and build events round-trip for Debug|x64.
    const std::string appVcxprojPath = dir.join("App/App.vcxproj");
    const std::string initialXml = dir.read("App/App.vcxproj");
    auto parsed = frost::app::parseVcxprojPropertiesForConfig(initialXml, "Debug|x64");
    parsed["ClCompile.LanguageStandard"] = "stdcpp23";
    parsed["ClCompile.Optimization"] = "MaxSpeed";
    parsed["ClCompile.AdditionalIncludeDirectories"] = "include;../third_party/include";
    parsed["ClCompile.PreprocessorDefinitions"] = "_DEBUG;FROST_FEATURE=1;%(PreprocessorDefinitions)";
    parsed["Link.AdditionalDependencies"] = "user32.lib;gdi32.lib;opengl32.lib";
    parsed["Link.AdditionalLibraryDirectories"] = "../third_party/lib/x64";
    parsed["Link.SubSystem"] = "Windows";
    parsed["PostBuildEvent.Command"] = "echo PostBuildDone";

    const std::string updatedXml = frost::app::applyVcxprojPropertiesForConfig(initialXml, "Debug|x64", parsed);
    auto reparsed = frost::app::parseVcxprojPropertiesForConfig(updatedXml, "Debug|x64");
    EXPECT_EQ(reparsed["ClCompile.LanguageStandard"], "stdcpp23");
    EXPECT_EQ(reparsed["ClCompile.Optimization"], "MaxSpeed");
    EXPECT_EQ(reparsed["ClCompile.AdditionalIncludeDirectories"], "include;../third_party/include");
    EXPECT_EQ(reparsed["ClCompile.PreprocessorDefinitions"], "_DEBUG;FROST_FEATURE=1;%(PreprocessorDefinitions)");
    EXPECT_EQ(reparsed["Link.AdditionalDependencies"], "user32.lib;gdi32.lib;opengl32.lib");
    EXPECT_EQ(reparsed["Link.AdditionalLibraryDirectories"], "../third_party/lib/x64");
    EXPECT_EQ(reparsed["Link.SubSystem"], "Windows");
    EXPECT_EQ(reparsed["PostBuildEvent.Command"], "echo PostBuildDone");

    // 2. Project properties dialog opens, modifies linker AdditionalDependencies, and saves to disk.
    bool savedCalled = false;
    QTimer::singleShot(0, rig.window.get(), [&] {
        auto* dlg = rig.window->findChild<QDialog*>(QStringLiteral("projectPropertiesDialog"));
        ASSERT_NE(dlg, nullptr);
        auto* depsEdit = dlg->findChild<QLineEdit*>(QStringLiteral("prop_Link.AdditionalDependencies"));
        EXPECT_NE(depsEdit, nullptr);
        if (depsEdit != nullptr)
            depsEdit->setText(QStringLiteral("ws2_32.lib;advapi32.lib"));
        // OK saves and then closes (QDialog::accept alone would skip the save).
        auto* buttons = dlg->findChild<QDialogButtonBox*>();
        QPushButton* ok = buttons != nullptr ? buttons->button(QDialogButtonBox::Ok) : nullptr;
        EXPECT_NE(ok, nullptr);
        if (ok != nullptr)
            ok->click();
        if (dlg->isVisible())
            dlg->reject();
    });
    frost::app::showProjectPropertiesDialog(rig.window.get(), frost::io::realFileSystem(), appVcxprojPath, "Debug|x64",
                                            [&] { savedCalled = true; });
    EXPECT_TRUE(savedCalled);
    auto onDiskAfterDlg = frost::app::parseVcxprojPropertiesForConfig(dir.read("App/App.vcxproj"), "Debug|x64");
    EXPECT_EQ(onDiskAfterDlg["Link.AdditionalDependencies"], "ws2_32.lib;advapi32.lib");

    // 3. C# .csproj properties & NuGet PackageReference round-trip.
    const std::string sampleCsproj = "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                                     "  <PropertyGroup>\n"
                                     "    <OutputType>Exe</OutputType>\n"
                                     "    <TargetFramework>net10.0</TargetFramework>\n"
                                     "  </PropertyGroup>\n"
                                     "</Project>\n";
    auto csProps = frost::app::parseCsprojProperties(sampleCsproj);
    EXPECT_EQ(csProps["Prop.OutputType"], "Exe");
    EXPECT_EQ(csProps["Prop.TargetFramework"], "net10.0");
    csProps["Prop.AllowUnsafeBlocks"] = "true";
    csProps["Prop.Nullable"] = "enable";
    csProps["Prop.PackageId"] = "Frost.Sample";
    csProps["Prop.Version"] = "2.1.0";
    const std::string updatedCsproj = frost::app::applyCsprojProperties(sampleCsproj, csProps);
    auto reparsedCs = frost::app::parseCsprojProperties(updatedCsproj);
    EXPECT_EQ(reparsedCs["Prop.AllowUnsafeBlocks"], "true");
    EXPECT_EQ(reparsedCs["Prop.Nullable"], "enable");
    EXPECT_EQ(reparsedCs["Prop.PackageId"], "Frost.Sample");
    EXPECT_EQ(reparsedCs["Prop.Version"], "2.1.0");

    // NuGet (read-only until LTR-7): exactly the PackageReference items the project file lists.
    EXPECT_TRUE(frost::app::parseCsprojNuGetPackages(updatedCsproj).empty());
    const std::string packagesCsproj = "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                                       "  <ItemGroup>\n"
                                       "    <PackageReference Include=\"Serilog\" Version=\"4.0.1\" />\n"
                                       "    <PackageReference Update=\"Dapper\"><Version>2.1.35</Version>"
                                       "</PackageReference>\n"
                                       "    <PackageReference Include=\"Central.Managed\" />\n"
                                       "  </ItemGroup>\n"
                                       "</Project>\n";
    const auto pkgs = frost::app::parseCsprojNuGetPackages(packagesCsproj);
    ASSERT_EQ(pkgs.size(), 3U);
    EXPECT_EQ(pkgs[0].id, "Serilog");
    EXPECT_EQ(pkgs[0].version, "4.0.1");
    EXPECT_EQ(pkgs[1].id, "Dapper");
    EXPECT_EQ(pkgs[1].version, "2.1.35");
    EXPECT_EQ(pkgs[2].id, "Central.Managed");
    EXPECT_EQ(pkgs[2].version, "");

    // 4. Adding and removing a project in the solution is refused (SLN-6) and leaves Tiny.sln as it was.
    dir.write("Extra/Extra.vcxproj", initialXml);
    const std::string slnBefore = dir.read("Tiny.sln");
    const auto addedProject = frost::app::addExistingProjectToSolutionFile(
        frost::io::realFileSystem(), dir.join("Tiny.sln"), dir.join("Extra/Extra.vcxproj"));
    ASSERT_FALSE(addedProject.has_value());
    EXPECT_EQ(addedProject.error().code(), frost::ErrorCode::Unsupported);
    EXPECT_TRUE(addedProject.error().message().contains("SLN-6")) << addedProject.error().message();
    const auto removedProject = frost::app::removeProjectFromSolutionFile(
        frost::io::realFileSystem(), dir.join("Tiny.sln"), dir.join("App/App.vcxproj"));
    ASSERT_FALSE(removedProject.has_value());
    EXPECT_EQ(removedProject.error().code(), frost::ErrorCode::Unsupported);
    EXPECT_EQ(dir.read("Tiny.sln"), slnBefore);

    // 5. Attach to Process enumeration & dialog and Compound Run Configuration launch.
    EXPECT_TRUE(rig.workbench->registry().find("debug.attachToProcess").has_value());
    EXPECT_TRUE(rig.workbench->registry().find("run.runCompound").has_value());
    const auto procs = frost::app::enumerateRunningProcesses();
    EXPECT_FALSE(procs.empty());

    std::uint32_t attachedPid = 0;
    QTimer::singleShot(0, rig.window.get(), [&] {
        auto* dlg = rig.window->findChild<QDialog*>(QStringLiteral("attachToProcessDialog"));
        ASSERT_NE(dlg, nullptr);
        auto* filter = dlg->findChild<QLineEdit*>(QStringLiteral("attachProcessFilterEdit"));
        EXPECT_NE(filter, nullptr);
        auto* table = dlg->findChild<QTableWidget*>(QStringLiteral("attachProcessTable"));
        ASSERT_NE(table, nullptr);
        EXPECT_GT(table->rowCount(), 0);
        dlg->accept();
    });
    frost::app::showAttachToProcessDialog(
        rig.window.get(), [&](const frost::app::RunningProcessInfo& proc, bool) { attachedPid = proc.pid; });
    EXPECT_GT(attachedPid, 0U);

    // 6. Unit Testing Tool Window (Alt+8 / "unit_tests") & Editor Gutter Test Runner.
    EXPECT_TRUE(rig.workbench->registry().find("window.unitTests").has_value());
    dir.write("App/math_tests.cpp", "TEST(MathSuite, AddsNumbers) {\n"
                                    "    EXPECT_EQ(2 + 2, 4);\n"
                                    "}\n"
                                    "TEST_F(MathFixture, FailsIntentionally) {\n"
                                    "    EXPECT_TRUE(false);\n"
                                    "}\n"
                                    "int main(int argc, char** argv) {\n"
                                    "    return 0;\n"
                                    "}\n");
    dir.write("App/ManagedTests.cs", "public class CalculatorTests {\n"
                                     "    [Fact]\n"
                                     "    public void MultipliesCorrectly() {\n"
                                     "    }\n"
                                     "    public static void Main(string[] args) {}\n"
                                     "}\n");

    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/math_tests.cpp")).has_value());
    ASSERT_NE(rig.view(), nullptr);
    const auto& markers = rig.view()->editor().lineMarkers();
    int runTestMarkerCount = 0;
    for (const auto& m : markers) {
        if (m.severity == frost::ui::MarkerSeverity::RunTest)
            ++runTestMarkerCount;
    }
    // Only main() gets a run marker: per-test gutter markers stay hidden until RUN-10 runs one test.
    EXPECT_EQ(runTestMarkerCount, 1);
    // App.vcxproj does not list math_tests.cpp, so the marker says no project builds it and a click
    // runs nothing (it would otherwise run the startup project, which never compiles this main).
    const auto runMarker =
        std::ranges::find_if(markers, [](const auto& m) { return m.severity == frost::ui::MarkerSeverity::RunTest; });
    ASSERT_NE(runMarker, markers.end());
    EXPECT_EQ(runMarker->line.value(), 6U);
    EXPECT_TRUE(runMarker->message.contains("No loaded C++ project")) << runMarker->message;
    const auto noticesBeforeClick = rig.window->notifications().log().size();
    emit rig.view()->editor().gutterRunMarkerClicked(6, QPoint());
    ASSERT_GT(rig.window->notifications().log().size(), noticesBeforeClick);
    EXPECT_EQ(rig.window->notifications().log().back().notification.text.toStdString(), runMarker->message);
    EXPECT_FALSE(rig.workbench->runController()->running());
    EXPECT_FALSE(rig.workbench->runService()->lastOutcome().has_value());

    rig.window->toolWindows().show(frost::app::kUnitTestsToolWindow, false);
    QWidget* unitTestsWidget = rig.window->toolWindows().content(frost::app::kUnitTestsToolWindow);
    ASSERT_NE(unitTestsWidget, nullptr);
    auto* refreshBtn = unitTestsWidget->findChild<QPushButton*>(QStringLiteral("unitTestsRefreshButton"));
    ASSERT_NE(refreshBtn, nullptr);
    refreshBtn->click();
    auto* tree = unitTestsWidget->findChild<QTreeWidget*>(QStringLiteral("unitTestsTreeWidget"));
    ASSERT_NE(tree, nullptr);
    // Tests are ctest's (RUN-7): a Visual Studio solution has no CTest directory, so the window lists
    // nothing (the TEST() in math_tests.cpp is not a ctest test), says why and runs nothing.
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    auto* runAllBtn = unitTestsWidget->findChild<QPushButton*>(QStringLiteral("unitTestsRunAllButton"));
    ASSERT_NE(runAllBtn, nullptr);
    EXPECT_FALSE(runAllBtn->isEnabled());
    EXPECT_TRUE(runAllBtn->toolTip().contains(QStringLiteral("ctest"))) << runAllBtn->toolTip().toStdString();
    runAllBtn->click();
    auto* summaryLbl = unitTestsWidget->findChild<QLabel*>(QStringLiteral("unitTestsSummaryLabel"));
    ASSERT_NE(summaryLbl, nullptr);
    EXPECT_TRUE(summaryLbl->text().contains(QStringLiteral("solution"))) << summaryLbl->text().toStdString();
    EXPECT_FALSE(summaryLbl->text().contains(QStringLiteral("Passed")));
    EXPECT_FALSE(summaryLbl->text().contains(QStringLiteral("Failed")));
    EXPECT_FALSE(rig.workbench->runController()->running());

    // 7. Search Everywhere (Classes / Symbols / All) exercises Workbench::queryWorkspaceSymbols
    // asynchronously on the worker thread pool without use-after-free.
    auto* popup = rig.nav().showSearchEverywhere(QStringLiteral("Math"), frost::ui::SearchEverywhereCategory::Classes);
    ASSERT_NE(popup, nullptr);
    popup->setFilterText(QStringLiteral("MathSuite"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    popup->close();
}

/*
 * FND-3 slice A (RUN-0, SLN-1, LTR-1): the project dialogs and project-file writers show only what
 * real files say and refuse what has no safe writer yet. The tests run on copies of the tiny_sln
 * fixture and compare every project and solution file byte for byte before and after.
 */

// The real launcher, counting every start, so a test can prove that a view started nothing.
class CountingLauncher final : public frost::io::IProcessLauncher {
public:
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        ++launches;
        return frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        ++launches;
        return frost::io::realProcessLauncher().launchAsync(spec, context);
    }
    std::atomic<int> launches{0};
};

// The tiny_sln project and solution files under `dir` with their bytes (the rig's data/ excluded).
std::map<std::string, std::string> tinySolutionFiles(const WorkDir& dir) {
    std::map<std::string, std::string> files;
    auto& fs = frost::io::realFileSystem();
    for (const char* sub : {"", "App", "Lib"}) {
        const std::string folder = *sub ? dir.join(sub) : dir.path();
        auto entries = fs.listDirectory(folder);
        if (!entries) {
            ADD_FAILURE() << entries.error().describe();
            continue;
        }
        for (const auto& entry : *entries) {
            if (entry.type != frost::io::FileType::Regular)
                continue;
            const std::string path = frost::io::joinPath(folder, entry.name).value_or("");
            files[path] = fs.readFile(path).value_or("<unreadable>");
        }
    }
    return files;
}

// Inspects the modal dialog `name` once its event loop runs, then closes it if `inspect` did not.
void inspectModal(QWidget* parent, const QString& name, std::function<void(QDialog&)> inspect) {
    QTimer::singleShot(0, parent, [parent, name, inspect = std::move(inspect)] {
        auto* dlg = parent->findChild<QDialog*>(name);
        if (dlg == nullptr) {
            ADD_FAILURE() << "no dialog " << name.toStdString();
            if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                modal->reject();
            return;
        }
        inspect(*dlg);
        if (dlg->isVisible())
            dlg->reject();
    });
}

// Clicks the dialog's OK button, which runs its accept handler (QDialog::accept would skip it).
void clickOk(QDialog& dlg) {
    auto* buttons = dlg.findChild<QDialogButtonBox*>();
    QPushButton* ok = buttons != nullptr ? buttons->button(QDialogButtonBox::Ok) : nullptr;
    ASSERT_NE(ok, nullptr) << dlg.objectName().toStdString();
    ok->click();
}

// A FrostEngine asset's metadata sidecar (<asset>.fmeta) moves with it on rename and on move; when
// the target's sidecar name is taken, neither file moves.
TEST(Workbench, RenamingOrMovingAnAssetMovesItsFmetaSidecar) {
    WorkDir dir("fmeta-sidecar");
    dir.write("game/Game.frostproj", "{}");
    dir.write("game/Assets/Click.wav", "RIFF");
    dir.write("game/Assets/Click.wav.fmeta", "guid: 1\n");
    dir.write("game/Assets/Taken.wav.fmeta", "guid: 2\n");
    dir.write("game/Assets/Lone.png", "PNG");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("game")).has_value());
    auto& fs = frost::io::realFileSystem();
    const auto exists = [&](const char* rel) { return fs.exists(dir.join(rel)).value_or(false); };

    auto renamed = rig.workbench->renameItem(dir.join("game/Assets/Click.wav"), "Tap.wav");
    ASSERT_TRUE(renamed.has_value()) << renamed.error().describe();
    EXPECT_TRUE(exists("game/Assets/Tap.wav"));
    EXPECT_TRUE(exists("game/Assets/Tap.wav.fmeta"));
    EXPECT_FALSE(exists("game/Assets/Click.wav"));
    EXPECT_FALSE(exists("game/Assets/Click.wav.fmeta"));
    EXPECT_EQ(dir.read("game/Assets/Tap.wav.fmeta"), "guid: 1\n");

    // The target's sidecar name belongs to another asset: refused, both files stay.
    auto refused = rig.workbench->renameItem(dir.join("game/Assets/Tap.wav"), "Taken.wav");
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::AlreadyExists);
    EXPECT_TRUE(exists("game/Assets/Tap.wav"));
    EXPECT_TRUE(exists("game/Assets/Tap.wav.fmeta"));
    EXPECT_EQ(dir.read("game/Assets/Taken.wav.fmeta"), "guid: 2\n");

    // An asset without a sidecar renames alone.
    ASSERT_TRUE(rig.workbench->renameItem(dir.join("game/Assets/Lone.png"), "Alone.png").has_value());
    EXPECT_TRUE(exists("game/Assets/Alone.png"));
    EXPECT_FALSE(exists("game/Assets/Alone.png.fmeta"));

    // Moving to another folder carries the sidecar too.
    ASSERT_TRUE(fs.createDirectories(dir.join("game/Assets/Audio")).has_value());
    auto* fsView = rig.nav().fileSystemView();
    ASSERT_NE(fsView, nullptr);
    emit fsView->movePathsToDirectoryRequested({QString::fromStdString(dir.join("game/Assets/Tap.wav"))},
                                               QString::fromStdString(dir.join("game/Assets/Audio")));
    EXPECT_TRUE(exists("game/Assets/Audio/Tap.wav"));
    EXPECT_TRUE(exists("game/Assets/Audio/Tap.wav.fmeta"));
    EXPECT_FALSE(exists("game/Assets/Tap.wav"));
    EXPECT_FALSE(exists("game/Assets/Tap.wav.fmeta"));

    // A move whose sidecar target is taken is refused with a notice; nothing moves.
    dir.write("game/Assets/Audio/Boom.wav.fmeta", "guid: 3\n");
    dir.write("game/Assets/Boom.wav", "RIFF");
    dir.write("game/Assets/Boom.wav.fmeta", "guid: 4\n");
    const auto notices = rig.window->notifications().log().size();
    emit fsView->movePathsToDirectoryRequested({QString::fromStdString(dir.join("game/Assets/Boom.wav"))},
                                               QString::fromStdString(dir.join("game/Assets/Audio")));
    EXPECT_TRUE(exists("game/Assets/Boom.wav"));
    EXPECT_FALSE(exists("game/Assets/Audio/Boom.wav"));
    EXPECT_EQ(dir.read("game/Assets/Audio/Boom.wav.fmeta"), "guid: 3\n");
    EXPECT_EQ(dir.read("game/Assets/Boom.wav.fmeta"), "guid: 4\n");
    ASSERT_GT(rig.window->notifications().log().size(), notices);
    EXPECT_TRUE(rig.window->notifications().log().back().notification.title.contains(QStringLiteral("Boom.wav")));
}

// readProcProcesses over a synthetic /proc: kernel threads and this process are skipped, the
// executable's name wins over the 15-byte comm, a " (deleted)" link is cleaned, and a dotted
// .NET apphost (no .exe suffix) is recognised by its runtimeconfig.json.
TEST(Workbench, ProcParsingNamesProcessesFromTheExecutableAndSkipsKernelThreads) {
    WorkDir dir("synthetic-proc");
    const auto argv = [](std::initializer_list<const char*> args) {
        std::string out;
        for (const char* arg : args) {
            out += arg;
            out.push_back('\0');
        }
        return out;
    };
    const std::string bin = dir.join("bin");
    dir.write("bin/My.App", "apphost");
    dir.write("bin/My.App.runtimeconfig.json", "{}");
    // 2: a kernel thread (comm only). 10: an executable longer than comm's 15 bytes. 11: a deleted
    // executable. 12: another user's process (no exe link) with a truncated comm. 13: a short comm
    // without an exe link. 14: the dotted apphost. 40: this process. 0: never a process.
    dir.write("proc/2/comm", "kthreadd\n");
    dir.write("proc/2/cmdline", "");
    dir.write("proc/10/comm", "frost-language-\n");
    dir.write("proc/10/cmdline", argv({"/usr/bin/frost-language-server", "--stdio"}));
    dir.write("proc/11/comm", "old-tool\n");
    dir.write("proc/11/cmdline", argv({"/opt/old-tool"}));
    dir.write("proc/12/comm", "someone-elses-l\n");
    dir.write("proc/12/cmdline", argv({"/usr/lib/someone-elses-long-daemon", "-d"}));
    dir.write("proc/13/comm", "bash\n");
    dir.write("proc/13/cmdline", argv({"-bash"}));
    dir.write("proc/14/comm", "My.App\n");
    dir.write("proc/14/cmdline", argv({"./My.App"}));
    dir.write("proc/40/comm", "frost_app_tests\n");
    dir.write("proc/40/cmdline", argv({"frost_app_tests"}));
    dir.write("proc/0/comm", "zero\n");
    dir.write("proc/0/cmdline", argv({"zero"}));
    dir.write("proc/self/comm", "self\n");
    dir.write("proc/version", "Linux\n");

    const std::map<std::string, std::string> links{
        {"10", "/usr/bin/frost-language-server"},
        {"11", "/opt/old-tool (deleted)"},
        {"14", bin + "/My.App"},
        {"40", "/usr/bin/frost_app_tests"},
        {"0", "/usr/bin/zero"},
    };
    std::vector<std::string> asked;
    const auto readLink = [&](const std::string& linkPath) -> std::string {
        asked.push_back(linkPath);
        const QFileInfo link(QString::fromStdString(linkPath));
        EXPECT_EQ(link.fileName(), QStringLiteral("exe"));
        const auto it = links.find(QFileInfo(link.path()).fileName().toStdString());
        return it == links.end() ? std::string() : it->second;
    };
    auto processes = frost::app::readProcProcesses(dir.join("proc"), 40, readLink);
    std::ranges::sort(processes, {}, &frost::app::RunningProcessInfo::pid);
    std::vector<std::uint32_t> pids;
    for (const auto& process : processes)
        pids.push_back(process.pid);
    EXPECT_EQ(pids, (std::vector<std::uint32_t>{10, 11, 12, 13, 14}));
    ASSERT_EQ(processes.size(), 5U);
    EXPECT_EQ(processes[0].name, "frost-language-server");
    EXPECT_EQ(processes[0].executablePath, "/usr/bin/frost-language-server");
    EXPECT_EQ(processes[1].name, "old-tool");
    EXPECT_EQ(processes[1].executablePath, "/opt/old-tool");
    EXPECT_EQ(processes[2].name, "someone-elses-long-daemon");
    EXPECT_TRUE(processes[2].executablePath.empty());
    EXPECT_EQ(processes[3].name, "bash");
    EXPECT_EQ(processes[4].name, "My.App");
    EXPECT_TRUE(processes[4].isDotNet);
    for (std::size_t i = 0; i < 4; ++i)
        EXPECT_FALSE(processes[i].isDotNet) << processes[i].name;
    EXPECT_FALSE(asked.empty());

    // A missing root yields nothing rather than failing.
    EXPECT_TRUE(frost::app::readProcProcesses(dir.join("no-proc"), 0, readLink).empty());
}

// SLN-11: the Debugging page shows the per-user LocalDebugger* settings as MSBuild sees them (the
// values of App.vcxproj.user) and saves edits to App.vcxproj.user only, in Visual Studio's format:
// the project file stays byte for byte, the rest of the .user file too, and other configurations'
// settings are untouched. (Replaces the read-only page of RUN-1, which this epic makes editable.)
TEST(Workbench, ProjectPropertiesDebuggingPageSavesToTheUserFileOnly) {
    WorkDir dir("project-properties-debugging", "tiny_sln");
    WorkbenchRig rig(dir);
    rig.show();
    const std::string vcxproj = dir.read("App/App.vcxproj");
    const std::string user = dir.read("App/App.vcxproj.user");
    ASSERT_TRUE(user.contains("LocalDebuggerCommandArguments"));
    ASSERT_FALSE(vcxproj.contains("LocalDebugger"));
    bool inspected = false;
    bool savedCalled = false;
    QTimer::singleShot(0, rig.window.get(), [&] {
        auto* dlg = rig.window->findChild<QDialog*>(QStringLiteral("projectPropertiesDialog"));
        ASSERT_NE(dlg, nullptr);
        const auto closeOnExit = qScopeGuard([dlg] {
            if (dlg->isVisible())
                dlg->reject();
        });
        auto* args = dlg->findChild<QLineEdit*>(QStringLiteral("prop_Prop.LocalDebuggerCommandArguments"));
        auto* cwd = dlg->findChild<QLineEdit*>(QStringLiteral("prop_Prop.LocalDebuggerWorkingDirectory"));
        auto* command = dlg->findChild<QLineEdit*>(QStringLiteral("prop_Prop.LocalDebuggerCommand"));
        auto* environment = dlg->findChild<QPlainTextEdit*>(QStringLiteral("prop_Prop.LocalDebuggerEnvironment"));
        ASSERT_NE(args, nullptr);
        ASSERT_NE(cwd, nullptr);
        ASSERT_NE(command, nullptr);
        ASSERT_NE(environment, nullptr);
        EXPECT_EQ(args->text(), QStringLiteral("--print \"two words\" --echo --exit 3"));
        EXPECT_EQ(cwd->text(), QStringLiteral("$(SolutionDir)Lib"));
        EXPECT_EQ(environment->toPlainText(),
                  QStringLiteral("FROST_TINY_VALUE=hello from $(Configuration)\nFROST_TINY_OTHER=1"));
        for (QLineEdit* edit : {args, cwd, command})
            EXPECT_FALSE(edit->isReadOnly()) << edit->objectName().toStdString();
        EXPECT_FALSE(environment->isReadOnly());
        bool noteFound = false;
        for (auto* note : dlg->findChildren<QLabel*>(QStringLiteral("projectPropertiesNote")))
            noteFound = noteFound || note->text().contains(QStringLiteral(".vcxproj.user"));
        EXPECT_TRUE(noteFound);
        EXPECT_TRUE(dlg->findChildren<QLabel*>(QStringLiteral("projectPropertiesReadOnlyNote")).isEmpty() ||
                    std::ranges::none_of(dlg->findChildren<QLabel*>(QStringLiteral("projectPropertiesReadOnlyNote")),
                                         [](QLabel* note) { return note->text().contains(QStringLiteral("RUN-1")); }));
        args->setText(QStringLiteral("--print <changed> & more"));
        cwd->setText(QString());
        environment->setPlainText(QStringLiteral("FROST_TINY_VALUE=edited\nFROST_TINY_OTHER=2"));
        auto* buttons = dlg->findChild<QDialogButtonBox*>();
        QPushButton* ok = buttons != nullptr ? buttons->button(QDialogButtonBox::Ok) : nullptr;
        ASSERT_NE(ok, nullptr);
        ok->click();
        inspected = true;
    });
    frost::app::showProjectPropertiesDialog(rig.window.get(), frost::io::realFileSystem(), dir.join("App/App.vcxproj"),
                                            "Debug|x64", [&] { savedCalled = true; });
    EXPECT_TRUE(inspected);
    EXPECT_TRUE(savedCalled);
    EXPECT_EQ(dir.read("App/App.vcxproj"), vcxproj) << "debugger settings never go into the shared project file";
    // The writer's own output for the same edit (the writer is checked against MSBuild in
    // build_msbuild_tests): the Debug|x64 group changes, everything else of the file stays.
    auto expected = frost::build::updateUserFileProperties(
        user, "Debug", "x64",
        {{"LocalDebuggerCommandArguments", "--print <changed> & more"},
         {"LocalDebuggerWorkingDirectory", ""},
         {"LocalDebuggerEnvironment", "FROST_TINY_VALUE=edited\nFROST_TINY_OTHER=2"}});
    ASSERT_TRUE(expected.has_value()) << expected.error().describe();
    const std::string written = dir.read("App/App.vcxproj.user");
    EXPECT_EQ(written, *expected);
    EXPECT_TRUE(written.contains("<LocalDebuggerCommandArguments>--print &lt;changed&gt; &amp; more"
                                 "</LocalDebuggerCommandArguments>"))
        << written;
    EXPECT_FALSE(written.contains("LocalDebuggerWorkingDirectory")) << "a cleared field removes the value";
    EXPECT_TRUE(written.contains("<!-- SPDX-License-Identifier: Apache-2.0 -->")) << written;
}

TEST(Workbench, ProjectMembershipEditsAreRefusedAndLeaveSolutionFilesUnchanged) {
    WorkDir dir("project-edit-refusals", "tiny_sln");
    dir.write("App/App.vcxproj.filters",
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
              "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
              "  <ItemGroup>\n    <Filter Include=\"Source Files\">\n"
              "      <UniqueIdentifier>{4FC737F1-C7A5-4376-A066-2A32D752A2FF}</UniqueIdentifier>\n"
              "    </Filter>\n  </ItemGroup>\n"
              "  <ItemGroup>\n    <ClCompile Include=\"main.cpp\">\n      <Filter>Source Files</Filter>\n"
              "    </ClCompile>\n  </ItemGroup>\n</Project>\n");
    dir.write("App/extra.cpp", "int extra;\n");
    const auto before = tinySolutionFiles(dir);
    ASSERT_GE(before.size(), 9U);

    auto& fs = frost::io::realFileSystem();
    const std::string app = dir.join("App/App.vcxproj");
    const std::vector<std::string> sources{dir.join("App/main.cpp"), dir.join("App/extra.cpp")};
    const std::vector<std::string> folder{dir.join("App")};
    const auto expectRefused = [](const auto& result, std::string_view what) {
        ASSERT_FALSE(result.has_value()) << what;
        EXPECT_EQ(result.error().code(), frost::ErrorCode::Unsupported) << what;
        EXPECT_TRUE(result.error().message().contains("SLN-6")) << what << ": " << result.error().message();
        EXPECT_TRUE(result.error().message().contains("No file was changed")) << what;
    };
    for (const char* solutionName : {"Tiny.sln", "Tiny.slnx"}) {
        const std::string sln = dir.join(solutionName);
        expectRefused(frost::app::addExistingProjectToSolutionFile(fs, sln, dir.join("Lib/Lib.vcxproj")),
                      "add existing project");
        expectRefused(frost::app::removeProjectFromSolutionFile(fs, sln, app), "remove project");
        expectRefused(frost::app::addSolutionFolderToSolutionFile(fs, sln, "", "Folder"), "add solution folder");
        expectRefused(frost::app::renameSolutionFolderInSolutionFile(fs, sln, "/Folder/", "Renamed"),
                      "rename solution folder");
        expectRefused(frost::app::removeSolutionFolderFromSolutionFile(fs, sln, "/Folder/"), "remove solution folder");
        expectRefused(frost::app::moveProjectToSolutionFolderInSolutionFile(fs, sln, app, "/Folder/"),
                      "move project to solution folder");
    }
    expectRefused(frost::app::addFilterToVcxprojFilters(fs, app, "Source Files\\New"), "add filter");
    expectRefused(frost::app::renameFilterInVcxprojFilters(fs, app, "Source Files", "Sources"), "rename filter");
    expectRefused(frost::app::removeFilterFromVcxprojFilters(fs, app, "Source Files"), "remove filter");
    expectRefused(frost::app::moveFilesToVcxprojFilter(fs, app, sources, "Header Files"), "move to filter");
    expectRefused(frost::app::addExistingFilesToVcxproj(fs, app, sources, {}), "add files");
    expectRefused(frost::app::addExistingFilesToVcxproj(fs, app, folder, {}), "add folder");
    expectRefused(frost::app::removeFilesFromVcxproj(fs, app, sources), "remove files");
    EXPECT_EQ(tinySolutionFiles(dir), before);
}

TEST(Workbench, SolutionExplorerProjectEditsNotifyRefusalAndChangeNothing) {
    WorkDir dir("explorer-edit-refusals", "tiny_sln");
    RigOptions options;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    auto* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    const auto before = tinySolutionFiles(dir);

    const QString app = QString::fromStdString(dir.join("App/App.vcxproj"));
    const QString mainCpp = QString::fromStdString(dir.join("App/main.cpp"));
    const auto expectRefusalNotice = [&](const std::function<void()>& request, const char* what) {
        const auto count = rig.window->notifications().log().size();
        request();
        ASSERT_GT(rig.window->notifications().log().size(), count) << what;
        const auto& last = rig.window->notifications().log().back().notification;
        EXPECT_EQ(last.kind, frost::ui::NotificationKind::Error) << what;
        EXPECT_TRUE(last.text.contains(QStringLiteral("SLN-6"))) << what << ": " << last.text.toStdString();
    };
    expectRefusalNotice(
        [&] { emit explorer->moveFilesToFilterRequested(app, {mainCpp}, QStringLiteral("Header Files")); },
        "move to filter");
    expectRefusalNotice([&] { emit explorer->removeMultipleProjectFilesRequested(app, {mainCpp}); },
                        "remove several files");
    expectRefusalNotice([&] { emit explorer->removeFilterRequested(app, QStringLiteral("Source Files")); },
                        "remove filter");
    expectRefusalNotice([&] { emit explorer->removeSolutionFolderRequested(QStringLiteral("/Folder/")); },
                        "remove solution folder");
    expectRefusalNotice([&] { emit explorer->moveProjectToSolutionFolderRequested(app, QStringLiteral("/Folder/")); },
                        "move project to solution folder");
    // These used to open a picker or a name prompt before refusing; now they refuse at once (no modal
    // dialog opens, or the emit would not return).
    expectRefusalNotice(
        [&] { emit explorer->addExistingProjectRequested(QString::fromStdString(dir.join("Tiny.sln"))); },
        "add existing project");
    expectRefusalNotice([&] { emit explorer->removeProjectFromSolutionRequested(app); }, "remove project");
    expectRefusalNotice([&] { emit explorer->addFilterRequested(app, QString()); }, "add filter");
    expectRefusalNotice([&] { emit explorer->renameFilterRequested(app, QStringLiteral("Source Files")); },
                        "rename filter");
    expectRefusalNotice([&] { emit explorer->addSolutionFolderRequested(QString()); }, "add solution folder");
    expectRefusalNotice(
        [&] { emit explorer->renameSolutionFolderRequested(QStringLiteral("/Folder/"), QStringLiteral("Folder")); },
        "rename solution folder");
    expectRefusalNotice([&] { emit explorer->moveProjectToNewSolutionFolderRequested(app); },
                        "move project to a new solution folder");
    expectRefusalNotice([&] { emit explorer->moveFilesToNewFilterRequested(app, {mainCpp}); },
                        "move files to a new filter");
    expectRefusalNotice([&] { emit explorer->addExistingFolderToFilterRequested(app, QString()); },
                        "add existing folder");
    EXPECT_EQ(QApplication::activeModalWidget(), nullptr);

    // The menus offer none of these: they are disabled up front, the tooltip naming SLN-6.
    {
        const QModelIndex appIndex =
            explorer->filterModel()->mapFromSource(explorer->model()->indexOfPath(dir.join("App/main.cpp")).parent());
        ASSERT_TRUE(appIndex.isValid());
        std::unique_ptr<QMenu> projectMenu(explorer->createContextMenu(appIndex));
        ASSERT_NE(projectMenu, nullptr);
        int disabled = 0;
        for (QAction* action : projectMenu->actions()) {
            const QString name = action->menu() ? action->menu()->objectName() : action->objectName();
            if (name == QStringLiteral("removeProjectFromSolution") || name == QStringLiteral("addNewFilter") ||
                name == QStringLiteral("moveToSolutionFolderMenu")) {
                EXPECT_FALSE(action->isEnabled()) << name.toStdString();
                EXPECT_TRUE(action->toolTip().contains(QStringLiteral("SLN-6"))) << name.toStdString();
                ++disabled;
            }
        }
        EXPECT_EQ(disabled, 3);
    }

    // Remove from Project goes to the ProjectHost helper. None is installed beside the test binary
    // and none is configured, so the request is refused with a notification (on Linux: Windows only).
    const QModelIndex mainIndex =
        explorer->filterModel()->mapFromSource(explorer->model()->indexOfPath(dir.join("App/main.cpp")));
    ASSERT_TRUE(mainIndex.isValid());
    std::unique_ptr<QMenu> menu(explorer->createContextMenu(mainIndex));
    ASSERT_NE(menu, nullptr);
    const auto actions = menu->actions();
    const auto remove = std::ranges::find_if(
        actions, [](QAction* action) { return action->objectName() == QStringLiteral("removeFromProject"); });
    ASSERT_NE(remove, actions.end());
    const auto count = rig.window->notifications().log().size();
    (*remove)->trigger();
    ASSERT_GT(rig.window->notifications().log().size(), count);
    EXPECT_EQ(rig.window->notifications().log().back().notification.kind, frost::ui::NotificationKind::Error);

    EXPECT_EQ(tinySolutionFiles(dir), before);
}

/*
 * A folder that is not a CMake folder lists no test and starts nothing (RUN-7: tests are ctest's).
 * It replaces UnitTestsWindowListsDiscoveredTestsAndStartsNothing (harm register, D32), which
 * asserted the R0 source-text scan's "Discovered, not run" rows; the window over the real ctest is
 * tests/app/unit_tests_window_test.cpp.
 */
TEST(Workbench, UnitTestsWindowWithoutACMakeFolderListsNothingAndStartsNothing) {
    WorkDir dir("unit-tests-no-cmake");
    dir.write("Tests/math_tests.cpp", "TEST(MathSuite, AddsNumbers) {\n"
                                      "    EXPECT_EQ(2 + 2, 4);\n"
                                      "}\n");
    CountingLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tests")).has_value());

    rig.window->toolWindows().show(frost::app::kUnitTestsToolWindow, false);
    QWidget* window = rig.window->toolWindows().content(frost::app::kUnitTestsToolWindow);
    ASSERT_NE(window, nullptr);
    const int launchesBefore = launcher.launches.load();
    auto* refresh = window->findChild<QPushButton*>(QStringLiteral("unitTestsRefreshButton"));
    ASSERT_NE(refresh, nullptr);
    refresh->click();
    auto* tree = window->findChild<QTreeWidget*>(QStringLiteral("unitTestsTreeWidget"));
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->columnCount(), 3); // Name, status, duration.
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    auto* summary = window->findChild<QLabel*>(QStringLiteral("unitTestsSummaryLabel"));
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("Open a CMake folder"))) << summary->text().toStdString();

    for (const char* name : {"unitTestsRunAllButton", "unitTestsRunSelectedButton", "unitTestsRunFailedButton",
                             "unitTestsDebugSelectedButton", "unitTestsStopButton"}) {
        auto* button = window->findChild<QPushButton*>(QLatin1String(name));
        ASSERT_NE(button, nullptr) << name;
        EXPECT_FALSE(button->isEnabled()) << name;
        button->click();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_EQ(launcher.launches.load(), launchesBefore);
    auto* details = window->findChild<QPlainTextEdit*>(QStringLiteral("unitTestsOutputView"));
    ASSERT_NE(details, nullptr);
    EXPECT_FALSE(details->toPlainText().contains(QStringLiteral("[       OK ]")));
    EXPECT_FALSE(details->toPlainText().contains(QStringLiteral("Passed")));
}

TEST(Workbench, ProjectDialogsAreReadOnlyAndWriteNoProjectFile) {
    WorkDir dir("project-dialogs-read-only", "tiny_sln");
    CountingLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    const auto solution = rig.workbench->solution();
    ASSERT_NE(solution, nullptr);
    dir.write("App/Managed.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\">\n  <ItemGroup>\n"
                                    "    <PackageReference Include=\"Serilog\" Version=\"4.0.1\" />\n"
                                    "    <PackageReference Include=\"Central.Managed\" />\n"
                                    "  </ItemGroup>\n</Project>\n");
    const auto before = tinySolutionFiles(dir);
    auto& fs = frost::io::realFileSystem();
    const std::string app = dir.join("App/App.vcxproj");
    bool written = false;
    const auto onWrite = [&] { written = true; };

    /* Configuration Manager (editable since SLN-7; its writes are tested in
     * solution_configuration_edit_test.cpp and workbench_configuration_manager_test.cpp): the
     * mappings as loaded, edited only through the row's editors, never typed into the cells; OK
     * without an edit applies the active configuration and writes nothing. */
    std::optional<int> activeChosen;
    inspectModal(rig.window.get(), QStringLiteral("configurationManagerDialog"), [](QDialog& dlg) {
        auto* table = dlg.findChild<QTableWidget*>(QStringLiteral("cfgManagerProjectTable"));
        ASSERT_NE(table, nullptr);
        EXPECT_EQ(table->rowCount(), 2);
        for (int row = 0; row < table->rowCount(); ++row) {
            for (int column = 0; column < table->columnCount(); ++column) {
                ASSERT_NE(table->item(row, column), nullptr);
                EXPECT_FALSE(table->item(row, column)->flags().testFlag(Qt::ItemIsEditable));
            }
            EXPECT_EQ(table->item(row, 1)->text(), QStringLiteral("Debug"));
            EXPECT_EQ(table->item(row, 2)->text(), QStringLiteral("x64"));
        }
        EXPECT_EQ(dlg.findChild<QLabel*>(QStringLiteral("cfgManagerReadOnlyNote")), nullptr);
        clickOk(dlg);
    });
    frost::app::showConfigurationManagerDialog(
        rig.window.get(), fs, *solution, 0, [&](int index) { activeChosen = index; }, onWrite);
    EXPECT_EQ(activeChosen, 0);

    // Add Project Reference and Solution Properties: references as loaded, not editable.
    const auto expectReadOnlyList = [](QDialog& dlg, const char* listName, const char* noteName) {
        auto* list = dlg.findChild<QListWidget*>(QLatin1String(listName));
        ASSERT_NE(list, nullptr);
        EXPECT_GT(list->count(), 0);
        for (int row = 0; row < list->count(); ++row)
            EXPECT_FALSE(list->item(row)->flags().testFlag(Qt::ItemIsEnabled));
        auto* note = dlg.findChild<QLabel*>(QLatin1String(noteName));
        ASSERT_NE(note, nullptr);
        EXPECT_TRUE(note->text().contains(QStringLiteral("SLN-6")));
    };
    inspectModal(rig.window.get(), QStringLiteral("addProjectReferenceDialog"), [&](QDialog& dlg) {
        expectReadOnlyList(dlg, "projectReferenceList", "projectReferenceReadOnlyNote");
    });
    frost::app::showAddProjectReferenceDialog(rig.window.get(), fs, *solution, app, onWrite);
    inspectModal(rig.window.get(), QStringLiteral("solutionPropertiesDialog"), [&](QDialog& dlg) {
        expectReadOnlyList(dlg, "solutionDependenciesList", "solutionDependenciesReadOnlyNote");
        clickOk(dlg);
    });
    bool startupChosen = false;
    frost::app::showSolutionPropertiesDialog(
        rig.window.get(), fs, *solution, std::nullopt, [&](frost::project::ProjectId) { startupChosen = true; },
        onWrite);
    EXPECT_TRUE(startupChosen);

    // NuGet: only the project file's PackageReference items; no catalogue, browse or install.
    {
        std::unique_ptr<QWidget> nuget(
            frost::app::createNuGetToolWindowWidget(nullptr, fs, {}, dir.join("App/Managed.csproj"), onWrite));
        auto* table = nuget->findChild<QTableWidget*>(QStringLiteral("nugetPackagesTable"));
        ASSERT_NE(table, nullptr);
        ASSERT_EQ(table->rowCount(), 2);
        EXPECT_EQ(table->item(0, 0)->text(), QStringLiteral("Serilog"));
        EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("4.0.1"));
        EXPECT_EQ(table->item(1, 0)->text(), QStringLiteral("Central.Managed"));
        EXPECT_TRUE(table->item(1, 1)->text().contains(QStringLiteral("not set")));
        for (const char* gone : {"nugetBrowseTable", "nugetUpdatesTable", "nugetInstallButton", "nugetRemoveButton",
                                 "nugetUpdateAllButton", "nugetFeedCombo"})
            EXPECT_EQ(nuget->findChild<QWidget*>(QLatin1String(gone)), nullptr) << gone;
        auto* note = nuget->findChild<QLabel*>(QStringLiteral("nugetReadOnlyNote"));
        ASSERT_NE(note, nullptr);
        EXPECT_TRUE(note->text().contains(QStringLiteral("LTR-7")));
    }

    // ASM / IL: disabled with its reasons; no compiler is started for the active file.
    {
        const int launchesBefore = launcher.launches.load();
        const std::string mainCpp = dir.join("App/main.cpp");
        std::unique_ptr<QWidget> asmView(frost::app::createAsmViewerToolWindowWidget(
            nullptr, [&]() -> std::pair<std::string, int> { return {mainCpp, 1}; }));
        auto* button = asmView->findChild<QPushButton*>(QStringLiteral("asmRefreshButton"));
        ASSERT_NE(button, nullptr);
        EXPECT_FALSE(button->isEnabled());
        button->click();
        auto* output = asmView->findChild<QPlainTextEdit*>(QStringLiteral("asmViewerOutput"));
        ASSERT_NE(output, nullptr);
        EXPECT_TRUE(output->toPlainText().contains(QStringLiteral("CPP-3")));
        EXPECT_TRUE(output->toPlainText().contains(QStringLiteral("LTR-3")));
        EXPECT_TRUE(output->toPlainText().contains(QString::fromStdString(mainCpp)));
        EXPECT_FALSE(output->toPlainText().contains(QStringLiteral(".assembly")));
        EXPECT_TRUE(asmView->findChildren<QProcess*>().isEmpty());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        EXPECT_EQ(launcher.launches.load(), launchesBefore);
    }

    EXPECT_FALSE(written);
    EXPECT_EQ(tinySolutionFiles(dir), before);
    EXPECT_FALSE(fs.exists(dir.join("App/App.vcxproj.filters")).value_or(true));
}

TEST(Workbench, OpeningUntrustedProjectOrFolderPromptsForTrustImmediately) {
    WorkDir dir("trust-on-open", "tiny_sln");
    dir.write("untrusted_folder/script.lua", "return 1\n");
    WorkbenchRig rig(dir);
    rig.show();

    const std::string folderPath = dir.join("untrusted_folder");
    ASSERT_FALSE(rig.trust->isTrusted(folderPath));
    ASSERT_TRUE(rig.workbench->openPath(folderPath).has_value());
    EXPECT_FALSE(rig.window->projectTrusted());

    auto* folderDialog = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(folderDialog, nullptr);
    EXPECT_TRUE(folderDialog->isVisible());
    folderDialog->trustButton()->click();
    EXPECT_TRUE(rig.trust->isTrusted(folderPath));
    EXPECT_TRUE(rig.window->projectTrusted());

    ASSERT_FALSE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")).has_value());
    EXPECT_FALSE(rig.window->projectTrusted());

    auto* slnDialog = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(slnDialog, nullptr);
    EXPECT_TRUE(slnDialog->isVisible());
    slnDialog->trustButton()->click();
    EXPECT_TRUE(rig.trust->isTrusted(dir.path()));
    EXPECT_TRUE(rig.window->projectTrusted());
}

// --- Settings that are shown are applied: save actions, confirm exit, reopen last project ---

TEST(Workbench, SaveActionsFollowTheSettingsAndEditorConfigAndSpareTheCaretLine) {
    WorkDir dir("save-actions");
    dir.write("proj/code.txt", "first  \nsecond  \nthird\t");
    dir.write("proj/notes.md", "keep  \nme  ");
    dir.write("proj/.editorconfig",
              "root = true\n[*.md]\ntrim_trailing_whitespace = false\ninsert_final_newline = true\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application,
                                  frost::settings::key::editorTrimTrailingWhitespace, std::string("all")));
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::editorInsertFinalNewline, true));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("proj/code.txt")));
    ASSERT_NE(rig.view(), nullptr);
    ASSERT_TRUE(rig.view()->editor().typeText("#")); // The caret stays on line 1.
    ASSERT_TRUE(rig.workbench->save(*rig.view()));
    // Line 1 keeps its whitespace (the caret is there); the others are trimmed; a final newline is added.
    EXPECT_EQ(dir.read("proj/code.txt"), "#first  \nsecond\nthird\n");
    EXPECT_EQ(rig.view()->document().document().committedText().text(), "#first  \nsecond\nthird\n");
    EXPECT_FALSE(rig.view()->document().dirty());

    // .editorconfig turns trimming off for *.md and keeps the final newline.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("proj/notes.md")));
    ASSERT_TRUE(rig.workbench->currentView()->editor().typeText("!"));
    ASSERT_TRUE(rig.workbench->save(*rig.workbench->currentView()));
    EXPECT_EQ(dir.read("proj/notes.md"), "!keep  \nme  \n");

    // "none" and no final newline: the save writes the text as it is.
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application,
                                  frost::settings::key::editorTrimTrailingWhitespace, std::string("none")));
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::editorInsertFinalNewline, false));
    dir.write("proj/plain.txt", "a  \nb  ");
    ASSERT_TRUE(rig.workbench->openPath(dir.join("proj/plain.txt")));
    ASSERT_TRUE(rig.workbench->currentView()->editor().typeText("x"));
    ASSERT_TRUE(rig.workbench->save(*rig.workbench->currentView()));
    EXPECT_EQ(dir.read("proj/plain.txt"), "xa  \nb  ");
}

// Every caret's line is spared, not only the primary caret's: whitespace typed under a secondary
// caret survives the save as well.
TEST(Workbench, SaveActionsSpareTheLineOfEveryCaret) {
    WorkDir dir("save-actions-multi-caret");
    dir.write("proj/code.txt", "one  \ntwo  \nthree  \nfour  \n");
    dir.write("proj/.editorconfig", "root = true\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application,
                                  frost::settings::key::editorTrimTrailingWhitespace, std::string("all")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("proj/code.txt")));
    ASSERT_NE(rig.view(), nullptr);
    auto& editor = rig.view()->editor();
    // Carets at the start of lines 1 and 3; the primary is the first.
    const std::vector<frost::text::Selection> carets{frost::text::Selection::caret(frost::ByteOffset{0}),
                                                     frost::text::Selection::caret(frost::ByteOffset{12})};
    editor.setState(frost::text::EditorState::from(frost::text::SelectionSet::fromSelections(carets, 0)));
    ASSERT_EQ(editor.editor().state().selections.size(), 2U);
    ASSERT_TRUE(editor.typeText("#"));
    ASSERT_EQ(rig.view()->document().document().text().text(), "#one  \ntwo  \n#three  \nfour  \n");
    ASSERT_TRUE(rig.workbench->save(*rig.view()));
    EXPECT_EQ(dir.read("proj/code.txt"), "#one  \ntwo\n#three  \nfour\n");

    // With one caret left (line 3, after "#thr"), line 1 is trimmed on the next save.
    editor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{15}));
    ASSERT_TRUE(editor.typeText("!"));
    ASSERT_TRUE(rig.workbench->save(*rig.view()));
    EXPECT_EQ(dir.read("proj/code.txt"), "#one\ntwo\n#thr!ee  \nfour\n");
}

TEST(Workbench, SaveActionsReachTheFileWhenAutosavesRanFirstWithTheDefaultSettings) {
    WorkDir dir("save-actions-autosave");
    dir.write("proj/code.txt", "keep  \nb\nc\n");
    // Keeps the checkout's own .editorconfig (trim_trailing_whitespace = true) out of the test.
    dir.write("proj/.editorconfig", "root = true\n");
    RigOptions options;
    options.autosave = true; // As the defaults: autosave on idle and on frame deactivation.
    WorkbenchRig rig(dir, std::move(options));
    // The default save action: trim the modified lines, except a caret line.
    ASSERT_EQ(rig.settings->snapshot()->get(frost::settings::key::editorTrimTrailingWhitespace), "modified");
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::saveAutosaveIdleSeconds,
                                  std::int64_t{1}));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("proj/code.txt")));
    DocumentEditorView* view = rig.view();
    ASSERT_NE(view, nullptr);
    QWidget* editor = view->widget();
    editor->setFocus();
    QApplication::processEvents();
    const auto typeSpaces = [&] {
        sendKey(editor, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
        sendKey(editor, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    };
    view->editor().goToLine(3, 2);
    typeSpaces(); // "c  "
    view->editor().goToLine(2, 2);
    typeSpaces(); // "b  ", where the caret stays
    EXPECT_EQ(view->document().document().committedText().text(), "keep  \nb  \nc  \n");
    // The idle autosave applies the save actions and spares the caret line.
    EXPECT_TRUE(waitFor([&] { return dir.read("proj/code.txt") == "keep  \nb  \nc\n"; }, std::chrono::seconds(10)))
        << dir.read("proj/code.txt");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
    // The caret moves away and Ctrl+S (Save All) of the now clean document trims the line the
    // autosave spared; the unedited "keep  " stays.
    view->editor().goToLine(1, 1);
    sendKey(editor, Qt::Key_S, Qt::ControlModifier);
    EXPECT_EQ(dir.read("proj/code.txt"), "keep  \nb\nc\n");
    EXPECT_EQ(view->document().document().committedText().text(), "keep  \nb\nc\n");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));

    // Frame deactivation (Alt+Tab) applies them as well.
    view->editor().goToLine(3, 2);
    typeSpaces(); // "c  "
    view->editor().goToLine(1, 1);
    rig.workbench->frameDeactivated();
    EXPECT_EQ(dir.read("proj/code.txt"), "keep  \nb\nc\n");
    EXPECT_EQ(view->document().document().committedText().text(), "keep  \nb\nc\n");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
}

TEST(Workbench, ConfirmExitAsksBeforeTheIdeWindowClosesOnlyWhenEnabled) {
    WorkDir dir("confirm-exit");
    WorkbenchRig rig(dir);
    rig.show();
    // Off by default: closing does not ask.
    ASSERT_FALSE(rig.settings->snapshot()->get(frost::settings::key::generalConfirmExit));
    rig.window->close();
    EXPECT_EQ(rig.prompts.exitPrompts, 0);
    EXPECT_FALSE(rig.window->isVisible());
    rig.show();
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::generalConfirmExit, true));
    rig.prompts.exitAnswer = false;
    EXPECT_FALSE(rig.window->close()); // Cancel keeps the IDE open.
    EXPECT_EQ(rig.prompts.exitPrompts, 1);
    EXPECT_TRUE(rig.window->isVisible());
    rig.prompts.exitAnswer = true;
    EXPECT_TRUE(rig.window->close());
    EXPECT_EQ(rig.prompts.exitPrompts, 2);
    EXPECT_FALSE(rig.window->isVisible());
}

TEST(Workbench, StartupShowsTheLauncherUnlessReopenLastProjectIsOn) {
    /* The user's design: FrostIDE starts in the stripped-down launcher (open, recent and new project),
       even when a project was open last time. general.reopenLastProject is an opt-in, off by default
       (it once defaulted on, and then every start skipped the launcher). */
    WorkDir dir("reopen-off");
    dir.write("proj/main.cpp", "int main() { return 0; }\n");
    {
        WorkbenchRig first(dir);
        ASSERT_TRUE(first.workbench->openPath(dir.join("proj")));
    }
    {
        WorkbenchRig fresh(dir); // Default settings: the launcher, not the last project.
        EXPECT_FALSE(fresh.settings->snapshot()->get(frost::settings::key::generalReopenLastProject));
        EXPECT_FALSE(fresh.workbench->showStartupWindow());
        EXPECT_FALSE(fresh.workbench->hasProject());
        ASSERT_NE(fresh.workbench->launcherWindow(), nullptr);
        EXPECT_TRUE(fresh.workbench->launcherWindow()->isVisible());
        EXPECT_FALSE(fresh.window->isVisible());
    }
    {
        WorkbenchRig second(dir);
        ASSERT_TRUE(second.settings->set(frost::settings::Scope::Application,
                                         frost::settings::key::generalReopenLastProject, true)); // Opt-in.
        EXPECT_TRUE(second.workbench->showStartupWindow()); // On: startup reopens it in the IDE window.
        EXPECT_EQ(second.workbench->projectRoot(), dir.join("proj"));
        EXPECT_TRUE(second.window->isVisible());
        ASSERT_NE(second.workbench->launcherWindow(), nullptr);
        EXPECT_FALSE(second.workbench->launcherWindow()->isVisible());
    }
    WorkbenchRig third(dir);
    ASSERT_TRUE(third.settings->set(frost::settings::Scope::Application, frost::settings::key::generalReopenLastProject,
                                    false));
    EXPECT_FALSE(third.workbench->showStartupWindow());
    EXPECT_FALSE(third.workbench->hasProject());
    ASSERT_NE(third.workbench->launcherWindow(), nullptr);
    EXPECT_TRUE(third.workbench->launcherWindow()->isVisible());
    EXPECT_FALSE(third.window->isVisible());
}

// --- Attach to Process and compound runs never start anything silently ---

TEST(Workbench, AttachToProcessPromptsForTrustInsteadOfGrantingIt) {
    WorkDir dir("attach-trust");
    WorkbenchRig rig(dir);
    rig.show();
    const auto pickProcess = [&](std::uint32_t pid) {
        QTimer::singleShot(0, rig.window.get(), [&, pid] {
            auto* dialog = rig.window->findChild<QDialog*>(QStringLiteral("attachToProcessDialog"));
            ASSERT_NE(dialog, nullptr);
            auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("attachProcessTable"));
            ASSERT_NE(table, nullptr);
            dialog->findChild<QLineEdit*>(QStringLiteral("attachProcessFilterEdit"))->setText(QString::number(pid));
            for (int row = 0; row < table->rowCount(); ++row)
                if (table->item(row, 0)->text() == QString::number(pid))
                    table->setCurrentCell(row, 0);
            dialog->accept();
        });
        auto id = rig.workbench->registry().find("debug.attachToProcess");
        ASSERT_TRUE(id);
        ASSERT_EQ(rig.workbench->registry().tryInvoke(*id, frost::input::ActionContext{}),
                  frost::input::InvokeResult::Ran);
    };
    // A process whose image path the OS does not report (System, another user's) is refused, not guessed.
    const auto processes = frost::app::enumerateRunningProcesses();
    const auto pathless = std::ranges::find_if(processes, [](const auto& p) { return p.executablePath.empty(); });
    if (pathless != processes.end()) {
        pickProcess(pathless->pid);
        const auto& log = rig.window->notifications().log();
        ASSERT_FALSE(log.empty());
        EXPECT_TRUE(log.back().notification.text.contains(QStringLiteral("executable path is unavailable")))
            << log.back().notification.text.toStdString();
        EXPECT_EQ(rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog")), nullptr);
    }
    // A child in an untrusted system folder (the list leaves out this test process itself): the
    // normal trust prompt opens and nothing is trusted or attached. Killed when `child` goes.
    frost::io::ProcessSpec spec;
#ifdef _WIN32
    spec.executable =
        QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/PING.EXE")).toStdString();
    spec.arguments = {"-n", "60", "127.0.0.1"};
#else
    spec.executable = "/bin/sleep";
    spec.arguments = {"60"};
#endif
    spec.executable = QDir::toNativeSeparators(QString::fromStdString(spec.executable)).toStdString();
    spec.stdoutMode = frost::io::Stdio::Null;
    spec.stderrMode = frost::io::Stdio::Null;
    auto child = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(child) << child.error().describe();
    const auto killChild = qScopeGuard([&] { static_cast<void>((*child)->terminate()); });
    const std::string exeFolder = std::string(frost::io::parentPath(spec.executable));
    ASSERT_FALSE(rig.trust->isTrusted(exeFolder));
    ASSERT_TRUE(waitFor(
        [&] {
            return std::ranges::any_of(frost::app::enumerateRunningProcesses(),
                                       [&](const auto& p) { return p.pid == (*child)->pid(); });
        },
        std::chrono::seconds(5)));
    pickProcess(static_cast<std::uint32_t>((*child)->pid()));
    auto* trust = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(trust, nullptr) << (rig.window->notifications().log().empty()
                                      ? std::string("no notification")
                                      : rig.window->notifications().log().back().notification.text.toStdString());
    EXPECT_TRUE(trust->isVisible());
    EXPECT_FALSE(rig.trust->isTrusted(exeFolder));
    EXPECT_FALSE(rig.workbench->debugController()->debugging());
    trust->reject();
    QApplication::processEvents();
    EXPECT_FALSE(rig.trust->isTrusted(exeFolder));
    EXPECT_FALSE(rig.workbench->debugController()->debugging());
}

TEST(Workbench, CompoundRunIsUnavailableUntilRunFiveAndStartsNothing) {
    WorkDir dir("compound-run", "tiny_sln");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto id = rig.workbench->registry().find("run.runCompound");
    ASSERT_TRUE(id);
    frost::input::ActionContext context;
    context.trusted = true;
    EXPECT_EQ(rig.workbench->registry().tryInvoke(*id, context), frost::input::InvokeResult::Disabled);
    EXPECT_TRUE(launcher.launches().empty());
}

// RUN-5: the Run/Debug Configurations dialog's compound section is the feature now (it replaced
// R2's disabled RUN-5 notice, D32): in an open workspace Add Compound... adds an unnamed compound
// configuration to the dialog's list and starts nothing; Cancel stores nothing.
TEST(Workbench, CompoundRunInTheRunConfigurationsDialogAddsAConfigurationAndStartsNothing) {
    WorkDir dir("compound-run-dialog", "tiny_sln");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto id = rig.workbench->registry().find("run.editConfigurations");
    ASSERT_TRUE(id);
    const std::size_t before = rig.workbench->runController()->configurations().all().size();
    bool clicked = false;
    bool enabled = false;
    int rowsBefore = -1;
    int rowsAfter = -1;
    QString added;
    QTimer::singleShot(0, rig.window.get(), [&] {
        auto* group = rig.window->findChild<QWidget*>(QStringLiteral("compoundRunGroup"));
        auto* launch = rig.window->findChild<QPushButton*>(QStringLiteral("compoundLaunchButton"));
        auto* list = rig.window->findChild<QListWidget*>(QStringLiteral("runConfigurationList"));
        if (group != nullptr && launch != nullptr && list != nullptr) {
            enabled = launch->isEnabled() && group->isEnabled();
            rowsBefore = list->count();
            launch->click();
            rowsAfter = list->count();
            if (list->currentItem() != nullptr)
                added = list->currentItem()->text();
            clicked = true;
        }
        // Cancel: no dialog stays open.
        for (QDialog* dialog : rig.window->findChildren<QDialog*>())
            if (dialog->isVisible())
                dialog->reject();
    });
    frost::input::ActionContext context;
    context.trusted = true;
    EXPECT_EQ(rig.workbench->registry().tryInvoke(*id, context), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(clicked);
    EXPECT_TRUE(enabled) << "an open workspace can store a compound configuration";
    EXPECT_EQ(rowsAfter, rowsBefore + 1);
    EXPECT_TRUE(added.contains(QStringLiteral("compound"))) << added.toStdString();
    EXPECT_EQ(rig.workbench->runController()->configurations().all().size(), before) << "Cancel stored it";
    EXPECT_TRUE(launcher.launches().empty());
}

// The "Debug Selected Test" and window.services shortcuts are gone: neither did what it said.
TEST(Workbench, NoActionPretendsToDebugOneTestOrOpenAServicesWindow) {
    WorkDir dir("removed-actions");
    WorkbenchRig rig(dir);
    EXPECT_FALSE(rig.workbench->registry().find("window.services").has_value());
    EXPECT_TRUE(rig.workbench->registry().find("window.unitTests").has_value());
    // The IntelliJ keymap still names window.services for Alt+8; with no such action that binding
    // resolves to nothing, and Alt+8 still toggles the Unit Tests window (module default).
    rig.show();
    rig.window->refreshActions();
    ASSERT_FALSE(rig.window->toolWindows().isVisible(frost::app::kUnitTestsToolWindow));
    sendKey(rig.window.get(), Qt::Key_8, Qt::AltModifier);
    EXPECT_TRUE(rig.window->toolWindows().isVisible(frost::app::kUnitTestsToolWindow));
    sendKey(rig.window.get(), Qt::Key_8, Qt::AltModifier);
    EXPECT_FALSE(rig.window->toolWindows().isVisible(frost::app::kUnitTestsToolWindow));
}

} // namespace
