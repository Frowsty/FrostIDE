// SPDX-License-Identifier: Apache-2.0
//
// Functional tests of ShellWindow (frost/ui/shell/shell_window.hpp), offscreen: the window builds,
// the menus come from the action registry with the keymap's shortcut text (de-DE included), the
// dispatcher routes keys to the matcher once per event, the status bar follows the active editor,
// tool windows toggle and remember their sizes, the popups open and close, and pseudo-localization
// reaches the shell's strings.

#include "frost/core/i18n.hpp"
#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_menus.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/status_bar.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "shell/shell_rig.hpp"
#include <QAction>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
#include <QToolButton>
#include <gtest/gtest.h>

namespace {

using frost::input::SimulatedLayoutId;
using frost::ui::EditorStatus;
using frost::ui::ShellWindow;
using frost::ui::test::ShellRig;
namespace actions = frost::ui::actions;
namespace tool_windows = frost::ui::tool_windows;

// Sends a key press (and, with `release`, the release) to `target` as an application would.
void sendKey(QWidget* target, int key, Qt::KeyboardModifiers modifiers, bool release = true) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &press);
    if (release) {
        QKeyEvent up(QEvent::KeyRelease, key, modifiers);
        QApplication::sendEvent(target, &up);
    }
}

TEST(ShellWindow, BuildsTheRiderLayout) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    EXPECT_EQ(window.windowTitle(), QStringLiteral("FrostIDE"));
    EXPECT_EQ(window.statusBar()->currentMessage(), QStringLiteral("Ready"));
    EXPECT_NE(window.centralWidget(), nullptr);
    EXPECT_NE(window.findChild<QLabel*>(QStringLiteral("placeholder")), nullptr);
    // The toolbar slots.
    EXPECT_NE(window.mainToolbar().runConfigurations(), nullptr);
    EXPECT_FALSE(window.mainToolbar().runConfigurations()->isEnabled());
    for (const std::string_view id : {frost::ui::MainToolbar::kBuild, frost::ui::MainToolbar::kRun,
                                      frost::ui::MainToolbar::kDebug, frost::ui::MainToolbar::kStop}) {
        ASSERT_NE(window.mainToolbar().button(id), nullptr) << id;
        EXPECT_FALSE(window.mainToolbar().button(id)->isEnabled()) << id; // Nothing registered them yet.
    }
    // A later slice fills the combo and registers Build: both become usable.
    window.mainToolbar().runConfigurations()->addItem(QStringLiteral("Debug | x64"));
    EXPECT_TRUE(window.mainToolbar().runConfigurations()->isEnabled());
    int builds = 0;
    rig.addCounted("build.project", "Build Solution", builds);
    window.refreshActions();
    EXPECT_TRUE(window.mainToolbar().button(frost::ui::MainToolbar::kBuild)->isEnabled());
    window.mainToolbar().button(frost::ui::MainToolbar::kBuild)->click();
    EXPECT_EQ(builds, 1);
    // The default tool windows with their stripe buttons.
    for (const std::string_view id : {tool_windows::kProject, tool_windows::kCommit, tool_windows::kRun,
                                      tool_windows::kProblems, tool_windows::kNotifications}) {
        EXPECT_TRUE(window.toolWindows().contains(id)) << id;
        EXPECT_NE(window.toolWindows().stripeButton(id), nullptr) << id;
        EXPECT_FALSE(window.toolWindows().isVisible(id)) << id;
    }
}

TEST(ShellWindow, MenusComeFromTheRegistryWithShortcutText) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    frost::ui::ShellMenus& menus = window.menus();
    // Shell actions with their IntelliJ shortcuts.
    ASSERT_NE(menus.action(actions::kProjectWindow), nullptr);
    EXPECT_EQ(menus.action(actions::kProjectWindow)->text(), QStringLiteral("Project\tAlt+1"));
    EXPECT_EQ(menus.action(actions::kGoToLine)->text(), QStringLiteral("Go to Line:Column...\tCtrl+G"));
    // QAction shortcuts are never set: the dispatcher is the only key path.
    EXPECT_TRUE(menus.action(actions::kProjectWindow)->shortcut().isEmpty());
    // The M0 Exit item keeps its object name.
    EXPECT_NE(window.findChild<QAction*>(QStringLiteral("actionExit")), nullptr);
    // Unregistered actions are hidden; a menu with nothing visible is hidden.
    ASSERT_NE(menus.action("editor.undo"), nullptr);
    EXPECT_FALSE(menus.action("editor.undo")->isVisible());
    EXPECT_FALSE(menus.menu("&Edit")->menuAction()->isVisible());
    EXPECT_TRUE(menus.menu("&File")->menuAction()->isVisible());

    // A slice registers an editor action later: it appears with its keymap shortcut.
    int comments = 0;
    rig.addCounted("editor.commentLine", "Comment Line", comments);
    // A placeholder (no handler) is shown disabled.
    frost::input::ActionSpec placeholder;
    placeholder.id = "editor.commentBlock";
    placeholder.name = "Comment with Block Comment";
    ASSERT_TRUE(rig.registry.add(std::move(placeholder)).has_value());
    menus.refreshIfStale();
    QAction* comment = menus.action("editor.commentLine");
    EXPECT_TRUE(comment->isVisible());
    EXPECT_TRUE(comment->isEnabled());
    EXPECT_EQ(comment->text(), QStringLiteral("Comment Line\tCtrl+/"));
    EXPECT_TRUE(menus.menu("&Code")->menuAction()->isVisible());
    EXPECT_TRUE(menus.action("editor.commentBlock")->isVisible());
    EXPECT_FALSE(menus.action("editor.commentBlock")->isEnabled());
    comment->trigger();
    EXPECT_EQ(comments, 1);
}

TEST(ShellWindow, GermanLayoutShowsCtrlMinusForCommentLine) {
    ShellRig rig(SimulatedLayoutId::DeDe);
    int comments = 0;
    rig.addCounted("editor.commentLine", "Comment Line", comments);
    ShellWindow window(rig.environment());
    // ADR 0030: on de-DE, Comment Line's Ctrl+Slash is the '-' key.
    EXPECT_EQ(window.menus().action("editor.commentLine")->text(), QStringLiteral("Comment Line\tCtrl+-"));
}

TEST(ShellWindow, DispatcherRoutesKeysToTheMatcher) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    frost::ui::test::showAndWait(window);
    QWidget* target = window.tabs().emptyState();

    // ShortcutOverride is accepted for a bound key, so Qt sends the press instead of a QShortcut.
    QKeyEvent bound(QEvent::ShortcutOverride, Qt::Key_1, Qt::AltModifier);
    bound.ignore();
    QApplication::sendEvent(target, &bound);
    EXPECT_TRUE(bound.isAccepted());
    QKeyEvent unbound(QEvent::ShortcutOverride, Qt::Key_Q, Qt::NoModifier);
    unbound.ignore();
    QApplication::sendEvent(target, &unbound);
    EXPECT_FALSE(unbound.isAccepted());

    // Alt+1 activates the Project tool window; Alt+4 and Alt+6 share the bottom side.
    sendKey(target, Qt::Key_1, Qt::AltModifier);
    EXPECT_TRUE(window.toolWindows().isVisible(tool_windows::kProject));
    sendKey(target, Qt::Key_4, Qt::AltModifier);
    EXPECT_TRUE(window.toolWindows().isVisible(tool_windows::kRun));
    sendKey(target, Qt::Key_6, Qt::AltModifier);
    EXPECT_TRUE(window.toolWindows().isVisible(tool_windows::kProblems));
    EXPECT_FALSE(window.toolWindows().isVisible(tool_windows::kRun));
    EXPECT_TRUE(window.toolWindows().isVisible(tool_windows::kProject));
}

TEST(ShellWindow, EachKeyEventReachesTheMatcherOnce) {
    // Double Shift fires on the second release only if every press and release is seen exactly once,
    // although Qt re-sends the ignored events from the label to each of its parents.
    ShellRig rig;
    int searches = 0;
    rig.addCounted("search.everywhere", "Search Everywhere", searches);
    ShellWindow window(rig.environment());
    frost::ui::test::showAndWait(window);
    QWidget* target = window.tabs().emptyState();
    sendKey(target, Qt::Key_Shift, Qt::ShiftModifier);
    sendKey(target, Qt::Key_Shift, Qt::ShiftModifier);
    EXPECT_EQ(searches, 1);
}

TEST(ShellWindow, TwoStrokePrefixShowsInTheStatusBar) {
    ShellRig rig;
    int commits = 0;
    rig.addCounted("vcs.commit", "Commit", commits);
    ShellWindow window(rig.environment());
    frost::ui::test::showAndWait(window);
    // Bind a two-stroke sequence through an override, as a user keymap would.
    frost::input::KeymapPreferences preferences;
    preferences.overrides.push_back(
        {"vcs.commit", {*frost::input::parseSequence("Ctrl+K, Ctrl+C", frost::input::SyntaxMode::Strict)}});
    ASSERT_TRUE(rig.keymap->setPreferences(preferences).has_value());
    QWidget* target = window.tabs().emptyState();
    sendKey(target, Qt::Key_K, Qt::ControlModifier);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("Ctrl+K")))
        << window.statusBar()->currentMessage().toStdString();
    sendKey(target, Qt::Key_C, Qt::ControlModifier);
    EXPECT_EQ(commits, 1);
    EXPECT_EQ(window.statusBar()->currentMessage(), QStringLiteral("Ready"));
}

TEST(ShellWindow, StatusBarFollowsTheActiveEditor) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    frost::ui::ShellStatusBar& status = window.statusWidgets();
    EXPECT_TRUE(status.caret()->isHidden());

    ASSERT_TRUE(window.openFile("/src/main.cpp").has_value());
    frost::ui::test::FakeEditor* editor = rig.editors.last;
    EditorStatus next;
    next.line = 12;
    next.column = 5;
    next.lineSeparator = "CRLF";
    next.encoding = "UTF-8 BOM";
    next.indent = {true, 4};
    next.readOnly = true;
    editor->setStatus(next);
    EXPECT_FALSE(status.caret()->isHidden());
    EXPECT_EQ(status.caret()->text(), QStringLiteral("12:5"));
    EXPECT_EQ(status.lineSeparator()->text(), QStringLiteral("CRLF"));
    EXPECT_EQ(status.encoding()->text(), QStringLiteral("UTF-8 BOM"));
    EXPECT_EQ(status.indent()->text(), QStringLiteral("Tab"));
    EXPECT_EQ(status.readOnly()->text(), QStringLiteral("Read-only"));
    EXPECT_EQ(window.windowTitle(), QStringLiteral("main.cpp - FrostIDE"));

    next.selectedCharacters = 3;
    next.indent = {false, 2};
    next.readOnly = false;
    editor->setStatus(next);
    EXPECT_EQ(status.caret()->text(), QStringLiteral("12:5 (3 chars)"));
    EXPECT_EQ(status.indent()->text(), QStringLiteral("2 spaces"));
    EXPECT_EQ(status.readOnly()->text(), QStringLiteral("Writable"));

    window.tabs().closeAll();
    EXPECT_TRUE(status.caret()->isHidden());
    EXPECT_EQ(window.windowTitle(), QStringLiteral("FrostIDE"));
}

TEST(ShellWindow, OpenFailureBecomesAnErrorNotification) {
    ShellRig rig;
    rig.editors.failing.push_back("/missing.cpp");
    ShellWindow window(rig.environment());
    EXPECT_FALSE(window.openFile("/missing.cpp").has_value());
    ASSERT_EQ(window.notifications().log().size(), 1U);
    EXPECT_EQ(window.notifications().log().back().notification.kind, frost::ui::NotificationKind::Error);
}

TEST(ShellWindow, ToolWindowsToggleAndRememberTheirSize) {
    ShellRig rig;
    {
        ShellWindow window(rig.environment());
        frost::ui::test::showAndWait(window);
        frost::ui::ToolWindowManager& windows = window.toolWindows();
        EXPECT_EQ(window.invoke(actions::kProjectWindow), frost::input::InvokeResult::Ran);
        EXPECT_TRUE(windows.isVisible(tool_windows::kProject));
        EXPECT_TRUE(windows.stripeButton(tool_windows::kProject)->isChecked());
        EXPECT_NE(windows.content(tool_windows::kProject), nullptr); // Created on first show.
        windows.setSize(tool_windows::kProject, 333);
        windows.hide(tool_windows::kProject);
        EXPECT_FALSE(windows.isVisible(tool_windows::kProject));
        EXPECT_FALSE(windows.stripeButton(tool_windows::kProject)->isChecked());
        // The stripe button toggles it back, with the remembered size.
        windows.stripeButton(tool_windows::kProject)->click();
        EXPECT_TRUE(windows.isVisible(tool_windows::kProject));
        EXPECT_EQ(windows.size(tool_windows::kProject), 333);
        QApplication::processEvents();
        EXPECT_EQ(windows.horizontalSplitter()->sizes().front(), 333);

        // A drag of the splitter is read back into the model.
        QList<int> sizes = windows.horizontalSplitter()->sizes();
        sizes[1] += sizes[0] - 250;
        sizes[0] = 250;
        windows.horizontalSplitter()->setSizes(sizes);
        windows.syncSizesFromSplitters();
        EXPECT_EQ(windows.size(tool_windows::kProject), 250);
    }
    const frost::ui::ToolWindowLayout* stored = rig.settings.current().find(tool_windows::kProject);
    ASSERT_NE(stored, nullptr);
    EXPECT_TRUE(stored->visible);
    EXPECT_EQ(stored->size, 250);

    // A new window over the same settings restores visibility and size.
    ShellWindow again(rig.environment());
    EXPECT_TRUE(again.toolWindows().isVisible(tool_windows::kProject));
    EXPECT_EQ(again.toolWindows().size(tool_windows::kProject), 250);
}

TEST(ShellWindow, HideAllAndJumpToLast) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    frost::ui::ToolWindowManager& windows = window.toolWindows();
    windows.show(tool_windows::kProject, false);
    windows.show(tool_windows::kRun, false);
    EXPECT_EQ(window.invoke(actions::kHideAllWindows), frost::input::InvokeResult::Ran);
    EXPECT_FALSE(windows.isVisible(tool_windows::kProject));
    EXPECT_FALSE(windows.isVisible(tool_windows::kRun));
    EXPECT_EQ(window.invoke(actions::kJumpToLastWindow), frost::input::InvokeResult::Ran);
    EXPECT_TRUE(windows.isVisible(tool_windows::kProject));
    EXPECT_TRUE(windows.isVisible(tool_windows::kRun));
}

TEST(ShellWindow, LayoutStoreFailureIsReported) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    rig.settings.failNextStore(frost::ErrorCode::IoError);
    window.toolWindows().show(tool_windows::kProblems, false);
    ASSERT_FALSE(window.notifications().log().empty());
    EXPECT_EQ(window.notifications().log().back().notification.kind, frost::ui::NotificationKind::Warning);
}

TEST(ShellWindow, ToolWindowContentCanBeReplaced) {
    // W1's Solution Explorer replaces the Project placeholder.
    ShellRig rig;
    ShellWindow window(rig.environment());
    auto* explorer = new QLabel(QStringLiteral("Solution Explorer"));
    window.toolWindows().setContent(tool_windows::kProject, explorer);
    window.toolWindows().show(tool_windows::kProject, false);
    EXPECT_EQ(window.toolWindows().content(tool_windows::kProject), explorer);
}

TEST(ShellWindow, GoToLinePopupMovesTheCaretAndClosesOnEscape) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    frost::ui::test::showAndWait(window);
    EXPECT_EQ(window.showGoToLine(), nullptr); // No editor yet.
    ASSERT_TRUE(window.openFile("/src/a.cpp").has_value());
    frost::ui::test::FakeEditor* editor = rig.editors.last;

    frost::ui::InputPopup* popup = window.showGoToLine();
    ASSERT_NE(popup, nullptr);
    EXPECT_TRUE(popup->isOpen());
    EXPECT_EQ(window.activePopup(), popup);
    popup->field()->setText(QStringLiteral("abc"));
    popup->submit();
    EXPECT_TRUE(popup->isOpen()); // Invalid input keeps it open with a message.
    EXPECT_FALSE(popup->errorLabel()->isHidden());
    popup->field()->setText(QStringLiteral(" 42 : 7 "));
    sendKey(popup->field(), Qt::Key_Return, Qt::NoModifier, false);
    ASSERT_EQ(editor->gotoCalls.size(), 1U);
    EXPECT_EQ(editor->gotoCalls.front(), std::make_pair(42, 7));
    EXPECT_EQ(window.statusWidgets().caret()->text(), QStringLiteral("42:7"));

    // Escape cancels (through the dispatcher, which passes it on in a popup).
    frost::ui::InputPopup* second = window.showGoToLine();
    ASSERT_NE(second, nullptr);
    std::optional<frost::ui::Popup::CloseReason> reason;
    QObject::connect(second, &frost::ui::Popup::closed, [&reason](frost::ui::Popup::CloseReason r) { reason = r; });
    sendKey(second->field(), Qt::Key_Escape, Qt::NoModifier, false);
    ASSERT_TRUE(reason.has_value());
    EXPECT_EQ(*reason, frost::ui::Popup::CloseReason::Cancelled);
    EXPECT_EQ(editor->gotoCalls.size(), 1U);
}

TEST(ShellWindow, SwitcherSelectsThePreviousTabAndAcceptsOnCtrlRelease) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    frost::ui::test::showAndWait(window);
    ASSERT_TRUE(window.openFile("/src/a.cpp").has_value());
    ASSERT_TRUE(window.openFile("/src/b.cpp").has_value());
    ASSERT_TRUE(window.openFile("/src/c.cpp").has_value());
    EXPECT_EQ(window.tabs().currentEditor()->path(), "/src/c.cpp");

    frost::ui::ListPopup* switcher = window.showSwitcher(false);
    ASSERT_NE(switcher, nullptr);
    EXPECT_EQ(switcher->visibleCount(), 3);
    EXPECT_EQ(switcher->currentItem(), 1); // MRU order: c, b, a; the previous tab is selected.
    // Ctrl+Tab again moves on.
    EXPECT_EQ(window.showSwitcher(false), switcher);
    EXPECT_EQ(switcher->currentItem(), 2);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    QApplication::sendEvent(switcher->list(), &release);
    EXPECT_FALSE(switcher->isOpen());
    EXPECT_EQ(window.tabs().currentEditor()->path(), "/src/a.cpp");
}

TEST(ShellWindow, PseudoLocalizationReachesShellStrings) {
    frost::i18n::setPseudoLocalization(true);
    {
        ShellRig rig;
        ShellWindow window(rig.environment());
        EXPECT_TRUE(window.statusBar()->currentMessage().startsWith(QLatin1Char('[')));
        const QString file = window.menus().menu("&File")->title();
        EXPECT_TRUE(file.startsWith(QStringLiteral("[&F"))) << file.toStdString();
        // Action names and key names are pseudo-localized too; the shortcut column keeps its tab.
        const QString project = window.menus().action(actions::kProjectWindow)->text();
        EXPECT_TRUE(project.startsWith(QLatin1Char('['))) << project.toStdString();
        EXPECT_TRUE(project.contains(QLatin1Char('\t'))) << project.toStdString();
    }
    frost::i18n::setPseudoLocalization(false);
}

TEST(GoToLine, ParsesLineAndColumn) {
    using frost::ui::LinePosition;
    using frost::ui::parseGoToLine;
    EXPECT_EQ(parseGoToLine(QStringLiteral("12")), (LinePosition{12, 1}));
    EXPECT_EQ(parseGoToLine(QStringLiteral("12:5")), (LinePosition{12, 5}));
    EXPECT_EQ(parseGoToLine(QStringLiteral("  3 : 4 ")), (LinePosition{3, 4}));
    EXPECT_FALSE(parseGoToLine(QStringLiteral("")).has_value());
    EXPECT_FALSE(parseGoToLine(QStringLiteral("0")).has_value());
    EXPECT_FALSE(parseGoToLine(QStringLiteral("1:0")).has_value());
    EXPECT_FALSE(parseGoToLine(QStringLiteral("1:2:3")).has_value());
    EXPECT_FALSE(parseGoToLine(QStringLiteral("x")).has_value());
}

} // namespace
