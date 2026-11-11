// SPDX-License-Identifier: Apache-2.0
//
// Functional tests of EditorTabs (frost/ui/shell/editor_tabs.hpp) over fake editors, offscreen:
// open, re-open, close, close others/all, pin, the dirty marker, middle-click close, the MRU order
// and the tab list for overflow.

#include "frost/ui/shell/editor_tabs.hpp"

#include "shell/shell_rig.hpp"
#include <QColor>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using frost::ui::EditorTabs;
using frost::ui::test::FakeEditorFactory;

// Clicks tab `index` with the middle button (press and release on the same tab).
void middleClick(QTabBar& bar, int index) {
    const QPointF at = bar.tabRect(index).center();
    const QPointF global = bar.mapToGlobal(at);
    QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &release);
}

TEST(EditorTabs, OpensActivatesAndReusesTabs) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    EXPECT_FALSE(tabs.emptyState()->isHidden());
    int changes = 0;
    QObject::connect(&tabs, &EditorTabs::currentEditorChanged, [&changes] { ++changes; });

    ASSERT_EQ(tabs.open("/src/a.cpp").value_or(-1), 0);
    ASSERT_EQ(tabs.open("/src/b.cpp").value_or(-1), 1);
    EXPECT_EQ(tabs.count(), 2);
    EXPECT_EQ(tabs.currentIndex(), 1);
    EXPECT_EQ(tabs.tabText(0), QStringLiteral("a.cpp"));
    EXPECT_TRUE(tabs.emptyState()->isHidden());
    EXPECT_GE(changes, 2);

    // Opening an open path activates its tab instead of a second editor.
    EXPECT_EQ(tabs.open("/src/a.cpp").value_or(-1), 0);
    EXPECT_EQ(factory.opened, 2);
    EXPECT_EQ(tabs.currentIndex(), 0);
    // A background open keeps the current tab.
    EXPECT_EQ(tabs.open("/src/c.cpp", false).value_or(-1), 2);
    EXPECT_EQ(tabs.currentIndex(), 0);

    factory.failing.emplace_back("/missing");
    const auto failed = tabs.open("/missing");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), frost::ErrorCode::NotFound);
    EXPECT_EQ(tabs.count(), 3);
}

TEST(EditorTabs, SplitGroupsKeepIndependentTabsAndActiveRouting) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    ASSERT_TRUE(tabs.open("/a.cpp").has_value());
    auto* first = tabs.activeGroup();
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    auto* second = tabs.activeGroup();
    ASSERT_NE(first, second);
    EXPECT_EQ(tabs.groupCount(), 2);
    EXPECT_EQ(tabs.currentEditor()->path(), "/a.cpp");
    ASSERT_TRUE(tabs.open("/b.cpp").has_value());
    EXPECT_EQ(second->count(), 2);
    EXPECT_EQ(first->count(), 1);
    tabs.setActiveGroup(first);
    EXPECT_EQ(tabs.currentEditor()->path(), "/a.cpp");
    ASSERT_TRUE(tabs.split(Qt::Vertical));
    EXPECT_EQ(tabs.groupCount(), 3);
    EXPECT_TRUE(tabs.closeGroup());
    EXPECT_EQ(tabs.groupCount(), 2);
    tabs.setActiveGroup(second);
    EXPECT_EQ(tabs.currentEditor()->path(), "/b.cpp");
    tabs.closeAll();
    EXPECT_EQ(tabs.totalCount(), 0);
}

TEST(EditorTabs, FocusSwitchChangesActiveGroup) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    tabs.show();
    ASSERT_TRUE(tabs.open("/a.cpp").has_value());
    auto* first = tabs.activeGroup();
    QWidget* firstWidget = tabs.currentEditor()->widget();
    firstWidget->setFocusPolicy(Qt::StrongFocus);
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    auto* second = tabs.activeGroup();
    QWidget* secondWidget = tabs.currentEditor()->widget();
    secondWidget->setFocusPolicy(Qt::StrongFocus);
    firstWidget->setFocus();
    QApplication::processEvents();
    EXPECT_EQ(tabs.activeGroup(), first);
    secondWidget->setFocus();
    QApplication::processEvents();
    EXPECT_EQ(tabs.activeGroup(), second);
}

TEST(EditorTabs, EmptySplitGroupHasCloseControl) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    ASSERT_TRUE(tabs.open("/a.cpp"));
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    ASSERT_TRUE(tabs.close(tabs.currentIndex()));
    auto* button = tabs.activeGroup()->findChild<QToolButton*>(QStringLiteral("closeEditorGroup"));
    ASSERT_NE(button, nullptr);
    EXPECT_FALSE(button->isHidden());
    button->click();
    EXPECT_EQ(tabs.groupCount(), 1);
    EXPECT_EQ(tabs.currentEditor()->path(), "/a.cpp");
}

TEST(EditorTabs, SplitLayoutRoundTripsAndRejectsInvalidTree) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    ASSERT_TRUE(tabs.open("/a.cpp").has_value());
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    ASSERT_TRUE(tabs.split(Qt::Vertical));
    const std::string layout = tabs.saveLayout();
    tabs.closeAll();
    ASSERT_TRUE(tabs.restoreLayout(layout));
    EXPECT_EQ(tabs.groupCount(), 3);
    EXPECT_EQ(tabs.saveLayout(), layout);
    EXPECT_FALSE(tabs.restoreLayout(R"({"children":[{"group":0},{"group":0}]})"));
    EXPECT_EQ(tabs.groupCount(), 3);
}

TEST(EditorTabs, CloseGroupRespectsInactiveDocumentVeto) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    ASSERT_TRUE(tabs.open("/a.cpp").has_value());
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    ASSERT_TRUE(tabs.open("/b.cpp").has_value());
    factory.last->allowClose = false;
    EXPECT_FALSE(tabs.closeGroup());
    EXPECT_EQ(tabs.groupCount(), 2);
    EXPECT_EQ(tabs.totalCount(), 2);
    tabs.closeAll();
    EXPECT_EQ(tabs.totalCount(), 1);
}

TEST(EditorTabs, CloseAllRetainsVetoedTabInInactiveGroup) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    ASSERT_TRUE(tabs.open("/a.cpp"));
    auto* first = tabs.activeGroup();
    factory.last->allowClose = false;
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    ASSERT_TRUE(tabs.open("/b.cpp"));
    tabs.closeAll();
    EXPECT_EQ(tabs.totalCount(), 1);
    EXPECT_EQ(first->count(), 1);
    EXPECT_EQ(first->currentEditor()->path(), "/a.cpp");
}

TEST(EditorTabs, DirtyMarkerFollowsTheEditor) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    ASSERT_TRUE(tabs.open("/src/main.cpp").has_value());
    frost::ui::test::FakeEditor* editor = factory.last;
    EXPECT_FALSE(tabs.isDirty(0));
    editor->setDirty(true);
    EXPECT_TRUE(tabs.isDirty(0));
    EXPECT_EQ(tabs.tabText(0), QStringLiteral("*main.cpp"));
    EXPECT_EQ(tabs.tabBar()->tabText(0), QStringLiteral("*main.cpp"));
    editor->setDirty(false);
    EXPECT_EQ(tabs.tabText(0), QStringLiteral("main.cpp"));
}

TEST(EditorTabs, TitleColoursFollowTheProvider) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    ASSERT_TRUE(tabs.open("/src/a.cpp").has_value());
    EXPECT_FALSE(tabs.titleColor(0).isValid());

    // The VCS colours (M5): a modified a.cpp, an unversioned b.cpp opened later.
    std::string modified = "/src/a.cpp";
    tabs.setTitleColorProvider([&modified](const std::string& path) -> std::optional<QColor> {
        if (path == modified) {
            return QColor(0x6D, 0x9F, 0xE6);
        }
        if (path == "/src/b.cpp") {
            return QColor(0xD0, 0x7A, 0x5A);
        }
        return std::nullopt;
    });
    EXPECT_EQ(tabs.titleColor(0), QColor(0x6D, 0x9F, 0xE6));
    ASSERT_TRUE(tabs.open("/src/b.cpp").has_value());
    EXPECT_EQ(tabs.titleColor(1), QColor(0xD0, 0x7A, 0x5A));

    // After a commit a.cpp is clean again: a refresh restores the default colour.
    modified.clear();
    tabs.refreshTitleColors();
    EXPECT_FALSE(tabs.titleColor(0).isValid());
    EXPECT_FALSE(tabs.titleColor(7).isValid());
}

TEST(EditorTabs, ClosesTabsAndRespectsTheEditor) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    for (const char* path : {"/a", "/b", "/c", "/d"}) {
        ASSERT_TRUE(tabs.open(path).has_value());
    }
    EXPECT_TRUE(tabs.close(1));
    EXPECT_EQ(tabs.count(), 3);
    EXPECT_EQ(tabs.indexOf("/b"), -1);

    // An editor that refuses (unsaved changes, the user cancelled) keeps its tab.
    factory.last->allowClose = false;
    EXPECT_FALSE(tabs.close(tabs.indexOf("/d")));
    EXPECT_EQ(tabs.count(), 3);
    factory.last->allowClose = true;

    tabs.closeOthers(tabs.indexOf("/c"));
    EXPECT_EQ(tabs.count(), 1);
    EXPECT_EQ(tabs.currentEditor()->path(), "/c");
    tabs.closeAll();
    EXPECT_EQ(tabs.count(), 0);
    EXPECT_EQ(tabs.currentEditor(), nullptr);
    EXPECT_FALSE(tabs.emptyState()->isHidden());
}

TEST(EditorTabs, PinnedTabsMoveFirstAndSurviveCloseOthers) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    for (const char* path : {"/a", "/b", "/c"}) {
        ASSERT_TRUE(tabs.open(path).has_value());
    }
    tabs.setPinned(tabs.indexOf("/c"), true);
    EXPECT_EQ(tabs.indexOf("/c"), 0);
    EXPECT_TRUE(tabs.isPinned(0));
    EXPECT_EQ(tabs.currentEditor()->path(), "/c"); // Pinning keeps the current editor.

    tabs.closeOthers(tabs.indexOf("/a"));
    EXPECT_EQ(tabs.count(), 2); // /c is pinned, /a was asked for.
    EXPECT_NE(tabs.indexOf("/c"), -1);
    EXPECT_NE(tabs.indexOf("/a"), -1);
    tabs.closeAllButPinned();
    EXPECT_EQ(tabs.count(), 1);
    EXPECT_EQ(tabs.editor(0)->path(), "/c");
    tabs.setPinned(0, false);
    EXPECT_FALSE(tabs.isPinned(0));
}

TEST(EditorTabs, MiddleClickClosesTheTab) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    tabs.resize(800, 400);
    ASSERT_TRUE(tabs.open("/a").has_value());
    ASSERT_TRUE(tabs.open("/b").has_value());
    tabs.show();
    QApplication::processEvents();
    middleClick(*tabs.tabBar(), 0);
    EXPECT_EQ(tabs.count(), 1);
    EXPECT_EQ(tabs.editor(0)->path(), "/b");
}

TEST(EditorTabs, MruOrderAndCycling) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    for (const char* path : {"/a", "/b", "/c"}) {
        ASSERT_TRUE(tabs.open(path).has_value());
    }
    tabs.activate(tabs.indexOf("/a"));
    const std::vector<int> mru = tabs.mruOrder();
    ASSERT_EQ(mru.size(), 3U);
    EXPECT_EQ(tabs.editor(mru[0])->path(), "/a");
    EXPECT_EQ(tabs.editor(mru[1])->path(), "/c");
    EXPECT_EQ(tabs.editor(mru[2])->path(), "/b");

    tabs.activateNext();
    EXPECT_EQ(tabs.currentEditor()->path(), "/b");
    tabs.activatePrevious();
    tabs.activatePrevious();
    EXPECT_EQ(tabs.currentEditor()->path(), "/c"); // Wraps around.
}

TEST(EditorTabs, TabListShowsEveryTabWhenOverflowing) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    tabs.resize(160, 200);
    tabs.show();
    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(tabs.open("/src/a_rather_long_file_name_" + std::to_string(i) + ".cpp").has_value());
    }
    QApplication::processEvents();
    EXPECT_TRUE(tabs.isOverflowing());
    EXPECT_FALSE(tabs.tabListButton()->isHidden());
    QMenu menu;
    tabs.populateTabList(menu);
    EXPECT_EQ(menu.actions().size(), 12);
    menu.actions().front()->trigger();
    EXPECT_EQ(tabs.currentIndex(), 0);
}

TEST(EditorTabs, CloseTabsToLeftAndRightPreservePinnedTabs) {
    FakeEditorFactory factory;
    EditorTabs tabs(factory);
    for (const char* path : {"/a.cpp", "/b.cpp", "/c.cpp", "/d.cpp", "/e.cpp"}) {
        ASSERT_TRUE(tabs.open(path).has_value());
    }
    tabs.setPinned(tabs.indexOf("/a.cpp"), true);
    // Order is now: /a.cpp (pinned), /b.cpp, /c.cpp, /d.cpp, /e.cpp.
    tabs.closeTabsToLeft(tabs.indexOf("/d.cpp"));
    EXPECT_EQ(tabs.count(), 3);
    EXPECT_NE(tabs.indexOf("/a.cpp"), -1);
    EXPECT_EQ(tabs.indexOf("/b.cpp"), -1);
    EXPECT_EQ(tabs.indexOf("/c.cpp"), -1);
    EXPECT_NE(tabs.indexOf("/d.cpp"), -1);
    EXPECT_NE(tabs.indexOf("/e.cpp"), -1);

    tabs.closeTabsToRight(tabs.indexOf("/d.cpp"));
    EXPECT_EQ(tabs.count(), 2);
    EXPECT_NE(tabs.indexOf("/a.cpp"), -1);
    EXPECT_NE(tabs.indexOf("/d.cpp"), -1);
    EXPECT_EQ(tabs.indexOf("/e.cpp"), -1);
}

} // namespace
