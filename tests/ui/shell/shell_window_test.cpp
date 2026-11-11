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
#include "frost/ui/shell/navigation_bar.hpp"
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
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTimer>
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
    // Separators group the configuration, build, execution and utility controls without
    // replacing their actions.
    int separators = 0;
    for (const QAction* action : window.mainToolbar().actions()) {
        separators += action->isSeparator() ? 1 : 0;
    }
    EXPECT_GE(separators, 3);
    // The default tool windows with their stripe buttons.
    for (const std::string_view id : {tool_windows::kProject, tool_windows::kCommit, tool_windows::kRun,
                                      tool_windows::kProblems, tool_windows::kNotifications}) {
        EXPECT_TRUE(window.toolWindows().contains(id)) << id;
        ASSERT_NE(window.toolWindows().stripeButton(id), nullptr) << id;
        EXPECT_FALSE(window.toolWindows().stripeButton(id)->accessibleName().isEmpty()) << id;
        EXPECT_FALSE(window.toolWindows().isVisible(id)) << id;
    }
}

TEST(ShellWindow, ToolbarFocusFollowsVisibleDynamicControls) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    auto& toolbar = window.mainToolbar();
    QAction* runAction = nullptr;
    for (QAction* action : toolbar.actions()) {
        if (toolbar.widgetForAction(action) == toolbar.runConfigurations()) {
            runAction = action;
            break;
        }
    }
    ASSERT_NE(runAction, nullptr);
    auto* profile = new QComboBox(&toolbar);
    profile->addItem(QStringLiteral("Profile"));
    QAction* profileAction = toolbar.insertWidget(runAction, profile);
    auto* disabled = new QComboBox(&toolbar);
    disabled->setEnabled(false);
    toolbar.insertWidget(runAction, disabled);
    auto* middle = new QComboBox(&toolbar);
    middle->addItem(QStringLiteral("Intermediate target"));
    QAction* middleAction = toolbar.insertWidget(runAction, middle);
    int builds = 0;
    rig.addCounted("build.project", "Build Solution", builds);
    window.refreshActions();
    toolbar.runConfigurations()->addItem(QStringLiteral("Target"));
    window.show();
    QApplication::processEvents();
    auto order = toolbar.focusOrder();
    ASSERT_GE(order.size(), 2U);
    EXPECT_EQ(order[0], profile);
    EXPECT_EQ(order[1], middle);
    EXPECT_EQ(toolbar.projectButton()->focusPolicy(), Qt::NoFocus);
    window.invoke(actions::kFocusToolbar);
    EXPECT_EQ(QApplication::focusWidget(), profile);
    sendKey(profile, Qt::Key_Tab, Qt::NoModifier);
    EXPECT_EQ(QApplication::focusWidget(), middle);
    middleAction->setVisible(false);
    profile->setFocus();
    sendKey(profile, Qt::Key_Tab, Qt::NoModifier);
    EXPECT_EQ(QApplication::focusWidget(), toolbar.runConfigurations());
    sendKey(toolbar.runConfigurations(), Qt::Key_Tab, Qt::NoModifier);
    EXPECT_EQ(QApplication::focusWidget(), toolbar.button(frost::ui::MainToolbar::kBuild));
    sendKey(toolbar.button(frost::ui::MainToolbar::kBuild), Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(QApplication::focusWidget(), toolbar.runConfigurations());
    toolbar.removeAction(middleAction);
    QApplication::processEvents();
    profile->setFocus();
    sendKey(profile, Qt::Key_Tab, Qt::NoModifier);
    EXPECT_EQ(QApplication::focusWidget(), toolbar.runConfigurations());
    sendKey(toolbar.runConfigurations(), Qt::Key_Tab, Qt::NoModifier);
    auto* build = toolbar.button(frost::ui::MainToolbar::kBuild);
    EXPECT_EQ(QApplication::focusWidget(), build);
    sendKey(build, Qt::Key_Tab, Qt::NoModifier);
    QWidget* afterToolbar = QApplication::focusWidget();
    ASSERT_NE(afterToolbar, nullptr);
    EXPECT_FALSE(toolbar.isAncestorOf(afterToolbar));
    sendKey(afterToolbar, Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(QApplication::focusWidget(), build);
    profileAction->setVisible(false);
    order = toolbar.focusOrder();
    ASSERT_FALSE(order.empty());
    EXPECT_EQ(order[0], toolbar.runConfigurations());
    window.invoke(actions::kFocusToolbar);
    EXPECT_EQ(QApplication::focusWidget(), toolbar.runConfigurations());
}

TEST(ShellWindow, NavigationBarOpensModelFilesAndRejectsStaleMenus) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    QStandardItemModel model;
    constexpr int kPath = Qt::UserRole + 1;
    constexpr int kDir = Qt::UserRole + 2;
    auto* root = new QStandardItem(QStringLiteral("Project"));
    root->setData(true, kDir);
    model.appendRow(root);
    auto* folder = new QStandardItem(QStringLiteral("Folder"));
    folder->setData(true, kDir);
    root->appendRow(folder);
    auto* first = new QStandardItem(QStringLiteral("first.cpp"));
    first->setData(QStringLiteral("/project/first.cpp"), kPath);
    folder->appendRow(first);
    auto* second = new QStandardItem(QStringLiteral("second.cpp"));
    second->setData(QStringLiteral("/project/second.cpp"), kPath);
    folder->appendRow(second);
    std::string opened;
    auto& bar = window.navigationBar();
    bar.setModel(
        &model, kPath,
        [&model](const std::string& path) {
            const QModelIndex folderIndex = model.index(0, 0, model.index(0, 0));
            for (int row = 0; row < model.rowCount(folderIndex); ++row) {
                const QModelIndex child = model.index(row, 0, folderIndex);
                if (child.data(kPath).toString().toStdString() == path)
                    return child;
            }
            return QModelIndex();
        },
        [](const QModelIndex& index) { return index.data(kDir).toBool(); },
        [&opened](const std::string& path) { opened = path; });
    bar.setPath("/project/first.cpp");
    window.show();
    QApplication::processEvents();
    window.invoke(actions::kFocusNavigationBar);
    auto* button = qobject_cast<QToolButton*>(QApplication::focusWidget());
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->text(), QStringLiteral("Project"));
    ASSERT_NE(button->menu(), nullptr);
    sendKey(button, Qt::Key_Tab, Qt::NoModifier);
    auto* folderButton = qobject_cast<QToolButton*>(QApplication::focusWidget());
    ASSERT_NE(folderButton, nullptr);
    EXPECT_EQ(folderButton->text(), QStringLiteral("Folder"));
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("navigationScroll"));
    ASSERT_NE(scroll, nullptr);
    scroll->setFixedWidth(30);
    QApplication::processEvents();
    button->setFocus();
    folderButton->setFocus();
    QApplication::processEvents();
    EXPECT_GT(scroll->horizontalScrollBar()->value(), 0);
    scroll->setMinimumWidth(0);
    scroll->setMaximumWidth(QWIDGETSIZE_MAX);
    sendKey(folderButton, Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(QApplication::focusWidget(), button);
    folderButton->setFocus();
    bool sawPopup = false;
    QTimer::singleShot(0, &window, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu)
            return;
        sawPopup = true;
        sendKey(menu, Qt::Key_Down, Qt::NoModifier);
        sendKey(menu, Qt::Key_Down, Qt::NoModifier);
        sendKey(menu, Qt::Key_Return, Qt::NoModifier);
    });
    sendKey(folderButton, Qt::Key_Space, Qt::NoModifier);
    QElapsedTimer waitForOpen;
    waitForOpen.start();
    while (opened.empty() && waitForOpen.elapsed() < 1000) {
        QApplication::processEvents();
    }
    EXPECT_TRUE(sawPopup);
    EXPECT_EQ(opened, "/project/second.cpp");
    opened.clear();
    QTimer::singleShot(0, &window, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu)
            sendKey(menu, Qt::Key_Escape, Qt::NoModifier);
    });
    sendKey(folderButton, Qt::Key_Space, Qt::NoModifier);
    EXPECT_TRUE(opened.empty());
    QTimer::singleShot(0, &window, [&] { model.clear(); });
    sendKey(folderButton, Qt::Key_Space, Qt::NoModifier);
    QApplication::processEvents();
    EXPECT_TRUE(opened.empty());
}

TEST(ShellWindow, NavigationBarRejectsQueuedSelectionAfterReset) {
    frost::ui::NavigationBar bar;
    QStandardItemModel model;
    constexpr int kPath = Qt::UserRole + 1;
    auto* root = new QStandardItem(QStringLiteral("Project"));
    model.appendRow(root);
    auto* file = new QStandardItem(QStringLiteral("file.cpp"));
    file->setData(QStringLiteral("/project/file.cpp"), kPath);
    root->appendRow(file);
    int opened = 0;
    bar.setModel(
        &model, kPath, [&model](const std::string&) { return model.index(0, 0, model.index(0, 0)); },
        [](const QModelIndex& index) { return !index.parent().isValid(); },
        [&opened](const std::string&) { ++opened; });
    bar.setPath("/project/file.cpp");
    auto* button = bar.findChild<QToolButton*>();
    ASSERT_NE(button, nullptr);
    ASSERT_NE(button->menu(), nullptr);
    QMetaObject::invokeMethod(button->menu(), "aboutToShow", Qt::DirectConnection);
    ASSERT_EQ(button->menu()->actions().size(), 1);
    button->menu()->actions()[0]->trigger();
    model.clear();
    QApplication::processEvents();
    EXPECT_EQ(opened, 0);
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
    rig.editors.failing.emplace_back("/missing.cpp");
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

TEST(ShellWindow, ActionContextUsesTextFieldFocusForChildLineEditInsideEditorSurface) {
    ShellRig rig;
    bool editorEnterCalled = false;
    frost::input::ActionSpec enterSpec;
    enterSpec.id = "editor.enter";
    enterSpec.name = "Enter";
    enterSpec.context = {{frost::input::ContextKey::EditorFocus}, {}};
    enterSpec.handler = [&](const frost::input::ActionContext&, const frost::input::InvokeInfo&) {
        editorEnterCalled = true;
    };
    ASSERT_TRUE(rig.registry.add(std::move(enterSpec)).has_value());
    frost::input::KeymapPreferences preferences;
    preferences.overrides.push_back(
        {"editor.enter", {*frost::input::parseSequence("Enter", frost::input::SyntaxMode::Strict)}});
    ASSERT_TRUE(rig.keymap->setPreferences(preferences).has_value());

    ShellWindow window(rig.environment());
    frost::ui::test::showAndWait(window);
    ASSERT_TRUE(window.openFile("/src/a.cpp").has_value());
    frost::ui::test::FakeEditor* editor = rig.editors.last;
    ASSERT_NE(editor, nullptr);

    // A QLineEdit child inside the editor widget (like EditorFindBar or GoToLinePopup) must set
    // TextFieldFocus rather than EditorFocus, so pressing Enter never triggers editor.enter!
    auto* findInput = new QLineEdit(editor->widget());
    findInput->show();
    findInput->setFocus();
    QApplication::processEvents();
    ASSERT_EQ(QApplication::focusWidget(), findInput);

    EXPECT_TRUE(window.actionContext().keys.has(frost::input::ContextKey::TextFieldFocus));
    EXPECT_FALSE(window.actionContext().keys.has(frost::input::ContextKey::EditorFocus));

    sendKey(findInput, Qt::Key_Return, Qt::NoModifier);
    EXPECT_FALSE(editorEnterCalled);
}

TEST(ShellWindow, RiderHeaderControlsAndQuickSwitchThemeCatalog) {
    ShellRig rig;
    frost::ui::InMemoryAppearanceSettings appearanceSettings;
    frost::ui::ShellEnvironment env = rig.environment();
    env.appearanceSettings = &appearanceSettings;
    ShellWindow window(env);
    frost::ui::test::showAndWait(window);

    auto& header = window.mainToolbar();
    ASSERT_NE(header.mainMenuButton(), nullptr);
    ASSERT_NE(header.backButton(), nullptr);
    ASSERT_NE(header.forwardButton(), nullptr);
    ASSERT_NE(header.projectButton(), nullptr);
    ASSERT_NE(header.themeButton(), nullptr);
    ASSERT_NE(header.minimizeButton(), nullptr);
    ASSERT_NE(header.maximizeButton(), nullptr);
    ASSERT_NE(header.closeButton(), nullptr);

    // Header utility/caption controls keep Qt::NoFocus so Tab focus order stays on dynamic slots.
    EXPECT_EQ(header.mainMenuButton()->focusPolicy(), Qt::NoFocus);
    EXPECT_EQ(header.backButton()->focusPolicy(), Qt::NoFocus);
    EXPECT_EQ(header.forwardButton()->focusPolicy(), Qt::NoFocus);
    EXPECT_EQ(header.themeButton()->focusPolicy(), Qt::NoFocus);
    EXPECT_EQ(header.minimizeButton()->focusPolicy(), Qt::NoFocus);
    EXPECT_EQ(header.maximizeButton()->focusPolicy(), Qt::NoFocus);
    EXPECT_EQ(header.closeButton()->focusPolicy(), Qt::NoFocus);

    // Left sidebar houses both top-left and bottom-left stripes plus the More Tool Windows (⋯) button.
    ASSERT_NE(window.toolWindows().moreToolWindowsButton(), nullptr);
    ASSERT_NE(window.toolWindows().moreToolWindowsButton()->menu(), nullptr);
    QMetaObject::invokeMethod(window.toolWindows().moreToolWindowsButton()->menu(), "aboutToShow",
                              Qt::DirectConnection);
    EXPECT_GE(window.toolWindows().moreToolWindowsButton()->menu()->actions().size(), 8);

    // NavigationBar breadcrumbs live inside the bottom StatusBar on the left like Rider New UI.
    auto* navScroll = window.statusBar()->findChild<QWidget*>(QStringLiteral("navigationScroll"));
    EXPECT_NE(navScroll, nullptr);

    // Main Menu popup populates top-level menus and the Show Classic Menu Bar toggle.
    QMenu* mainMenu = header.mainMenuButton()->menu();
    ASSERT_NE(mainMenu, nullptr);
    QMetaObject::invokeMethod(mainMenu, "aboutToShow", Qt::DirectConnection);
    EXPECT_GE(mainMenu->actions().size(), 3);

    // Clicking the header's Quick Switch Theme button opens the theme switcher popup with all 34 themes + Sync with OS.
    header.themeButton()->click();
    auto* popup = qobject_cast<frost::ui::ListPopup*>(window.activePopup());
    ASSERT_NE(popup, nullptr);
    EXPECT_EQ(popup->visibleCount(), 35);
    EXPECT_NE(popup->findChild<QLabel*>(QStringLiteral("popupFooter")), nullptr);

    // Refactor and Run/Debug actions are wired in ShellMenus.
    EXPECT_NE(window.menus().menu("&Refactor"), nullptr);
    EXPECT_NE(window.menus().action("refactor.rename"), nullptr);
    EXPECT_NE(window.menus().action("debug.toggleBreakpoint"), nullptr);
    EXPECT_NE(window.menus().action("debug.stepOver"), nullptr);

    // Tool window headers expose Rider's Options (⋮) menu alongside Hide.
    window.toolWindows().show(tool_windows::kProject, false);
    auto* leftPanel = window.findChild<QWidget*>(QStringLiteral("toolWindowPanel.left"));
    ASSERT_NE(leftPanel, nullptr);
    auto* optionsBtn = leftPanel->findChild<QToolButton*>(QStringLiteral("toolWindowOptions"));
    ASSERT_NE(optionsBtn, nullptr);
    ASSERT_NE(optionsBtn->menu(), nullptr);
    QMetaObject::invokeMethod(optionsBtn->menu(), "aboutToShow", Qt::DirectConnection);
    EXPECT_GE(optionsBtn->menu()->actions().size(), 4);
}

} // namespace
