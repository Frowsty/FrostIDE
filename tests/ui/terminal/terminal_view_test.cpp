// SPDX-License-Identifier: Apache-2.0
//
// Offscreen tests of frost::ui_terminal over frost::term's FakePtyLauncher: the Terminal tool window
// in a ShellWindow (window.terminal, tabs with +, close and rename, a tab closing when its shell
// exits), typing, IME commits and pastes reaching the terminal, the keys the terminal keeps from the
// IDE's keymap, the output on the screen, selection copy, the wheel's scrollback and the trust rule.

#include "frost/term/fake_pty.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/terminal/terminal_view.hpp"

#include "shell/shell_rig.hpp"
#include <QApplication>
#include <QClipboard>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTabBar>
#include <QToolButton>
#include <QWheelEvent>
#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace {

using namespace std::chrono_literals;
using frost::term::FakePty;
using frost::term::FakePtyLauncher;
using frost::ui::ShellWindow;
using frost::ui::TerminalContext;
using frost::ui::TerminalEnvironment;
using frost::ui::TerminalIntegration;
using frost::ui::TerminalToolWindow;
using frost::ui::TerminalView;
using frost::ui::test::ShellRig;
using frost::ui::test::showAndWait;
using frost::ui::test::waitFor;
namespace terminal_actions = frost::ui::terminal_actions;

#ifdef _WIN32
constexpr const char* kShell = "C:\\Tools\\fake-shell.exe";
constexpr const char* kHome = "C:\\Users\\dev";
constexpr const char* kRoot = "D:\\work\\project";
#else
constexpr const char* kShell = "/opt/fake/shell";
constexpr const char* kHome = "/home/dev";
constexpr const char* kRoot = "/work/project";
#endif

// A terminal environment over `launcher` with a scripted home directory and `kShell` configured.
TerminalEnvironment fakeEnvironment(FakePtyLauncher& launcher, std::optional<TerminalContext> context = {}) {
    TerminalEnvironment env;
    env.launcher = &launcher;
    env.settings = [] { return frost::term::TerminalSettings{kShell, 1000}; };
    env.probe.environment = [](std::string_view name) -> std::optional<std::string> {
        if (name == "HOME" || name == "USERPROFILE") {
            return std::string(kHome);
        }
        return std::nullopt;
    };
    env.probe.isExecutable = [](const std::string& path) { return path == kShell; };
    if (context) {
        env.context = [context] { return *context; };
    }
    return env;
}

void sendKey(QWidget* target, int key, Qt::KeyboardModifiers modifiers, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QApplication::sendEvent(target, &release);
}

// A shell window with the terminal docked and shown, its first tab open.
struct TerminalRig {
    TerminalRig() : window(rig.environment()) {
        showAndWait(window);
        integration = std::make_unique<TerminalIntegration>(window, fakeEnvironment(launcher));
        EXPECT_EQ(window.invoke(terminal_actions::kTerminalWindow), frost::input::InvokeResult::Ran);
        EXPECT_TRUE(waitFor([this] { return launcher.count() == 1 && toolWindow() != nullptr; }));
    }
    [[nodiscard]] TerminalToolWindow* toolWindow() const { return integration->toolWindow(); }
    [[nodiscard]] TerminalView* view() const { return toolWindow()->currentView(); }
    [[nodiscard]] std::shared_ptr<FakePty> pty(std::size_t index = 0) const { return launcher.pty(index); }

    ShellRig rig;
    FakePtyLauncher launcher;
    ShellWindow window;
    std::unique_ptr<TerminalIntegration> integration;
};

TEST(TerminalToolWindow, AltF12ActionOpensTheBottomWindowWithOneTab) {
    TerminalRig t;
    ASSERT_NE(t.toolWindow(), nullptr);
    EXPECT_TRUE(t.window.toolWindows().isVisible(frost::ui::tool_windows::kTerminal));
    EXPECT_EQ(t.toolWindow()->count(), 1);
    EXPECT_EQ(t.toolWindow()->tabName(0), QStringLiteral("Local"));
    EXPECT_EQ(t.launcher.specs()[0].executable, kShell);
    EXPECT_EQ(t.launcher.specs()[0].workingDirectory, kHome); // No project.
    EXPECT_TRUE(waitFor([&t] { return t.view()->hasFocus(); }));
    // The grid follows the widget: the terminal was opened (and resized) to the view's size.
    EXPECT_EQ(t.pty()->size(), t.view()->gridSize());
}

TEST(TerminalToolWindow, KittyModeForwardsShiftEnter) {
    TerminalRig t;
    t.pty()->print("\x1b[>5uREADY");
    ASSERT_TRUE(waitFor([&t] {
        return t.view()->session()->read(
            [](const frost::term::VtScreen& s) { return s.screenText().find("READY") != std::string::npos; });
    }));
    sendKey(t.view(), Qt::Key_Return, Qt::ShiftModifier, QStringLiteral("\r"));
    EXPECT_TRUE(t.pty()->waitForInput("\x1b[13;2u", 5s)) << t.pty()->input();
}

#ifdef _WIN32
TEST(TerminalToolWindow, Win32ModeForwardsNativeShiftEnterPressAndRelease) {
    TerminalRig t;
    t.pty()->print("\x1b[?9001hREADY");
    ASSERT_TRUE(waitFor([&t] {
        return t.view()->session()->read(
            [](const frost::term::VtScreen& s) { return s.screenText().find("READY") != std::string::npos; });
    }));
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier, 28, 13, 0, QStringLiteral("\r"));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::ShiftModifier, 28, 13, 0, QStringLiteral("\r"));
    QApplication::sendEvent(t.view(), &press);
    QApplication::sendEvent(t.view(), &release);
    EXPECT_TRUE(t.pty()->waitForInput("\x1b[13;28;13;1;16;1_\x1b[13;28;13;0;16;1_", 5s)) << t.pty()->input();
}
#endif

#ifdef _WIN32
TEST(TerminalToolWindow, Win32ModePreservesAllUtf16Units) {
    TerminalRig t;
    t.pty()->print("\x1b[?9001hREADY");
    ASSERT_TRUE(waitFor([&t] {
        return t.view()->session()->read(
            [](const frost::term::VtScreen& s) { return s.screenText().find("READY") != std::string::npos; });
    }));
    const QString text = QString::fromUtf8("\xf0\x9f\x98\x80"
                                           "ab");
    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 30, 65, 0, text);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, 30, 65, 0, text);
    QApplication::sendEvent(t.view(), &press);
    QApplication::sendEvent(t.view(), &release);
    const std::string expected = "\x1b[65;30;55357;1;0;1_\x1b[65;30;56832;1;0;1_"
                                 "\x1b[65;30;97;1;0;1_\x1b[65;30;98;1;0;1_"
                                 "\x1b[65;30;55357;0;0;1_\x1b[65;30;56832;0;0;1_"
                                 "\x1b[65;30;97;0;0;1_\x1b[65;30;98;0;0;1_";
    EXPECT_TRUE(t.pty()->waitForInput(expected, 5s)) << t.pty()->input();
}

TEST(TerminalToolWindow, NativeModifierSidesComeFromTheEvent) {
    TerminalRig t;
    t.pty()->print("\x1b[?9001hREADY");
    ASSERT_TRUE(waitFor([&t] {
        return t.view()->session()->read(
            [](const frost::term::VtScreen& s) { return s.screenText().find("READY") != std::string::npos; });
    }));
    // Qt's recorded right-control bit is 0x20, independent of live keyboard state.
    QKeyEvent press(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier, 45, 88, 0x20, QStringLiteral("x"));
    QApplication::sendEvent(t.view(), &press);
    EXPECT_TRUE(t.pty()->waitForInput("\x1b[88;45;120;1;4;1_", 5s)) << t.pty()->input();
}

TEST(TerminalToolWindow, NativeKeysAreReleasedOnFocusLoss) {
    TerminalRig t;
    t.pty()->print("\x1b[?9001hREADY");
    ASSERT_TRUE(waitFor([&t] {
        return t.view()->session()->read(
            [](const frost::term::VtScreen& s) { return s.screenText().find("READY") != std::string::npos; });
    }));
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier, 28, 13, 0, QStringLiteral("\r"));
    QApplication::sendEvent(t.view(), &press);
    QFocusEvent lost(QEvent::FocusOut);
    QApplication::sendEvent(t.view(), &lost);
    EXPECT_TRUE(t.pty()->waitForInput("\x1b[13;28;13;1;16;1_\x1b[13;28;0;0;0;1_", 5s)) << t.pty()->input();
}
#endif

TEST(TerminalToolWindow, TabsOpenCloseAndRename) {
    TerminalRig t;
    TerminalToolWindow* window = t.toolWindow();
    window->findChild<QToolButton*>(QStringLiteral("terminalNewTab"))->click();
    ASSERT_EQ(window->count(), 2);
    EXPECT_EQ(window->tabName(1), QStringLiteral("Local (2)"));
    EXPECT_EQ(window->currentIndex(), 1);
    EXPECT_EQ(t.launcher.count(), 2U);

    window->renameTab(1, QStringLiteral("build"));
    EXPECT_EQ(window->tabBar()->tabText(1), QStringLiteral("build"));
    window->renameTab(1, QString());
    EXPECT_EQ(window->tabName(1), QStringLiteral("Local (2)"));

    // In-place rename through the action.
    EXPECT_EQ(t.window.invoke(terminal_actions::kRenameTab), frost::input::InvokeResult::Ran);
    auto* editor = window->findChild<QLineEdit*>(QStringLiteral("terminalRenameEditor"));
    ASSERT_NE(editor, nullptr);
    editor->setText(QStringLiteral("server"));
    sendKey(editor, Qt::Key_Return, Qt::NoModifier);
    EXPECT_EQ(window->tabName(1), QStringLiteral("server"));

    const auto second = t.pty(1);
    EXPECT_EQ(t.window.invoke(terminal_actions::kCloseTab), frost::input::InvokeResult::Ran);
    EXPECT_EQ(window->count(), 1);
    EXPECT_TRUE(second->closed()); // Closing the tab ended its shell.
    EXPECT_EQ(t.window.invoke(terminal_actions::kNewTab), frost::input::InvokeResult::Ran);
    EXPECT_EQ(window->count(), 2);
}

TEST(TerminalToolWindow, ATabClosesWhenItsShellExits) {
    TerminalRig t;
    t.pty()->exit(0);
    EXPECT_TRUE(waitFor([&t] { return t.toolWindow()->count() == 0; }));
    EXPECT_FALSE(t.toolWindow()->message().isEmpty());
}

TEST(TerminalToolWindow, OpenInTerminalStartsInTheFolder) {
    TerminalRig t;
    const std::string folder = std::string(kHome) + (kHome[0] == '/' ? "/src" : "\\src");
    auto opened = t.integration->openInTerminal(folder);
    ASSERT_TRUE(opened.has_value()) << opened.error().message();
    EXPECT_EQ(t.launcher.specs().back().workingDirectory, folder);
}

TEST(TerminalToolWindow, UntrustedProjectStartsInTheHomeDirectory) {
    FakePtyLauncher launcher;
    TerminalToolWindow tool(fakeEnvironment(launcher, TerminalContext{kRoot, false}));
    ASSERT_TRUE(tool.newTab().has_value());
    EXPECT_EQ(launcher.specs()[0].workingDirectory, kHome);
#ifdef _WIN32
    bool hardened = false;
    for (const auto& [name, value] : launcher.specs()[0].environment) {
        hardened =
            hardened || (name == "NoDefaultCurrentDirectoryInExePath" && value == std::optional<std::string>("1"));
    }
    EXPECT_TRUE(hardened);
#endif
    TerminalToolWindow trusted(fakeEnvironment(launcher, TerminalContext{kRoot, true}));
    ASSERT_TRUE(trusted.newTab().has_value());
    EXPECT_EQ(launcher.specs()[1].workingDirectory, kRoot);
}

TEST(TerminalView, TypingReachesTheTerminal) {
    TerminalRig t;
    TerminalView* view = t.view();
    sendKey(view, Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
    sendKey(view, Qt::Key_S, Qt::NoModifier, QStringLiteral("s"));
    sendKey(view, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    sendKey(view, Qt::Key_Up, Qt::NoModifier);
    sendKey(view, Qt::Key_Right, Qt::ControlModifier);
    EXPECT_TRUE(t.pty()->waitForInput("ls\r\x1b[A\x1b[1;5C", 5s)) << t.pty()->input();

    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("\xe4\xb8\xad"));
    QApplication::sendEvent(view, &commit);
    EXPECT_TRUE(t.pty()->waitForInput("\xe4\xb8\xad", 5s));
}

TEST(TerminalView, TerminalKeepsItsKeysFromTheIdeKeymap) {
    TerminalRig t;
    TerminalView* view = t.view();
    // Escape is window.focusEditor and Ctrl+E Recent Files elsewhere; here they reach the shell.
    sendKey(view, Qt::Key_Escape, Qt::NoModifier, QStringLiteral("\x1b"));
    sendKey(view, Qt::Key_E, Qt::ControlModifier, QStringLiteral("\x05"));
    sendKey(view, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03"));
    sendKey(view, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
    EXPECT_TRUE(t.pty()->waitForInput("\x1b\x05\x03\t", 5s)) << t.pty()->input();
    EXPECT_TRUE(view->hasFocus());

    QKeyEvent kept(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
    kept.ignore();
    QApplication::sendEvent(view, &kept);
    EXPECT_TRUE(kept.isAccepted());

    // Alt+F12 stays the IDE's: the visible, focused terminal hides.
    sendKey(view, Qt::Key_F12, Qt::AltModifier);
    EXPECT_FALSE(t.window.toolWindows().isVisible(frost::ui::tool_windows::kTerminal));
}

TEST(TerminalView, OutputIsShownAndSelectionCopies) {
    TerminalRig t;
    TerminalView* view = t.view();
    t.pty()->print("hello world\r\nsecond line");
    ASSERT_TRUE(waitFor([&] {
        return view->session()->read([](const frost::term::VtScreen& s) { return s.lineText(1); }) == "second line";
    }));
    view->repaint(); // Painting the grid must not fail.

    view->select(frost::term::ScreenPos{0, 6}, frost::term::ScreenPos{1, 5});
    EXPECT_EQ(view->selectedText(), QStringLiteral("world\nsecond"));
    QApplication::clipboard()->clear();
    // Ctrl+C with a selection copies instead of interrupting.
    sendKey(view, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03"));
    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("world\nsecond"));
    EXPECT_FALSE(view->hasSelection());
    EXPECT_EQ(t.pty()->input().find('\x03'), std::string::npos);

    view->select(frost::term::ScreenPos{0, 0}, frost::term::ScreenPos{0, 4});
    sendKey(view, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("hello"));

    QApplication::clipboard()->setText(QStringLiteral("echo pasted\n"));
    sendKey(view, Qt::Key_V, Qt::ControlModifier, QStringLiteral("\x16"));
    EXPECT_TRUE(t.pty()->waitForInput("echo pasted\r", 5s)) << t.pty()->input();
}

TEST(TerminalView, WheelScrollsTheScrollback) {
    TerminalRig t;
    TerminalView* view = t.view();
    std::string lines;
    for (int i = 0; i < 200; ++i) {
        lines += "line " + std::to_string(i) + "\r\n";
    }
    t.pty()->print(lines);
    ASSERT_TRUE(waitFor([&] {
        return view->session()->read([](const frost::term::VtScreen& s) { return s.scrollbackSize(); }) > 100;
    }));
    QApplication::processEvents();
    const QPointF centre(view->width() / 2.0, view->height() / 2.0);
    QWheelEvent up(centre, view->mapToGlobal(centre), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                   Qt::NoScrollPhase, false);
    QApplication::sendEvent(view, &up);
    EXPECT_EQ(view->scrollOffset(), 3);
    // Typing returns to the live screen.
    sendKey(view, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    EXPECT_EQ(view->scrollOffset(), 0);
}

} // namespace
