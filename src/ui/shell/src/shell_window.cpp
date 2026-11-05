// SPDX-License-Identifier: Apache-2.0
//
// ShellWindow (frost/ui/shell/shell_window.hpp): builds the shell, registers the shell's actions
// and wires the parts together.

#include "frost/ui/shell/shell_window.hpp"

#include "frost/core/build_info.hpp"
#include "frost/input/keymap.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/glacial_logo.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/island_frame.hpp"
#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/navigation_bar.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_menus.hpp"
#include "frost/ui/shell/shortcut_dispatcher.hpp"
#include "frost/ui/shell/status_bar.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "shell_util.hpp"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QRhiWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef _WIN32
#include <windows.h>
#endif

#include <array>
#include <utility>

namespace frost::ui {

using shell_detail::fromUtf8;
using shell_detail::text;
using shell_detail::toUtf8;

namespace {

// Window-level actions: anywhere except in a popup or a modal dialog.
constexpr input::ContextRule kWindowRule{{}, {input::ContextKey::PopupFocus, input::ContextKey::DialogFocus}};
// The switcher also runs inside its own popup (Ctrl+Tab again moves the selection).
constexpr input::ContextRule kSwitcherRule{{}, {input::ContextKey::DialogFocus}};
// Escape returns from a tool window to the editor.
constexpr input::ContextRule kToolWindowRule{{input::ContextKey::ToolWindowFocus},
                                             {input::ContextKey::PopupFocus, input::ContextKey::DialogFocus}};

constexpr int kRefreshIntervalMs = 500;

bool isTextField(const QWidget* widget) {
    if (qobject_cast<const QLineEdit*>(widget) != nullptr || qobject_cast<const QTextEdit*>(widget) != nullptr ||
        qobject_cast<const QPlainTextEdit*>(widget) != nullptr ||
        qobject_cast<const QAbstractSpinBox*>(widget) != nullptr) {
        return true;
    }
    const auto* combo = qobject_cast<const QComboBox*>(widget);
    return combo != nullptr && combo->isEditable();
}

void addKeys(input::ContextKeys& keys, input::ContextKeys extra) {
    for (unsigned bit = 0; bit < 64; ++bit) {
        if ((extra.bits() >> bit & 1U) != 0) {
            keys.set(static_cast<input::ContextKey>(bit));
        }
    }
}

// A placeholder tool window body until the slice that owns the window fills it.
QWidget* placeholderContent(const QString& message) {
    auto* label = new QLabel(message);
    label->setObjectName(QStringLiteral("toolWindowPlaceholder"));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setMargin(12);
    return label;
}

struct ThemeChoice {
    std::string_view id;
    std::string_view name;
};
constexpr std::array kThemes{
    ThemeChoice{"rider-dark", "Rider Dark"},
    ThemeChoice{"rider-light", "Rider Light"},
    ThemeChoice{"rider-islands-dark", "Rider Islands Dark"},
    ThemeChoice{"darcula", "Darcula"},
    ThemeChoice{"glacier-dark", "Glacier Dark"},
    ThemeChoice{"glacier-light", "Glacier Light"},
    ThemeChoice{"glacier-classic", "Glacier Classic"},
    ThemeChoice{"glacier-high-contrast", "Glacier High Contrast"},
    ThemeChoice{"monokai-pro", "Monokai Pro"},
    ThemeChoice{"dracula", "Dracula"},
    ThemeChoice{"nord", "Nord"},
    ThemeChoice{"one-dark", "One Dark"},
    ThemeChoice{"solarized-dark", "Solarized Dark"},
    ThemeChoice{"solarized-light", "Solarized Light"},
    ThemeChoice{"catppuccin-mocha", "Catppuccin Mocha"},
    ThemeChoice{"catppuccin-macchiato", "Catppuccin Macchiato"},
    ThemeChoice{"catppuccin-frappe", "Catppuccin Frappe"},
    ThemeChoice{"catppuccin-latte", "Catppuccin Latte"},
    ThemeChoice{"tokyo-night", "Tokyo Night"},
    ThemeChoice{"tokyo-night-storm", "Tokyo Night Storm"},
    ThemeChoice{"tokyo-night-light", "Tokyo Night Light"},
    ThemeChoice{"gruvbox-dark", "Gruvbox Dark"},
    ThemeChoice{"gruvbox-light", "Gruvbox Light"},
    ThemeChoice{"github-dark", "GitHub Dark"},
    ThemeChoice{"github-light", "GitHub Light"},
    ThemeChoice{"ayu-dark", "Ayu Dark"},
    ThemeChoice{"ayu-mirage", "Ayu Mirage"},
    ThemeChoice{"ayu-light", "Ayu Light"},
    ThemeChoice{"everforest-dark", "Everforest Dark"},
    ThemeChoice{"kanagawa-wave", "Kanagawa Wave"},
    ThemeChoice{"rose-pine", "Rose Pine"},
    ThemeChoice{"rose-pine-dawn", "Rose Pine Dawn"},
    ThemeChoice{"night-owl", "Night Owl"},
    ThemeChoice{"synthwave-84", "SynthWave '84"},
    ThemeChoice{kSyncWithOs, "Sync with OS"},
};

} // namespace

std::optional<LinePosition> parseGoToLine(const QString& value) {
    const QStringList parts = value.trimmed().split(QLatin1Char(':'));
    if (parts.isEmpty() || parts.size() > 2) {
        return std::nullopt;
    }
    bool ok = false;
    LinePosition position;
    position.line = parts[0].trimmed().toInt(&ok);
    if (!ok || position.line < 1) {
        return std::nullopt;
    }
    if (parts.size() == 2) {
        position.column = parts[1].trimmed().toInt(&ok);
        if (!ok || position.column < 1) {
            return std::nullopt;
        }
    }
    return position;
}

ShellWindow::ShellWindow(ShellEnvironment environment, QWidget* parent)
    : QMainWindow(parent), m_env(environment), m_notifications(new NotificationCenter(this)),
      m_tasks(new BackgroundTaskManager(this)) {
    setObjectName(QStringLiteral("FrostMainWindow"));
    setWindowTitle(QStringLiteral("FrostIDE"));
    setWindowIcon(glacialLogoIcon());
    resize(1280, 800);

#ifdef _WIN32
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        // Use a custom themed Rider window header on desktop instead of the native OS titlebar.
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint |
                       Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    }
#endif

    buildCentral();
    registerActions();
    buildToolWindows();

    m_toolbar = new MainToolbar([this](std::string_view id) { static_cast<void>(invoke(id)); }, this);
    addToolBar(Qt::TopToolBarArea, m_toolbar);

    m_menus = new ShellMenus(
        *menuBar(), m_env.registry, m_env.keymap, [this] { return actionContext(); },
        [this](std::string_view id) { static_cast<void>(invoke(id, input::InvokeSource::Menu)); }, this);
    m_menus->build(defaultMenus());
    m_menus->setChecked(actions::kToggleToolbar, [this] { return !m_toolbar->isHidden(); });
    m_menus->setChecked(actions::kToggleStatusBar, [this] { return !statusBar()->isHidden(); });
    m_menus->setChecked(actions::kCompactMode, [this] {
        return m_env.appearanceSettings != nullptr && m_env.appearanceSettings->load().density == Density::Compact;
    });
    for (const std::string_view id : {actions::kProjectWindow, actions::kCommitWindow, actions::kRunWindow,
                                      actions::kProblemsWindow, actions::kNotificationsWindow}) {
        const std::string window = id == actions::kProjectWindow    ? std::string(tool_windows::kProject)
                                   : id == actions::kCommitWindow   ? std::string(tool_windows::kCommit)
                                   : id == actions::kRunWindow      ? std::string(tool_windows::kRun)
                                   : id == actions::kProblemsWindow ? std::string(tool_windows::kProblems)
                                                                    : std::string(tool_windows::kNotifications);
        m_menus->setChecked(id,
                            [this, window] { return m_toolWindows != nullptr && m_toolWindows->isVisible(window); });
    }

    // On desktop, collapse the separate horizontal menu bar row by default so the unified Rider
    // header bar sits at the top of the window, while keeping all menu actions active and accessible
    // through the header's Main Menu button.
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        menuBar()->setFixedHeight(0);
    }
    m_toolbar->setMainMenuBuilder([this](QMenu& popup) {
        if (m_menus != nullptr) {
            m_menus->refresh();
        }
        for (QAction* topAction : menuBar()->actions()) {
            if (topAction != nullptr && topAction->isVisible()) {
                popup.addAction(topAction);
            }
        }
        popup.addSeparator();
        QAction* toggleMenuBar = popup.addAction(text("Show Classic Menu Bar"));
        toggleMenuBar->setCheckable(true);
        toggleMenuBar->setChecked(m_classicMenuBarVisible);
        connect(toggleMenuBar, &QAction::triggered, this, [this](bool checked) {
            m_classicMenuBarVisible = checked;
            if (checked) {
                menuBar()->setMinimumHeight(0);
                menuBar()->setMaximumHeight(QWIDGETSIZE_MAX);
            } else {
                menuBar()->setFixedHeight(0);
            }
        });
    });

    if (m_env.appearance != nullptr) {
        connect(m_env.appearance, &AppearanceManager::appearanceChanged, this, [this] {
            syncNativeWindowFrame();
            if (m_toolbar != nullptr) {
                m_toolbar->update();
            }
            update();
        });
    }

    m_status = new ShellStatusBar(*statusBar(), *m_tasks, *m_notifications, this);
    statusBar()->showMessage(text("Ready"));
    connect(m_status, &ShellStatusBar::caretClicked, this, [this] { static_cast<void>(showGoToLine()); });
    connect(m_status, &ShellStatusBar::notificationsClicked, this,
            [this] { m_toolWindows->toggle(tool_windows::kNotifications); });

    connect(m_tabs, &EditorTabs::currentEditorChanged, this, &ShellWindow::onCurrentEditorChanged);
    connect(m_tabs, &EditorTabs::statusUpdated, this, &ShellWindow::onEditorStatus);
    connect(m_toolWindows, &ToolWindowManager::storeFailed, this, [this](const QString& message) {
        m_notifications->notify(
            {text("Cannot save the window layout"), message, NotificationKind::Warning, std::nullopt, {}});
    });

    m_dispatcher = new ShortcutDispatcher(
        *this, m_env.registry, m_env.keymap, [this] { return actionContext(); }, m_env.followActiveLayout, this);
    connect(m_dispatcher, &ShortcutDispatcher::pendingChanged, this, [this](const QString& prefix) {
        statusBar()->showMessage(
            prefix.isEmpty() ? text("Ready") : text("%1 was pressed. Waiting for the second stroke...").arg(prefix));
    });
    connect(m_dispatcher, &ShortcutDispatcher::invoked, this, [this] { refreshActions(); });
    qApp->installEventFilter(m_dispatcher);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(kRefreshIntervalMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &ShellWindow::refreshActions);

    if (const Result<ShellLayout> layout = m_env.settings.load(); layout) {
        m_toolbar->setVisible(layout->toolbarVisible);
        statusBar()->setVisible(layout->statusBarVisible);
    }
    if (!m_registrationErrors.empty()) {
        QString details;
        for (const std::string& error : m_registrationErrors) {
            details += fromUtf8(error) + QLatin1Char('\n');
        }
        m_notifications->notify({text("Some shell actions could not be registered"),
                                 details.trimmed(),
                                 NotificationKind::Warning,
                                 std::nullopt,
                                 {}});
    }
    refreshActions();
}

ShellWindow::~ShellWindow() { teardown(); }

void ShellWindow::teardown() {
    if (m_tornDown) {
        return;
    }
    m_tornDown = true;
    for (const std::function<void()>& hook : std::exchange(m_teardownHooks, {})) {
        hook();
    }
    if (m_dispatcher != nullptr) {
        qApp->removeEventFilter(m_dispatcher);
        delete m_dispatcher;
        m_dispatcher = nullptr;
    }
    m_refreshTimer->stop();
    delete m_popup.data();
    m_notifications->setBalloonHost(nullptr);
    // The editors and the tool window contents go now, while the services they use still exist.
    delete m_toolWindows;
    m_toolWindows = nullptr;
    delete takeCentralWidget();
    m_tabs = nullptr;
}

void ShellWindow::buildCentral() {
    auto* host = new IslandHost(QBoxLayout::TopToBottom, this);
    host->setObjectName(QStringLiteral("shellCentral"));
    auto* dock = new QWidget(host);
    dock->setObjectName(QStringLiteral("toolWindowHost"));
    host->addIsland(dock, 1);

    auto* editorIsland = new IslandFrame(dock);
    editorIsland->setObjectName(QStringLiteral("editorIsland"));
    m_tabs = new EditorTabs(m_env.editors, editorIsland);
#ifdef _WIN32
    if (QGuiApplication::platformName() == QStringLiteral("windows") && GetSystemMetrics(SM_REMOTESESSION) == 0) {
        // Keep a persistent Direct3D 11 QRhiWidget in the top-level window from startup so Qt
        // creates the native HWND with a Direct3D surface up front instead of destroying and
        // recreating the window when the first RhiEditorViewport tab is opened.
        auto* rhiAnchor = new QRhiWidget(editorIsland);
        rhiAnchor->setObjectName(QStringLiteral("rhiBackingAnchor"));
        rhiAnchor->setApi(QRhiWidget::Api::Direct3D11);
        rhiAnchor->setFixedSize(1, 1);
        rhiAnchor->move(2, 2);
        rhiAnchor->lower();
        rhiAnchor->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        rhiAnchor->setFocusPolicy(Qt::NoFocus);
    }
#endif
    auto* editorLayout = new QVBoxLayout(editorIsland);
    editorLayout->setContentsMargins(1, 1, 1, 1);
    editorLayout->setSpacing(0);
    auto* navigationScroll = new QScrollArea(editorIsland);
    navigationScroll->setObjectName(QStringLiteral("navigationScroll"));
    navigationScroll->setFrameShape(QFrame::NoFrame);
    navigationScroll->setWidgetResizable(true);
    navigationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    navigationScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigationBar = new NavigationBar(navigationScroll);
    navigationScroll->setWidget(m_navigationBar);
    navigationScroll->setFixedHeight(fontMetrics().height() + 12);
    connect(navigationScroll->horizontalScrollBar(), &QScrollBar::rangeChanged, navigationScroll,
            [navigationScroll](int, int maximum) {
                navigationScroll->setFixedHeight(
                    navigationScroll->fontMetrics().height() + 12 +
                    (maximum > 0 ? navigationScroll->horizontalScrollBar()->sizeHint().height() : 0));
            });
    navigationScroll->hide();
    connect(m_navigationBar, &NavigationBar::contentVisibilityChanged, navigationScroll, &QWidget::setVisible);
    editorLayout->addWidget(navigationScroll);
    editorLayout->addWidget(m_tabs);

    m_toolWindows = new ToolWindowManager(*dock, *editorIsland, m_env.settings, this);
    setCentralWidget(host);
    m_notifications->setBalloonHost(host);
}

void ShellWindow::buildToolWindows() {
    struct Default {
        std::string_view id;
        std::string_view title;
        ToolWindowSide side;
        std::string_view icon;
        std::string_view action;
        std::string_view placeholder;
    };
    const std::array defaults{
        Default{tool_windows::kProject, "Project", ToolWindowSide::Left, icons::kNodesFolder, actions::kProjectWindow,
                "No solution is open. Open a solution or a folder to see its files here."},
        Default{tool_windows::kCommit, "Commit", ToolWindowSide::Left, icons::kVcsBranch, actions::kCommitWindow,
                "Version control is not set up for this project yet."},
        Default{tool_windows::kRun, "Run", ToolWindowSide::Bottom, icons::kRunRun, actions::kRunWindow,
                "Build and run output will appear here."},
        Default{tool_windows::kProblems, "Problems", ToolWindowSide::Bottom, icons::kStatusWarning,
                actions::kProblemsWindow, "No problems found."},
    };
    for (const Default& entry : defaults) {
        ToolWindowSpec spec;
        spec.id = std::string(entry.id);
        spec.title = text(entry.title);
        spec.side = entry.side;
        spec.iconId = std::string(entry.icon);
        spec.shortcutText = shortcutText(entry.action);
        const QString message = text(entry.placeholder);
        spec.createContent = [message] { return placeholderContent(message); };
        static_cast<void>(m_toolWindows->add(std::move(spec)));
    }
    ToolWindowSpec notificationsSpec;
    notificationsSpec.id = std::string(tool_windows::kNotifications);
    notificationsSpec.title = text("Notifications");
    notificationsSpec.side = ToolWindowSide::Right;
    notificationsSpec.iconId = std::string(icons::kToolwindowNotifications);
    notificationsSpec.createContent = [this] { return new NotificationLogView(*m_notifications); };
    static_cast<void>(m_toolWindows->add(std::move(notificationsSpec)));
}

void ShellWindow::addShellAction(std::string_view id, std::string_view name, std::string_view group,
                                 std::function<void()> run, std::function<bool()> enabled,
                                 std::optional<input::ContextRule> rule, bool searchable) {
    input::ActionSpec spec;
    spec.id = std::string(id);
    spec.name = std::string(name);
    spec.group = std::string(group);
    spec.context = rule.value_or(kWindowRule);
    spec.searchable = searchable;
    const QPointer<ShellWindow> self(this);
    spec.handler = [self, run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) {
        if (self != nullptr && !self->m_tornDown && run) {
            run();
        }
    };
    if (enabled) {
        spec.enabled = [self, enabled = std::move(enabled)](const input::ActionContext&) {
            return self != nullptr && !self->m_tornDown && enabled();
        };
    }
    if (Result<input::ActionId> added = m_env.registry.add(std::move(spec)); !added) {
        m_registrationErrors.push_back(added.error().message());
    }
}

void ShellWindow::registerActions() {
    namespace a = actions;
    const auto hasTabs = [this] { return m_tabs != nullptr && m_tabs->count() > 0; };
    const auto hasAnyTabs = [this] { return m_tabs != nullptr && m_tabs->totalCount() > 0; };
    const auto hasEditor = [this] { return m_tabs != nullptr && m_tabs->currentEditor() != nullptr; };
    const auto hasAppearance = [this] { return m_env.appearance != nullptr && m_env.appearanceSettings != nullptr; };
    const auto hasAppearanceSettings = [this] { return m_env.appearanceSettings != nullptr; };

    addShellAction(a::kOpenFile, "Open File...", "File", [this] {
        if (m_openFileHandler) {
            m_openFileHandler();
            return;
        }
        const QString chosen = QFileDialog::getOpenFileName(this, text("Open File"));
        if (!chosen.isEmpty()) {
            static_cast<void>(openFile(toUtf8(QDir::toNativeSeparators(chosen))));
        }
    });
    addShellAction(a::kExit, "Exit", "File", [this] { close(); });

    addShellAction(
        a::kCloseTab, "Close Tab", "Window", [this] { static_cast<void>(m_tabs->close(m_tabs->currentIndex())); },
        hasTabs);
    addShellAction(
        a::kCloseOtherTabs, "Close Other Tabs", "Window", [this] { m_tabs->closeOthers(m_tabs->currentIndex()); },
        hasTabs);
    addShellAction(a::kCloseAllTabs, "Close All Tabs", "Window", [this] { m_tabs->closeAll(); }, hasAnyTabs);
    addShellAction(
        a::kCloseAllButPinned, "Close All but Pinned", "Window", [this] { m_tabs->closeAllButPinned(); }, hasTabs);
    addShellAction(
        a::kPinTab, "Pin Tab", "Window",
        [this] {
            const int current = m_tabs->currentIndex();
            m_tabs->setPinned(current, !m_tabs->isPinned(current));
        },
        hasTabs);
    addShellAction(a::kNextTab, "Select Next Tab", "Window", [this] { m_tabs->activateNext(); }, hasTabs);
    addShellAction(a::kPreviousTab, "Select Previous Tab", "Window", [this] { m_tabs->activatePrevious(); }, hasTabs);
    addShellAction(
        a::kSwitcher, "Switcher", "Window", [this] { static_cast<void>(showSwitcher(false)); }, hasTabs, kSwitcherRule);
    addShellAction(
        a::kSwitcherBack, "Switcher (Backward)", "Window", [this] { static_cast<void>(showSwitcher(true)); }, hasTabs,
        kSwitcherRule, false);

    struct WindowAction {
        std::string_view action;
        std::string_view window;
        std::string_view name;
    };
    for (const WindowAction& entry :
         {WindowAction{a::kProjectWindow, tool_windows::kProject, "Project"},
          WindowAction{a::kCommitWindow, tool_windows::kCommit, "Commit"},
          WindowAction{a::kRunWindow, tool_windows::kRun, "Run"},
          WindowAction{a::kProblemsWindow, tool_windows::kProblems, "Problems"},
          WindowAction{a::kNotificationsWindow, tool_windows::kNotifications, "Notifications"}}) {
        const std::string window(entry.window);
        addShellAction(entry.action, entry.name, "Tool Windows", [this, window] { m_toolWindows->activate(window); });
    }
    addShellAction(a::kHideActiveWindow, "Hide Active Tool Window", "Window", [this] { m_toolWindows->hideActive(); });
    addShellAction(a::kHideAllWindows, "Hide All Tool Windows", "Window", [this] { m_toolWindows->hideAll(); });
    addShellAction(a::kJumpToLastWindow, "Jump to Last Tool Window", "Window", [this] { m_toolWindows->jumpToLast(); });
    addShellAction(a::kFocusEditor, "Focus Editor", "Window", [this] { focusEditor(); }, {}, kToolWindowRule);
    addShellAction(a::kFocusToolbar, "Focus Main Toolbar", "Window", [this] { focusFirst(m_toolbar->focusOrder()); });
    addShellAction(
        a::kFocusNavigationBar, "Focus Navigation Bar", "Navigate", [this] { m_navigationBar->focusNavigation(); },
        [this] { return m_navigationBar->hasNavigation(); });
    addShellAction(a::kFocusStatusBar, "Focus Status Bar", "Window", [this] { focusFirst(m_status->focusOrder()); });

    addShellAction(a::kToggleToolbar, "Toolbar", "View", [this] {
        m_toolbar->setVisible(m_toolbar->isHidden());
        storeChromeVisibility();
    });
    addShellAction(a::kToggleStatusBar, "Status Bar", "View", [this] {
        statusBar()->setVisible(statusBar()->isHidden());
        storeChromeVisibility();
    });
    addShellAction(
        a::kCompactMode, "Compact Mode", "View",
        [this] {
            storeAppearance([](AppearanceSettings& settings) {
                settings.density = settings.density == Density::Compact ? Density::Normal : Density::Compact;
            });
        },
        hasAppearance);
    const auto zoom = [this](Result<void> (AppearanceManager::*step)()) {
        if (Result<void> zoomed = (m_env.appearance->*step)(); !zoomed) {
            m_notifications->notify({text("Cannot change the IDE zoom"),
                                     fromUtf8(zoomed.error().message()),
                                     NotificationKind::Warning,
                                     std::nullopt,
                                     {}});
        }
    };
    addShellAction(a::kZoomIn, "Zoom In IDE", "View", [zoom] { zoom(&AppearanceManager::zoomIn); }, hasAppearance);
    addShellAction(a::kZoomOut, "Zoom Out IDE", "View", [zoom] { zoom(&AppearanceManager::zoomOut); }, hasAppearance);
    addShellAction(
        a::kResetZoom, "Reset IDE Zoom", "View", [zoom] { zoom(&AppearanceManager::resetZoom); }, hasAppearance);
    addShellAction(
        a::kQuickSwitchTheme, "Quick Switch Theme...", "View", [this] { static_cast<void>(showQuickSwitchTheme()); },
        hasAppearanceSettings);

    addShellAction(
        a::kGoToLine, "Go to Line:Column...", "Navigate", [this] { static_cast<void>(showGoToLine()); }, hasEditor);

    addShellAction(a::kAbout, "About FrostIDE", "Help", [this] {
        m_notifications->notify(
            {text("FrostIDE %1").arg(fromUtf8(build_info::kVersion)),
             text("Built with %1 and Qt %2 for %3.")
                 .arg(fromUtf8(build_info::kCompiler), QString::fromLatin1(qVersion()), fromUtf8(build_info::kSystem)),
             NotificationKind::Info,
             false,
             {}});
    });
}

input::ActionContext ShellWindow::actionContext() const {
    input::ActionContext context;
    context.level = m_level;
    context.trusted = m_trusted;
    input::ContextKeys& keys = context.keys;
    keys.set(input::ContextKey::ProjectOpen, m_projectOpen);
    if (QApplication::activeModalWidget() != nullptr) {
        keys.set(input::ContextKey::DialogFocus);
    }
    const QWidget* focus = QApplication::focusWidget();
    if (qobject_cast<Popup*>(QApplication::activePopupWidget()) != nullptr) {
        keys.set(input::ContextKey::PopupFocus);
    }
    if (focus == nullptr || keys.has(input::ContextKey::PopupFocus) || keys.has(input::ContextKey::DialogFocus) ||
        m_tabs == nullptr) {
        return context;
    }
    if (IEditorView* editor = m_tabs->currentEditor();
        editor != nullptr && (editor->widget() == focus || editor->widget()->isAncestorOf(focus))) {
        const QWidget* surface =
            editor->widget()->focusProxy() != nullptr ? editor->widget()->focusProxy() : editor->widget();
        if (focus != surface && focus != editor->widget() && isTextField(focus)) {
            keys.set(input::ContextKey::TextFieldFocus);
            return context;
        }
        keys.set(input::ContextKey::EditorFocus);
        addKeys(keys, editor->contextKeys());
        return context;
    }
    if (const std::string window = m_toolWindows != nullptr ? m_toolWindows->focusedWindow() : std::string();
        !window.empty()) {
        keys.set(input::ContextKey::ToolWindowFocus);
        keys.set(input::ContextKey::ProjectViewFocus, window == tool_windows::kProject || window == "fileSystem");
        keys.set(input::ContextKey::CommitViewFocus, window == tool_windows::kCommit);
        keys.set(input::ContextKey::DiffViewFocus, window == "gitDiff");
        keys.set(input::ContextKey::TerminalFocus, window == tool_windows::kTerminal);
    }
    if (isTextField(focus)) {
        keys.set(input::ContextKey::TextFieldFocus);
    }
    return context;
}

input::InvokeResult ShellWindow::invoke(std::string_view actionId, input::InvokeSource source) {
    const std::optional<input::ActionId> id = m_env.registry.find(actionId);
    if (!id || m_tornDown) {
        return input::InvokeResult::Disabled;
    }
    const input::InvokeResult result = m_env.registry.tryInvoke(*id, actionContext(), input::InvokeInfo{source, false});
    if (!m_tornDown) {
        refreshActions();
    }
    return result;
}

void ShellWindow::refreshActions() {
    if (m_tornDown) {
        return;
    }
    if (m_menus != nullptr) {
        m_menus->refreshIfStale();
    }
    if (m_toolbar != nullptr) {
        m_toolbar->refresh(m_env.registry, m_env.keymap, actionContext());
    }
}

void ShellWindow::setProject(const QString& name) {
    m_projectOpen = !name.isEmpty();
    m_projectName = name;
    m_toolbar->setProjectName(name);
    updateTitle();
}

void ShellWindow::updateTitle() {
    if (!m_projectName.isEmpty()) {
        setWindowTitle(QStringLiteral("%1 - FrostIDE").arg(m_projectName));
        return;
    }
    const IEditorView* editor = m_tabs != nullptr ? m_tabs->currentEditor() : nullptr;
    setWindowTitle(editor == nullptr
                       ? QStringLiteral("FrostIDE")
                       : QStringLiteral("%1 - FrostIDE").arg(QFileInfo(fromUtf8(editor->path())).fileName()));
}

Result<int> ShellWindow::openFile(const std::string& path) {
    Result<int> opened = m_tabs->open(path);
    if (!opened) {
        // OutOfRange is the large-file guard (ADR 0028): the file is refused before it is read.
        const QString name = QDir::toNativeSeparators(fromUtf8(path));
        const QString title = opened.error().code() == ErrorCode::OutOfRange ? text("%1 is too large to open").arg(name)
                                                                             : text("Cannot open %1").arg(name);
        m_notifications->notify({title, fromUtf8(opened.error().message()), NotificationKind::Error, std::nullopt, {}});
    } else if (IEditorView* editor = m_tabs->currentEditor(); editor != nullptr && isVisible()) {
        editor->widget()->setFocus(Qt::OtherFocusReason);
    }
    return opened;
}

ListPopup* ShellWindow::showSwitcher(bool backward) {
    if (m_switcher != nullptr && m_switcher->isOpen()) {
        m_switcher->moveSelection(backward ? -1 : 1);
        return m_switcher;
    }
    const std::vector<int> order = m_tabs->mruOrder();
    if (order.empty()) {
        return nullptr;
    }
    auto* popup = new ListPopup(this, text("Switcher"), false);
    popup->setObjectName(QStringLiteral("switcherPopup"));
    std::vector<ListPopupItem> items;
    for (const int index : order) {
        const IEditorView* editor = m_tabs->editor(index);
        const QString path = fromUtf8(editor->path());
        items.push_back(ListPopupItem{m_tabs->tabText(index), QDir::toNativeSeparators(QFileInfo(path).path()), QIcon(),
                                      QVariant(path)});
    }
    popup->setItems(std::move(items));
    popup->setAcceptOnRelease(Qt::Key_Control);
    const int count = static_cast<int>(order.size());
    popup->setCurrentItem(count == 1 ? 0 : (backward ? count - 1 : 1));
    connect(popup, &ListPopup::itemAccepted, this, [this](int, const QVariant& data) {
        m_tabs->activate(m_tabs->indexOf(toUtf8(data.toString())));
        focusEditor();
    });
    m_switcher = popup;
    openPopup(popup);
    return popup;
}

InputPopup* ShellWindow::showGoToLine() {
    IEditorView* editor = m_tabs->currentEditor();
    if (editor == nullptr) {
        return nullptr;
    }
    const EditorStatus status = editor->status();
    auto* popup =
        new InputPopup(this, text("Line [:column]:"), QStringLiteral("%1:%2").arg(status.line).arg(status.column));
    popup->setObjectName(QStringLiteral("goToLinePopup"));
    popup->setValidator([](const QString& value) -> std::optional<QString> {
        if (parseGoToLine(value)) {
            return std::nullopt;
        }
        return text("Enter a line number, optionally followed by a colon and a column");
    });
    connect(popup, &InputPopup::submitted, this, [this](const QString& value) {
        const std::optional<LinePosition> position = parseGoToLine(value);
        if (IEditorView* target = m_tabs->currentEditor(); target != nullptr && position) {
            target->goToLine(position->line, position->column);
            focusEditor();
        }
    });
    openPopup(popup);
    return popup;
}

ListPopup* ShellWindow::showQuickSwitchTheme() {
    if (m_env.appearanceSettings == nullptr) {
        return nullptr;
    }
    auto* popup = new ListPopup(this, text("Theme"), false);
    popup->setObjectName(QStringLiteral("quickSwitchThemePopup"));
    std::vector<ListPopupItem> items;
    const std::string current = m_env.appearanceSettings->load().theme;
    int selected = 0;
    for (const ThemeChoice& theme : kThemes) {
        if (theme.id == current) {
            selected = static_cast<int>(items.size());
        }
        items.push_back(ListPopupItem{text(theme.name), {}, {}, QVariant(fromUtf8(theme.id))});
    }
    popup->setItems(std::move(items));
    popup->setCurrentItem(selected);
    connect(popup, &ListPopup::itemAccepted, this, [this](int, const QVariant& data) {
        const std::string id = toUtf8(data.toString());
        storeAppearance([id](AppearanceSettings& settings) { settings.theme = id; });
    });
    openPopup(popup);
    return popup;
}

Popup* ShellWindow::activePopup() const { return m_popup != nullptr && m_popup->isOpen() ? m_popup.data() : nullptr; }

void ShellWindow::reportStartupWarnings(const std::vector<std::string>& warnings) {
    if (warnings.empty()) {
        return;
    }
    QString details;
    for (const std::string& warning : warnings) {
        details += fromUtf8(warning) + QLatin1Char('\n');
    }
    m_notifications->notify(
        {text("FrostIDE started with problems"), details.trimmed(), NotificationKind::Warning, true, {}});
}

void ShellWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    syncNativeWindowFrame();
    // Later slices register their actions after the window exists; pick them up.
    if (m_menus != nullptr) {
        m_menus->refresh();
    }
    refreshActions();
    m_refreshTimer->start();
}

void ShellWindow::hideEvent(QHideEvent* event) {
    QMainWindow::hideEvent(event);
    if (m_refreshTimer != nullptr) {
        m_refreshTimer->stop();
    }
}

void ShellWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event != nullptr && event->type() == QEvent::WindowStateChange) {
        syncNativeWindowFrame();
        if (m_toolbar != nullptr) {
            if (QToolButton* maxBtn = m_toolbar->maximizeButton(); maxBtn != nullptr) {
                maxBtn->update();
            }
        }
    }
}

bool ShellWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef _WIN32
    if ((windowFlags() & Qt::FramelessWindowHint) != 0 && eventType == "windows_generic_MSG" && message != nullptr &&
        result != nullptr) {
        const auto* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
            if (IsZoomed(msg->hwnd) != FALSE) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                const int frameX = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                const int frameY = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                params->rgrc[0].left += frameX;
                params->rgrc[0].top += frameY;
                params->rgrc[0].right -= frameX;
                params->rgrc[0].bottom -= frameY;
            }
            *result = 0;
            return true;
        }
        if (msg->message == WM_NCHITTEST && msg->hwnd == reinterpret_cast<HWND>(winId())) {
            const LONG x = static_cast<short>(LOWORD(msg->lParam));
            const LONG y = static_cast<short>(HIWORD(msg->lParam));
            if (!isMaximized() && !isFullScreen()) {
                RECT winRect{};
                if (GetWindowRect(msg->hwnd, &winRect) != FALSE) {
                    constexpr LONG kResizeBorder = 6;
                    const bool left = x >= winRect.left && x < winRect.left + kResizeBorder;
                    const bool right = x <= winRect.right && x > winRect.right - kResizeBorder;
                    const bool top = y >= winRect.top && y < winRect.top + kResizeBorder;
                    const bool bottom = y <= winRect.bottom && y > winRect.bottom - kResizeBorder;
                    if (top && left) {
                        *result = HTTOPLEFT;
                        return true;
                    }
                    if (top && right) {
                        *result = HTTOPRIGHT;
                        return true;
                    }
                    if (bottom && left) {
                        *result = HTBOTTOMLEFT;
                        return true;
                    }
                    if (bottom && right) {
                        *result = HTBOTTOMRIGHT;
                        return true;
                    }
                    if (left) {
                        *result = HTLEFT;
                        return true;
                    }
                    if (right) {
                        *result = HTRIGHT;
                        return true;
                    }
                    if (top) {
                        *result = HTTOP;
                        return true;
                    }
                    if (bottom) {
                        *result = HTBOTTOM;
                        return true;
                    }
                }
            }
            if (m_toolbar != nullptr && m_toolbar->isVisible()) {
                POINT pt{x, y};
                if (ScreenToClient(msg->hwnd, &pt) != FALSE) {
                    const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
                    const QPoint winPos(qRound(pt.x / dpr), qRound(pt.y / dpr));
                    const QPoint toolbarPos = m_toolbar->mapFrom(this, winPos);
                    if (m_toolbar->rect().contains(toolbarPos)) {
                        QWidget* child = m_toolbar->childAt(toolbarPos);
                        bool interactive = false;
                        for (QWidget* w = child; w != nullptr && w != m_toolbar; w = w->parentWidget()) {
                            if (const auto* btn = qobject_cast<const QToolButton*>(w)) {
                                if (btn->isEnabled() && (btn != m_toolbar->projectButton() || btn->menu() != nullptr)) {
                                    interactive = true;
                                }
                                break;
                            }
                            if (const auto* combo = qobject_cast<const QComboBox*>(w)) {
                                if (combo->isEnabled()) {
                                    interactive = true;
                                }
                                break;
                            }
                        }
                        if (!interactive) {
                            *result = HTCAPTION;
                            return true;
                        }
                    }
                }
            }
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void ShellWindow::syncNativeWindowFrame() {
#ifdef _WIN32
    if (QGuiApplication::platformName() != QStringLiteral("windows") || !isVisible()) {
        return;
    }
    const auto hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr) {
        return;
    }
    if ((windowFlags() & Qt::FramelessWindowHint) != 0) {
        // Restore WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU while
        // WM_NCCALCSIZE removes the standard frame so Windows Aero Snap / Windows 11 Snap Docking,
        // Win+Arrow shortcuts, and drag-from-maximized restore work natively.
        const LONG_PTR currentStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const LONG_PTR desiredStyle = (currentStyle & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_OVERLAPPED |
                                      WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        if (currentStyle != desiredStyle) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, desiredStyle);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
    struct NativeMargins {
        int leftWidth;
        int rightWidth;
        int topHeight;
        int bottomHeight;
    };
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    using DwmExtendFrameIntoClientAreaFn = HRESULT(WINAPI*)(HWND, const NativeMargins*);
    static const HMODULE dwmModule = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const DwmSetWindowAttributeFn setAttr =
        dwmModule == nullptr ? nullptr
                             : reinterpret_cast<DwmSetWindowAttributeFn>(
                                   reinterpret_cast<void*>(GetProcAddress(dwmModule, "DwmSetWindowAttribute")));
    static const DwmExtendFrameIntoClientAreaFn extendFrame =
        dwmModule == nullptr ? nullptr
                             : reinterpret_cast<DwmExtendFrameIntoClientAreaFn>(
                                   reinterpret_cast<void*>(GetProcAddress(dwmModule, "DwmExtendFrameIntoClientArea")));
    if (extendFrame != nullptr && (windowFlags() & Qt::FramelessWindowHint) != 0) {
        const NativeMargins margins{0, 0, 1, 0};
        extendFrame(hwnd, &margins);
    }
    if (setAttr == nullptr) {
        return;
    }
    const FrostStyle* style = FrostStyle::current();
    const BOOL dark = (style == nullptr || style->theme().isDark()) ? TRUE : FALSE;
    constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmwaWindowCornerPreference = 33;
    constexpr DWORD kDwmwaBorderColor = 34;
    constexpr DWORD kDwmwaCaptionColor = 35;
    constexpr DWORD kDwmwcpRound = 2;
    setAttr(hwnd, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));
    setAttr(hwnd, kDwmwaWindowCornerPreference, &kDwmwcpRound, sizeof(kDwmwcpRound));
    if (style != nullptr) {
        const Rgba bg = style->theme().color(ColorKey::MainWindowBackground);
        const Rgba border = style->theme().color(ColorKey::SeparatorColor);
        const COLORREF captionRef = RGB(bg.r(), bg.g(), bg.b());
        const COLORREF borderRef = RGB(border.r(), border.g(), border.b());
        setAttr(hwnd, kDwmwaCaptionColor, &captionRef, sizeof(captionRef));
        setAttr(hwnd, kDwmwaBorderColor, &borderRef, sizeof(borderRef));
    }
#endif
}

void ShellWindow::onCurrentEditorChanged(IEditorView* editor) {
    updateTitle();
    m_navigationBar->setPath(editor != nullptr ? editor->path() : std::string());
    if (editor == nullptr) {
        m_status->showEditor(nullptr);
    } else {
        const EditorStatus status = editor->status();
        m_status->showEditor(&status);
    }
    refreshActions();
}

void ShellWindow::onEditorStatus(IEditorView* editor) {
    if (editor != nullptr && editor == m_tabs->currentEditor()) {
        const EditorStatus status = editor->status();
        m_status->showEditor(&status);
    }
}

void ShellWindow::focusFirst(const std::vector<QWidget*>& widgets) {
    if (!widgets.empty()) {
        widgets.front()->setFocus(Qt::ShortcutFocusReason);
    }
}

void ShellWindow::focusEditor() {
    if (IEditorView* editor = m_tabs->currentEditor(); editor != nullptr) {
        editor->widget()->setFocus(Qt::OtherFocusReason);
    } else {
        m_tabs->setFocus(Qt::OtherFocusReason);
    }
}

void ShellWindow::openPopup(Popup* popup) {
    if (m_popup != nullptr && m_popup != popup) {
        m_popup->closePopup(Popup::CloseReason::Dismissed);
    }
    m_popup = popup;
    popup->showCentered(this, 0.2);
}

void ShellWindow::storeChromeVisibility() {
    Result<ShellLayout> loaded = m_env.settings.load();
    ShellLayout layout = loaded ? std::move(*loaded) : ShellLayout{};
    layout.toolbarVisible = !m_toolbar->isHidden();
    layout.statusBarVisible = !statusBar()->isHidden();
    if (const Result<void> stored = m_env.settings.store(layout); !stored) {
        m_notifications->notify({text("Cannot save the window layout"),
                                 fromUtf8(stored.error().message()),
                                 NotificationKind::Warning,
                                 std::nullopt,
                                 {}});
    }
}

void ShellWindow::storeAppearance(const std::function<void(AppearanceSettings&)>& change) {
    if (m_env.appearanceSettings == nullptr) {
        return;
    }
    AppearanceSettings settings = m_env.appearanceSettings->load();
    change(settings);
    if (const Result<void> stored = m_env.appearanceSettings->store(settings); !stored) {
        m_notifications->notify({text("Cannot change the appearance"),
                                 fromUtf8(stored.error().message()),
                                 NotificationKind::Error,
                                 std::nullopt,
                                 {}});
    }
}

QString ShellWindow::shortcutText(std::string_view actionId) {
    const std::optional<input::ActionId> id = m_env.registry.find(actionId);
    if (!id) {
        return {};
    }
    const std::optional<input::Sequence> shortcut = m_env.keymap.primaryShortcut(*id);
    return shortcut ? fromUtf8(input::toDisplayText(*shortcut, m_env.keymap.layout(), m_env.keymap.platform()))
                    : QString();
}

} // namespace frost::ui
