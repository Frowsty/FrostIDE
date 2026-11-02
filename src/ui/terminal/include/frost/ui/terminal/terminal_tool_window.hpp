// SPDX-License-Identifier: Apache-2.0
//
// The Terminal tool window (ADR 0051): tabs of TerminalViews, one shell session each, with a "+"
// button, close buttons and in-place renaming; and TerminalIntegration, which docks it at the bottom
// of a ShellWindow (Alt+F12, `window.terminal`) and registers the terminal's actions.
#pragma once

#include "frost/core/error.hpp"
#include "frost/term/pty.hpp"
#include "frost/term/shell.hpp"
#include "frost/ui/terminal/terminal_colors.hpp"

#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QLabel;
class QLineEdit;
class QStackedWidget;
class QTabBar;
class QToolButton;

namespace frost::ui {

class AppearanceManager;
class ShellWindow;
class TerminalView;

// The project a terminal tab starts for (the trust rule of frost/term/shell.hpp).
struct TerminalContext {
    // Absolute, "" without a project.
    std::string projectRoot;
    bool trusted = false;
};

// What the terminal needs. The launcher (and the appearance manager, when set) must outlive every
// TerminalToolWindow made from it.
struct TerminalEnvironment {
    term::IPtyLauncher* launcher = nullptr;
    // False prevents launching after the owner has shut down; empty means available.
    std::function<bool()> available{};
    // The open project now; empty: no project.
    std::function<TerminalContext()> context{};
    // terminal.shell and terminal.scrollbackLines now; empty: the registry defaults.
    std::function<term::TerminalSettings()> settings{};
    // How the default shell and the home directory are found; empty functions: the system's.
    term::ShellProbe probe{};
    // The theme (followed while it changes); null: TerminalColors::defaults().
    AppearanceManager* appearance = nullptr;
    // The editor font; an empty family: the registry defaults of editor.fontFamily and editor.fontSize.
    QString fontFamily{};
    double fontSize = 0;
};

// The ids of the terminal's actions; `window.terminal` keeps the keymap's IntelliJ-compatible id.
namespace terminal_actions {
inline constexpr std::string_view kTerminalWindow = "window.terminal";
inline constexpr std::string_view kNewTab = "terminal.newTab";
inline constexpr std::string_view kCloseTab = "terminal.closeTab";
inline constexpr std::string_view kRenameTab = "terminal.renameTab";
inline constexpr std::string_view kOpenInTerminal = "terminal.openInTerminal";
} // namespace terminal_actions

/*
 * The tool window's content. The first time it is shown without tabs it opens one. A tab is named
 * "Local", "Local (2)", ... until renamed; its tooltip is the title the program set. A tab whose
 * shell exits closes; when a shell cannot start, the window shows why. GUI thread only.
 */
class TerminalToolWindow final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalToolWindow(TerminalEnvironment environment, QWidget* parent = nullptr);
    ~TerminalToolWindow() override;
    TerminalToolWindow(const TerminalToolWindow&) = delete ("QObject instances have identity and cannot be copied");
    TerminalToolWindow&
    operator=(const TerminalToolWindow&) = delete ("QObject instances have identity and cannot be copied");

    /*
     * Opens a tab running the shell (frost::term::planTerminalLaunch over the environment's context
     * and settings) in `workingDirectory` ("" for the project root), makes it current and focuses it.
     * Returns its index. Errors: those of planning and starting; the message is also shown.
     */
    [[nodiscard]] Result<int> newTab(const std::string& workingDirectory = {});
    // Closes tab `index` (ending its shell's tree). Out of range: nothing.
    void closeTab(int index);
    // Closes every tab.
    void closeAllTabs();
    // Stops existing sessions and prevents queued or future tab launches during teardown.
    void shutdown();
    // Renames tab `index`; an empty name restores the default.
    void renameTab(int index, const QString& name);
    // Starts renaming the current tab in place (Enter applies, Escape cancels).
    void beginRename();

    [[nodiscard]] int count() const;
    [[nodiscard]] int currentIndex() const;
    void setCurrentIndex(int index);
    [[nodiscard]] TerminalView* view(int index) const;
    [[nodiscard]] TerminalView* currentView() const;
    [[nodiscard]] QString tabName(int index) const;
    [[nodiscard]] QTabBar* tabBar() const noexcept { return m_tabs; }
    // The message shown when there is no tab ("" when none).
    [[nodiscard]] QString message() const;

signals:
    void tabsChanged();

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Tab {
        QPointer<TerminalView> view;
        QString defaultName;
        QString customName;
    };
    void applyAppearance(TerminalView& view) const;
    void refreshTab(int index);
    void showMessage(const QString& text);
    [[nodiscard]] int indexOf(const TerminalView* view) const;
    void finishRename(bool apply);

    TerminalEnvironment m_env;
    QTabBar* m_tabs = nullptr;
    QToolButton* m_newTab = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_message = nullptr;
    QLineEdit* m_renameEditor = nullptr;
    int m_renaming = -1;
    std::vector<Tab> m_tabState;
    int m_nextNumber = 1;
    bool m_openedOnce = false;
    bool m_stopped = false;
};

/*
 * Docks the Terminal tool window (id "terminal") at the bottom of `shell` and registers
 * window.terminal (show and focus, or hide), terminal.newTab, terminal.closeTab, terminal.renameTab
 * and terminal.openInTerminal (the folder `folderProvider` returns, the project root by default).
 * Destroy it before the shell's teardown; the actions are inert afterwards. GUI thread only.
 */
class TerminalIntegration final : public QObject {
    Q_OBJECT

public:
    TerminalIntegration(ShellWindow& shell, TerminalEnvironment environment, QObject* parent = nullptr);
    ~TerminalIntegration() override;
    TerminalIntegration(const TerminalIntegration&) = delete ("the shell's actions refer to it");
    TerminalIntegration& operator=(const TerminalIntegration&) = delete ("the shell's actions refer to it");

    // Shows the terminal and opens a tab in `folder` (absolute). Errors: those of newTab.
    [[nodiscard]] Result<int> openInTerminal(const std::string& folder);
    // The folder terminal.openInTerminal uses ("" for the project root).
    void setFolderProvider(std::function<std::string()> provider) { m_folderProvider = std::move(provider); }
    // The tool window's content, or nullptr before it was first shown.
    [[nodiscard]] TerminalToolWindow* toolWindow() const;

private:
    void addAction(std::string_view id, std::string_view name, std::function<void()> run,
                   std::function<bool()> enabled = {});
    // Shows the tool window (creating its content) and returns the content.
    TerminalToolWindow* showToolWindow(bool focus);

    ShellWindow& m_shell;
    std::function<std::string()> m_folderProvider;
};

} // namespace frost::ui
