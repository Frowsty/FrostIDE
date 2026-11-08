// SPDX-License-Identifier: Apache-2.0
// Find in Files through the actual Workbench, editor and filesystem.
#include "frost/project/directory_roles.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/find/find_item_roles.hpp"
#include "frost/ui/find/find_popup.hpp"
#include "frost/ui/find/find_tool_window.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "workbench_rig.hpp"
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>

namespace {
using namespace frost;
using app::test::WorkbenchRig;
using app::test::WorkDir;
namespace actions = ui::find_actions;

ui::FindFeature* feature(WorkbenchRig& rig) { return rig.window->findChild<ui::FindFeature*>(); }

TEST(WorkbenchFind, FindsDirtyAndClosedFilesAndNavigatesUtf8ByteColumn) {
    WorkDir dir("find-navigate");
    dir.write("open.txt", "original\n");
    dir.write("closed.txt", "\xc3\xa9 needle\r\n");
    dir.write("ignored.txt", "needle\n");
    dir.write(".gitignore", "ignored.txt\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("open.txt")));
    ASSERT_TRUE(rig.view()->editor().typeText("needle "));
    rig.show();
    ASSERT_EQ(rig.window->invoke(actions::kFindInFiles), input::InvokeResult::Ran);
    ASSERT_NE(feature(rig), nullptr);
    auto* popup = feature(rig)->popup();
    ASSERT_NE(popup, nullptr);
    popup->queryField()->setText(QStringLiteral("needle"));
    popup->searchNow();
    popup->session()->wait();
    ASSERT_FALSE(popup->session()->error());
    ASSERT_EQ(popup->session()->matchCount(), 2U);
    bool selected = false;
    for (int row = 0; row < popup->resultList()->count(); ++row) {
        auto* item = popup->resultList()->item(row);
        int file = item->data(ui::kFindFileIndexRole).toInt();
        if (popup->session()->files().at(static_cast<std::size_t>(file)).path == dir.join("closed.txt")) {
            popup->resultList()->setCurrentRow(row);
            selected = true;
            break;
        }
    }
    ASSERT_TRUE(selected);
    ASSERT_TRUE(popup->activateCurrent());
    ASSERT_NE(rig.view(1), nullptr);
    EXPECT_EQ(rig.view(1)->path(), dir.join("closed.txt"));
    EXPECT_EQ(rig.view(1)->editor().editor().state().primary().head.value(), 3U);
}

TEST(WorkbenchFind, ResultForProjectFileOpensItsTextWithoutSwitchingProject) {
    WorkDir dir("find-project-file");
    dir.write("nested/project.frostproj", "needle\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    rig.show();
    auto* popup = feature(rig)->showPopup(false);
    popup->queryField()->setText(QStringLiteral("needle"));
    popup->searchNow();
    popup->session()->wait();
    ASSERT_EQ(popup->session()->matchCount(), 1U);
    ASSERT_TRUE(popup->activateCurrent());
    ASSERT_NE(rig.view(), nullptr);
    EXPECT_EQ(rig.view()->path(), dir.join("nested/project.frostproj"));
    auto* reopened = feature(rig)->showPopup(false);
    EXPECT_EQ(reopened->settings().scope.directory, dir.path());
}

TEST(WorkbenchFind, ReplaceFollowsEditorAndUndoPreservesDirtyTextAndClosedEncoding) {
    WorkDir dir("find-replace");
    dir.write("open.txt", "needle\n");
    dir.write("closed.txt", "\xef\xbb\xbfneedle\r\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("open.txt")));
    ASSERT_TRUE(rig.view()->editor().typeText("dirty "));
    rig.show();
    ASSERT_EQ(rig.window->invoke(actions::kReplaceInFiles), input::InvokeResult::Ran);
    auto* popup = feature(rig)->popup();
    ASSERT_NE(popup, nullptr);
    popup->queryField()->setText(QStringLiteral("needle"));
    popup->searchNow();
    popup->session()->wait();
    ASSERT_EQ(popup->session()->matchCount(), 2U);
    // Replacement text may be typed after results have already arrived.
    popup->replacementField()->setText(QStringLiteral("pin"));
    int changed = 0;
    QObject::connect(&rig.view()->editor(), &ui::EditorView::textChanged, [&changed] { ++changed; });
    auto replaced = feature(rig)->replace(*popup, true);
    ASSERT_TRUE(replaced.has_value());
    EXPECT_EQ(replaced->replacements, 2U);
    EXPECT_EQ(rig.view()->editor().document().text().text(), "dirty pin\n");
    EXPECT_TRUE(rig.view()->document().dirty());
    EXPECT_EQ(rig.view()->editor().editor().state().primary().head.value(), 9U);
    EXPECT_GT(changed, 0);
    EXPECT_EQ(dir.read("open.txt"), "needle\n");
    EXPECT_EQ(dir.read("closed.txt"), "\xef\xbb\xbfpin\r\n");
    ASSERT_TRUE(rig.view()->editor().perform(ui::EditorAction::Undo));
    EXPECT_EQ(rig.view()->editor().document().text().text(), "dirty needle\n");
}

TEST(WorkbenchFind, ProjectSwitchRefreshesAutomaticDirectoryAndKeepsExplicitScope) {
    WorkDir dir("find-project-switch");
    dir.write("one/a.txt", "first\n");
    dir.write("two/b.txt", "second\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("one")));
    rig.show();
    auto* popup = feature(rig)->showPopup(false);
    EXPECT_EQ(popup->settings().scope.directory, dir.join("one"));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("two")));
    popup = feature(rig)->showPopup(false);
    EXPECT_EQ(popup->settings().scope.directory, dir.join("two"));
    const QString chosen = QString::fromStdString(dir.join("two"));
    popup->directoryField()->setText(chosen);
    ASSERT_TRUE(QMetaObject::invokeMethod(popup->directoryField(), "textEdited", Q_ARG(QString, chosen)));
    popup->scopeButton(search::ScopeKind::OpenFiles)->click();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("one")));
    popup = feature(rig)->showPopup(false);
    EXPECT_EQ(popup->settings().scope.kind, search::ScopeKind::OpenFiles);
    EXPECT_EQ(popup->settings().scope.directory, dir.join("two"));
}

TEST(WorkbenchFind, RestoresExplicitSolutionScopeWhenItBecomesAvailableAgain) {
    WorkDir dir("find-scope-restore", "tiny_sln");
    dir.write("other/file.txt", "text\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    rig.show();
    auto* popup = feature(rig)->showPopup(false);
    popup->scopeButton(search::ScopeKind::Solution)->click();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("other")));
    popup = feature(rig)->showPopup(false);
    EXPECT_EQ(popup->settings().scope.kind, search::ScopeKind::Directory);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    popup = feature(rig)->showPopup(false);
    EXPECT_EQ(popup->settings().scope.kind, search::ScopeKind::Solution);
}

TEST(WorkbenchFind, SolutionScopeFindsUnsavedProjectFile) {
    WorkDir dir("find-solution", "tiny_sln");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/main.cpp")));
    ASSERT_TRUE(rig.view()->editor().typeText("unique_find_token "));
    rig.show();
    auto* popup = feature(rig)->showPopup(false);
    EXPECT_EQ(popup->settings().scope.kind, search::ScopeKind::Solution);
    popup->queryField()->setText(QStringLiteral("unique_find_token"));
    popup->searchNow();
    popup->session()->wait();
    ASSERT_FALSE(popup->session()->error());
    EXPECT_EQ(popup->session()->matchCount(), 1U);
}

TEST(WorkbenchFind, ReplacesOnlySelectedOccurrence) {
    WorkDir dir("find-selected");
    dir.write("open.txt", "needle needle\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("open.txt")));
    rig.show();
    auto* popup = feature(rig)->showPopup(true);
    popup->queryField()->setText(QStringLiteral("needle"));
    popup->replacementField()->setText(QStringLiteral("pin"));
    popup->searchNow();
    popup->session()->wait();
    ASSERT_EQ(popup->session()->matchCount(), 2U);
    popup->resultList()->setCurrentRow(0);
    auto result = feature(rig)->replace(*popup, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->replacements, 1U);
    EXPECT_EQ(rig.view()->editor().document().text().text(), "pin needle\n");
    ASSERT_TRUE(rig.view()->editor().perform(ui::EditorAction::Undo));
    EXPECT_EQ(rig.view()->editor().document().text().text(), "needle needle\n");
}

TEST(WorkbenchFind, ReplaceSkipsClosedFileChangedAfterSearch) {
    WorkDir dir("find-stale");
    dir.write("closed.txt", "needle\r\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    rig.show();
    auto* popup = feature(rig)->showPopup(true);
    popup->queryField()->setText(QStringLiteral("needle"));
    popup->replacementField()->setText(QStringLiteral("pin"));
    popup->searchNow();
    popup->session()->wait();
    ASSERT_EQ(popup->session()->matchCount(), 1U);
    dir.write("closed.txt", "needle externally changed\r\n");
    auto result = feature(rig)->replace(*popup, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->skipped.size(), 1U);
    EXPECT_EQ(result->skipped[0].reason, search::ReplaceSkipReason::Changed);
    EXPECT_EQ(dir.read("closed.txt"), "needle externally changed\r\n");
}

TEST(WorkbenchFind, ChangedQueryCannotReplaceStaleResultsBeforeDebounce) {
    WorkDir dir("find-stale-query");
    dir.write("file.txt", "needle\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    rig.show();
    auto* popup = feature(rig)->showPopup(true);
    popup->queryField()->setText(QStringLiteral("needle"));
    popup->searchNow();
    popup->session()->wait();
    ASSERT_EQ(popup->session()->matchCount(), 1U);
    popup->replacementField()->setText(QStringLiteral("pin"));
    popup->queryField()->setText(QStringLiteral("other"));
    EXPECT_FALSE(popup->resultsCurrent());
    EXPECT_FALSE(popup->replaceAllButton()->isEnabled());
    EXPECT_FALSE(feature(rig)->replace(*popup, true));
    EXPECT_EQ(dir.read("file.txt"), "needle\n");
}

TEST(WorkbenchFind, QueuedActionsAreInertAfterShutdown) {
    WorkDir dir("find-shutdown-actions");
    dir.write("file.txt", "needle\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    rig.show();
    ASSERT_NE(feature(rig), nullptr);
    QTimer::singleShot(0, rig.window.get(), [&rig] {
        (void)rig.window->invoke(actions::kFindInFiles);
        (void)rig.window->invoke(actions::kReplaceInFiles);
    });
    rig.workbench->shutdown();
    QApplication::processEvents();
    EXPECT_EQ(feature(rig), nullptr);
    EXPECT_EQ(rig.window->findChild<ui::FindPopup*>(), nullptr);
    EXPECT_EQ(dir.read("file.txt"), "needle\n");
}

TEST(WorkbenchFind, TransferredSearchIsCancelledBeforeShutdown) {
    WorkDir dir("find-transfer");
    dir.write("one.txt", "needle\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    rig.show();
    auto* popup = feature(rig)->showPopup(false);
    popup->queryField()->setText(QStringLiteral("needle"));
    popup->searchNow();
    popup->session()->wait();
    auto* window = feature(rig)->openInFindWindow(*popup);
    ASSERT_NE(window, nullptr);
    EXPECT_TRUE(window->hasOccurrences());
    ASSERT_EQ(rig.window->invoke(actions::kNextOccurrence), input::InvokeResult::Ran);
    ASSERT_NE(rig.view(), nullptr);
    EXPECT_EQ(rig.view()->path(), dir.join("one.txt"));
    window->session()->rerun();
    ASSERT_TRUE(window->session()->running());
    rig.workbench->shutdown();
    EXPECT_FALSE(window->hasOccurrences());
}
// UX-23 through the workbench: the project tree and Find in Files follow the project's directory
// roles. Build output (a build/ folder, a CMake preset's binary folder, a folder holding a
// CMakeCache.txt, .cache/) is neither listed nor searched; a source folder named Debug is. "Mark
// Directory as" in the File System view changes both and keeps the override.
TEST(WorkbenchFind, DirectoryRolesDecideWhatTheProjectTreeAndFindInFilesWalk) {
    WorkDir dir("find-roles");
    dir.write("CMakePresets.json",
              R"({"version":6,"configurePresets":[{"name":"dev","binaryDir":"${sourceDir}/artifacts/dev"}]})");
    dir.write("src/main.cpp", "needle\n");
    dir.write("Renderer/Debug/stats.cpp", "needle\n");
    dir.write("vendor/lib.cpp", "needle\n");
    dir.write("build/gen.cpp", "needle\n");
    dir.write("artifacts/dev/gen.cpp", "needle\n");
    dir.write("manual/CMakeCache.txt", "needle\n");
    dir.write(".cache/cpm/dep.cpp", "needle\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(app::test::waitFor([&] { return rig.nav().initialScanReady() && !rig.nav().scanPending(); }));
    ASSERT_NE(rig.nav().directoryRoles(), nullptr);
    const auto inTree = [&](std::string_view relative) {
        return rig.nav().projectFiles().find(dir.join(std::string(relative))).has_value();
    };
    EXPECT_TRUE(inTree("Renderer/Debug/stats.cpp"));
    EXPECT_TRUE(inTree("vendor/lib.cpp"));
    EXPECT_FALSE(inTree("build/gen.cpp"));
    EXPECT_FALSE(inTree("artifacts/dev/gen.cpp"));
    EXPECT_TRUE(inTree("artifacts/dev")); // Listed (so it can be marked again), never descended.
    EXPECT_FALSE(inTree("manual/CMakeCache.txt"));
    EXPECT_FALSE(inTree(".cache/cpm/dep.cpp"));

    rig.show();
    const auto searchedFiles = [&] {
        auto* popup = feature(rig)->showPopup(false);
        popup->queryField()->setText(QStringLiteral("needle"));
        popup->searchNow();
        popup->session()->wait();
        EXPECT_FALSE(popup->session()->error());
        std::set<std::string> files;
        for (const auto& file : popup->session()->files())
            files.insert(file.path);
        popup->closePopup(ui::Popup::CloseReason::Cancelled);
        return files;
    };
    EXPECT_EQ(searchedFiles(), (std::set<std::string>{dir.join("Renderer/Debug/stats.cpp"), dir.join("src/main.cpp"),
                                                      dir.join("vendor/lib.cpp")}));

    // Mark Directory as > Excluded on vendor/, from the File System view's menu.
    ui::FileSystemView* view = rig.nav().fileSystemView();
    ASSERT_NE(view, nullptr);
    QModelIndex vendor;
    ASSERT_TRUE(app::test::waitFor([&] { return (vendor = view->indexOfPath(dir.join("vendor"))).isValid(); }));
    {
        std::unique_ptr<QMenu> menu(view->createContextMenu(vendor));
        auto* source = menu->findChild<QAction*>(QStringLiteral("markDirectorySource"));
        auto* excluded = menu->findChild<QAction*>(QStringLiteral("markDirectoryExcluded"));
        auto* reset = menu->findChild<QAction*>(QStringLiteral("markDirectoryDefault"));
        ASSERT_NE(source, nullptr);
        ASSERT_NE(excluded, nullptr);
        ASSERT_NE(reset, nullptr);
        EXPECT_TRUE(source->isChecked());
        EXPECT_FALSE(reset->isEnabled()); // No override yet.
        excluded->trigger();
    }
    ASSERT_TRUE(app::test::waitFor([&] { return !inTree("vendor/lib.cpp") && !rig.nav().scanPending(); }));
    EXPECT_EQ(rig.nav().directoryRoles()->decide(dir.join("vendor")).reason, project::DirectoryRoleReason::Override);
    EXPECT_EQ(searchedFiles(), (std::set<std::string>{dir.join("Renderer/Debug/stats.cpp"), dir.join("src/main.cpp")}));

    // Marking the preset's build folder as sources walks it again; Default drops the override.
    ASSERT_TRUE(rig.nav().setDirectoryRole(dir.join("artifacts/dev"), project::DirectoryRole::Source).has_value());
    ASSERT_TRUE(app::test::waitFor([&] { return inTree("artifacts/dev/gen.cpp") && !rig.nav().scanPending(); }));
    EXPECT_TRUE(searchedFiles().contains(dir.join("artifacts/dev/gen.cpp")));
    ASSERT_TRUE(rig.nav().setDirectoryRole(dir.join("artifacts/dev"), std::nullopt).has_value());
    ASSERT_TRUE(app::test::waitFor([&] { return !inTree("artifacts/dev/gen.cpp") && !rig.nav().scanPending(); }));
    EXPECT_FALSE(rig.nav().setDirectoryRole(dir.join("../outside"), project::DirectoryRole::Excluded).has_value());
}
} // namespace
