// SPDX-License-Identifier: Apache-2.0
//
// ShellWindow: FrostIDE's main window shell (PLAN.md rows 55, 219, 239-242; ADR 0038), Rider's
// layout on the native window frame: the menu bar built from the action registry, the main toolbar,
// the editor tabs in the centre island, tool windows docked around it, the status bar widgets,
// balloons and background tasks, the popups, and the shortcut dispatcher that routes keys to
// frost::input. The composition root (src/app) supplies the services through ShellEnvironment.
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/ui/model/appearance_settings.hpp"
#include "frost/ui/shell/editor_host.hpp"
#include "frost/ui/shell/shell_settings.hpp"

#include <QMainWindow>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QTimer;

namespace frost::ui {

class AppearanceManager;
class BackgroundTaskManager;
class EditorTabs;
class InputPopup;
class ListPopup;
class MainToolbar;
class NavigationBar;
class NotificationCenter;
class Popup;
class ShellMenus;
class ShellStatusBar;
class ShortcutDispatcher;
class ToolWindowManager;

/*
 * The services a shell window uses. Every reference must outlive the window (more precisely, its
 * teardown(), which subclasses call first in their destructors). One window per registry: the
 * window registers its actions (frost/ui/shell/shell_actions.hpp) in the constructor, and the
 * registry has no removal.
 */
struct ShellEnvironment {
    input::ActionRegistry& registry;
    input::KeymapService& keymap;
    IShellSettings& settings;
    IEditorFactory& editors;
    // Optional: IDE zoom, compact mode and Quick Switch Theme need both.
    AppearanceManager* appearance = nullptr;
    IAppearanceSettings* appearanceSettings = nullptr;
    // Re-read the OS keyboard layout on KeyboardLayoutChange (off with simulated layouts).
    bool followActiveLayout = false;
};

// A 1-based Go to Line target.
struct LinePosition {
    int line = 1;
    int column = 1;
    friend bool operator==(const LinePosition&, const LinePosition&) = default;
};

// Parses Go to Line input: "line" or "line:column" (spaces allowed around the parts; both at
// least 1). nullopt for anything else. O(n).
[[nodiscard]] std::optional<LinePosition> parseGoToLine(const QString& text);

/*
 * The window. Created hidden; GUI thread only. Its title is "<project> - FrostIDE" while a project is
 * open (setProject), else "<file> - FrostIDE" while an editor is active, else "FrostIDE". Destruction: teardown()
 * removes the dispatcher and closes every editor and popup while the services still exist; ~ShellWindow calls it, and a
 * subclass that owns services must call it first in its own destructor.
 */
class ShellWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit ShellWindow(ShellEnvironment environment, QWidget* parent = nullptr);
    ~ShellWindow() override;

    ShellWindow(const ShellWindow&) = delete ("QObject instances have identity and cannot be copied");
    ShellWindow& operator=(const ShellWindow&) = delete ("QObject instances have identity and cannot be copied");

    [[nodiscard]] EditorTabs& tabs() const noexcept { return *m_tabs; }
    [[nodiscard]] ToolWindowManager& toolWindows() const noexcept { return *m_toolWindows; }
    [[nodiscard]] NotificationCenter& notifications() const noexcept { return *m_notifications; }
    [[nodiscard]] BackgroundTaskManager& backgroundTasks() const noexcept { return *m_tasks; }
    [[nodiscard]] ShellMenus& menus() const noexcept { return *m_menus; }
    [[nodiscard]] MainToolbar& mainToolbar() const noexcept { return *m_toolbar; }
    [[nodiscard]] NavigationBar& navigationBar() const noexcept { return *m_navigationBar; }
    [[nodiscard]] ShellStatusBar& statusWidgets() const noexcept { return *m_status; }
    [[nodiscard]] ShortcutDispatcher& dispatcher() const noexcept { return *m_dispatcher; }
    [[nodiscard]] const ShellEnvironment& environment() const noexcept { return m_env; }

    // The context actions are gated on now: the focus keys (editor, tool window, popup, dialog,
    // text field), ProjectOpen, and the live availability level and trust state.
    [[nodiscard]] input::ActionContext actionContext() const;
    // Runs `actionId` through the registry with the current context. Unregistered ids are
    // Disabled.
    input::InvokeResult invoke(std::string_view actionId, input::InvokeSource source = input::InvokeSource::Menu);
    // Re-reads the registry and the keymap into the menus and the toolbar (also runs on a timer
    // while the window is visible).
    void refreshActions();

    void setAvailabilityLevel(input::AvailabilityLevel level) noexcept { m_level = level; }
    void setProjectTrusted(bool trusted) noexcept { m_trusted = trusted; }
    // Shows `name` in the toolbar's project widget and the title; empty: no project (ProjectOpen off).
    // Whether the open project is trusted (the terminal's trust rule, ADR 0051, reads it).
    [[nodiscard]] bool projectTrusted() const noexcept { return m_trusted; }
    // Shows `name` in the toolbar's project widget; empty: no project (ProjectOpen off).
    void setProject(const QString& name);
    [[nodiscard]] const QString& projectName() const noexcept { return m_projectName; }
    /*
     * Replaces what File > Open... (actions::kOpenFile) does: the composition root shows its own
     * dialog (solutions, folders and files) and opens the choice through its open-file path. Empty
     * restores the built-in file dialog that opens the chosen file with openFile().
     */
    void setOpenFileHandler(std::function<void()> handler) { m_openFileHandler = std::move(handler); }

    // Opens `path` in a tab. A failure is shown as an error notification and returned; a file the
    // large-file guard refuses (OutOfRange) is titled "<file> is too large to open".
    Result<int> openFile(const std::string& path);

    // The shell's popups; each returns the open popup (owned by the window, deleted on close) or
    // nullptr when it does not apply (no tabs, no editor, no appearance manager).
    ListPopup* showSwitcher(bool backward);
    InputPopup* showGoToLine();
    ListPopup* showQuickSwitchTheme();
    // The open shell popup, or nullptr.
    [[nodiscard]] Popup* activePopup() const;
    // Shows `popup` (owned by the window, deleted on close) centred near the top, closing the one
    // open before; the popups of other components (Find Action, Go to File; ADR 0042) use it too.
    void openPopup(Popup* popup);
    // Moves the keyboard focus to the active editor, if any.
    void focusEditor();

    // Posts start-up warnings (AppearanceManager::InstallReport::warnings) as one notification.
    void reportStartupWarnings(const std::vector<std::string>& warnings);

    // Runs `hook` at the start of teardown(), while the tabs, tool windows and dispatcher are whole
    // (hooks run in the order added, once): the composition root removes what it attached to the
    // window there (ADR 0044).
    void addTeardownHook(std::function<void()> hook) { m_teardownHooks.push_back(std::move(hook)); }

protected:
    // Idempotent; see the class comment.
    void teardown();
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildCentral();
    void buildToolWindows();
    void registerActions();
    void addShellAction(std::string_view id, std::string_view name, std::string_view group, std::function<void()> run,
                        std::function<bool()> enabled = {}, std::optional<input::ContextRule> rule = std::nullopt,
                        bool searchable = true);
    void onCurrentEditorChanged(IEditorView* editor);
    void updateTitle();
    void onEditorStatus(IEditorView* editor);
    void focusFirst(const std::vector<QWidget*>& widgets);
    void storeChromeVisibility();
    void storeAppearance(const std::function<void(AppearanceSettings&)>& change);
    [[nodiscard]] QString shortcutText(std::string_view actionId);

    ShellEnvironment m_env;
    NotificationCenter* m_notifications = nullptr;
    BackgroundTaskManager* m_tasks = nullptr;
    EditorTabs* m_tabs = nullptr;
    ToolWindowManager* m_toolWindows = nullptr;
    ShellMenus* m_menus = nullptr;
    MainToolbar* m_toolbar = nullptr;
    NavigationBar* m_navigationBar = nullptr;
    ShellStatusBar* m_status = nullptr;
    ShortcutDispatcher* m_dispatcher = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QPointer<Popup> m_popup;
    QPointer<ListPopup> m_switcher;
    std::vector<std::string> m_registrationErrors;
    std::function<void()> m_openFileHandler;
    std::vector<std::function<void()>> m_teardownHooks;
    QString m_projectName;
    input::AvailabilityLevel m_level = input::AvailabilityLevel::L0;
    bool m_trusted = false;
    bool m_projectOpen = false;
    bool m_tornDown = false;
};

} // namespace frost::ui
