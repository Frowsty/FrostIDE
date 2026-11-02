// SPDX-License-Identifier: Apache-2.0
//
// TerminalToolWindow and TerminalIntegration (frost/ui/terminal/terminal_tool_window.hpp).

#include "frost/ui/terminal/terminal_tool_window.hpp"

#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/registry.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_view.hpp"
#include "frost/ui/tr.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <optional>
#include <utility>
#include <variant>

namespace frost::ui {

namespace {

constexpr std::string_view kContext = "frost::ui::TerminalToolWindow";

QString text(std::string_view source) { return qtr(kContext, source); }

QString fromUtf8(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

// Not in a popup or a dialog (the shell's window rule).
constexpr input::ContextRule kWindowRule{{}, {input::ContextKey::PopupFocus, input::ContextKey::DialogFocus}};

// editor.fontFamily and editor.fontSize from the built-in registry's defaults.
std::pair<QString, double> editorFontDefaults() {
    const settings::Registry& registry = settings::builtinRegistry();
    QString family = QStringLiteral("JetBrains Mono");
    double size = 13;
    if (const settings::KeySpec* spec = registry.find(settings::key::editorFontFamily.name)) {
        if (const auto* value = std::get_if<std::string>(&spec->defaultValue)) {
            family = fromUtf8(*value);
        }
    }
    if (const settings::KeySpec* spec = registry.find(settings::key::editorFontSize.name)) {
        if (const auto* value = std::get_if<std::int64_t>(&spec->defaultValue)) {
            size = static_cast<double>(*value);
        }
    }
    return {family, size};
}

// The probe with every unset function taken from the system.
term::ShellProbe completeProbe(term::ShellProbe probe) {
    if (!probe.environment || !probe.isExecutable) {
        term::ShellProbe system = term::systemShellProbe();
        if (!probe.environment) {
            probe.environment = std::move(system.environment);
        }
        if (!probe.isExecutable) {
            probe.isExecutable = std::move(system.isExecutable);
        }
        if (probe.systemDirectory.empty()) {
            probe.systemDirectory = std::move(system.systemDirectory);
        }
    }
    return probe;
}

} // namespace

// --- TerminalToolWindow ---

TerminalToolWindow::TerminalToolWindow(TerminalEnvironment environment, QWidget* parent)
    : QWidget(parent), m_env(std::move(environment)) {
    setObjectName(QStringLiteral("terminalToolWindow"));
    m_env.probe = completeProbe(std::move(m_env.probe));
    if (m_env.fontFamily.isEmpty()) {
        std::tie(m_env.fontFamily, m_env.fontSize) = editorFontDefaults();
    }

    m_tabs = new QTabBar(this);
    m_tabs->setObjectName(QStringLiteral("terminalTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setExpanding(false);
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_tabs->installEventFilter(this);
    m_newTab = new QToolButton(this);
    m_newTab->setObjectName(QStringLiteral("terminalNewTab"));
    m_newTab->setText(QStringLiteral("+"));
    m_newTab->setToolTip(text("New Tab"));
    m_newTab->setAutoRaise(true);
    m_newTab->setFocusPolicy(Qt::NoFocus);
    m_stack = new QStackedWidget(this);
    m_message = new QLabel(m_stack);
    m_message->setObjectName(QStringLiteral("terminalMessage"));
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_stack->addWidget(m_message);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(0);
    header->addWidget(m_tabs);
    header->addWidget(m_newTab);
    header->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_stack, 1);

    connect(m_newTab, &QToolButton::clicked, this, [this] { static_cast<void>(newTab()); });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, [this](int index) { closeTab(index); });
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < count() && m_tabState[static_cast<std::size_t>(index)].view != nullptr) {
            m_stack->setCurrentWidget(m_tabState[static_cast<std::size_t>(index)].view);
        }
    });
    connect(m_tabs, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index >= 0) {
            setCurrentIndex(index);
            beginRename();
        }
    });
    if (m_env.appearance != nullptr) {
        connect(m_env.appearance, &AppearanceManager::appearanceChanged, this, [this] {
            for (const Tab& tab : m_tabState) {
                if (tab.view != nullptr) {
                    applyAppearance(*tab.view);
                }
            }
        });
    }
    showMessage(QString());
}

TerminalToolWindow::~TerminalToolWindow() = default;

void TerminalToolWindow::applyAppearance(TerminalView& view) const {
    view.setFontSettings(m_env.fontFamily, m_env.fontSize);
    view.setColors(m_env.appearance != nullptr ? TerminalColors::fromTheme(m_env.appearance->theme())
                                               : TerminalColors::defaults());
}

void TerminalToolWindow::showMessage(const QString& message) {
    m_message->setText(message.isEmpty() ? text("No terminal is open. Click + to start one.") : message);
    if (count() == 0) {
        m_stack->setCurrentWidget(m_message);
    }
}

QString TerminalToolWindow::message() const { return count() == 0 ? m_message->text() : QString(); }

Result<int> TerminalToolWindow::newTab(const std::string& workingDirectory) {
    if (m_stopped || (m_env.available && !m_env.available()))
        return fail(ErrorCode::Cancelled, "terminal is shutting down");
    const auto failWith = [this](Error error) -> Result<int> {
        showMessage(text("Cannot start the terminal: ") + fromUtf8(error.message()));
        log::warn("ui", "terminal: {}", error.message());
        return std::unexpected(std::move(error));
    };
    if (m_env.launcher == nullptr) {
        return failWith(Error(ErrorCode::InvalidArgument, "no terminal launcher"));
    }
    const TerminalContext context = m_env.context ? m_env.context() : TerminalContext{};
    const term::TerminalSettings settings = m_env.settings ? m_env.settings() : term::TerminalSettings{};
    term::TerminalLaunchRequest request;
    request.projectRoot = context.projectRoot;
    request.trusted = context.trusted;
    request.configuredShell = settings.shell;
    request.workingDirectory = workingDirectory;
    Result<term::TerminalLaunch> launch = term::planTerminalLaunch(request, m_env.probe);
    if (!launch) {
        return failWith(std::move(launch.error()));
    }

    auto* view = new TerminalView(m_stack);
    applyAppearance(*view);
    m_stack->addWidget(view);
    // The grid follows the stack's size before the shell starts, so it starts at the right size.
    view->resize(m_stack->size());
    if (Result<void> started = view->start(*m_env.launcher, launch->spec, settings.scrollbackLines); !started) {
        delete view;
        return failWith(std::move(started.error()));
    }
    Tab tab;
    tab.view = view;
    tab.defaultName = m_nextNumber == 1 ? text("Local") : text("Local (%1)").arg(m_nextNumber);
    ++m_nextNumber;
    m_tabState.push_back(tab);
    const int index = m_tabs->addTab(tab.defaultName);
    connect(view, &TerminalView::titleChanged, this, [this, view] { refreshTab(indexOf(view)); });
    // A tab whose shell exited closes, as in IntelliJ (after the last output was painted).
    connect(view, &TerminalView::exited, this,
            [this, view] { QTimer::singleShot(0, this, [this, view] { closeTab(indexOf(view)); }); });
    m_openedOnce = true;
    setCurrentIndex(index);
    view->setFocus(Qt::OtherFocusReason);
    emit tabsChanged();
    return index;
}

void TerminalToolWindow::closeTab(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    if (m_renaming >= 0) {
        finishRename(false);
    }
    const Tab tab = m_tabState[static_cast<std::size_t>(index)];
    m_tabState.erase(m_tabState.begin() + index);
    m_tabs->removeTab(index);
    // Deleting the view closes its session, which ends the shell's tree.
    delete tab.view.data();
    if (count() == 0) {
        showMessage(QString());
    } else if (TerminalView* current = currentView()) {
        m_stack->setCurrentWidget(current);
    }
    emit tabsChanged();
}

void TerminalToolWindow::closeAllTabs() {
    while (count() > 0) {
        closeTab(count() - 1);
    }
}

void TerminalToolWindow::shutdown() {
    m_stopped = true;
    closeAllTabs();
}

void TerminalToolWindow::renameTab(int index, const QString& name) {
    if (index < 0 || index >= count()) {
        return;
    }
    m_tabState[static_cast<std::size_t>(index)].customName = name.trimmed();
    refreshTab(index);
    emit tabsChanged();
}

void TerminalToolWindow::refreshTab(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    const Tab& tab = m_tabState[static_cast<std::size_t>(index)];
    m_tabs->setTabText(index, tabName(index));
    m_tabs->setTabToolTip(index, tab.view != nullptr ? tab.view->title() : QString());
}

void TerminalToolWindow::beginRename() {
    const int index = currentIndex();
    if (index < 0) {
        return;
    }
    if (m_renameEditor == nullptr) {
        m_renameEditor = new QLineEdit(m_tabs);
        m_renameEditor->setObjectName(QStringLiteral("terminalRenameEditor"));
        m_renameEditor->installEventFilter(this);
        connect(m_renameEditor, &QLineEdit::returnPressed, this, [this] { finishRename(true); });
    }
    m_renaming = index;
    m_renameEditor->setText(tabName(index));
    m_renameEditor->setGeometry(m_tabs->tabRect(index));
    m_renameEditor->selectAll();
    m_renameEditor->show();
    m_renameEditor->setFocus(Qt::OtherFocusReason);
}

void TerminalToolWindow::finishRename(bool apply) {
    if (m_renaming < 0 || m_renameEditor == nullptr) {
        return;
    }
    const int index = m_renaming;
    m_renaming = -1;
    m_renameEditor->hide();
    if (apply) {
        renameTab(index, m_renameEditor->text());
    }
    if (TerminalView* current = currentView()) {
        current->setFocus(Qt::OtherFocusReason);
    }
}

bool TerminalToolWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_renameEditor && m_renaming >= 0) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            finishRename(false);
            return true;
        }
        if (event->type() == QEvent::FocusOut) {
            finishRename(true);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TerminalToolWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // The first show opens a shell, as IntelliJ's terminal does; later shows keep what is there.
    if (!m_openedOnce && count() == 0) {
        m_openedOnce = true;
        QTimer::singleShot(0, this, [this] {
            if (!m_stopped && count() == 0) {
                static_cast<void>(newTab());
            }
        });
    }
}

int TerminalToolWindow::count() const { return static_cast<int>(m_tabState.size()); }

int TerminalToolWindow::currentIndex() const { return count() == 0 ? -1 : m_tabs->currentIndex(); }

void TerminalToolWindow::setCurrentIndex(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    m_tabs->setCurrentIndex(index);
    if (TerminalView* view = m_tabState[static_cast<std::size_t>(index)].view) {
        m_stack->setCurrentWidget(view);
    }
}

TerminalView* TerminalToolWindow::view(int index) const {
    return index >= 0 && index < count() ? m_tabState[static_cast<std::size_t>(index)].view.data() : nullptr;
}

TerminalView* TerminalToolWindow::currentView() const { return view(currentIndex()); }

QString TerminalToolWindow::tabName(int index) const {
    if (index < 0 || index >= count()) {
        return {};
    }
    const Tab& tab = m_tabState[static_cast<std::size_t>(index)];
    return tab.customName.isEmpty() ? tab.defaultName : tab.customName;
}

int TerminalToolWindow::indexOf(const TerminalView* view) const {
    for (std::size_t i = 0; i < m_tabState.size(); ++i) {
        if (m_tabState[i].view == view) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// --- TerminalIntegration ---

TerminalIntegration::TerminalIntegration(ShellWindow& shell, TerminalEnvironment environment, QObject* parent)
    : QObject(parent), m_shell(shell) {
    namespace a = terminal_actions;
    ToolWindowSpec spec;
    spec.id = std::string(tool_windows::kTerminal);
    spec.title = text("Terminal");
    spec.side = ToolWindowSide::Bottom;
    spec.iconId = std::string(icons::kToolwindowTerminal);
    spec.createContent = [environment = std::move(environment)]() -> QWidget* {
        return new TerminalToolWindow(environment);
    };
    addAction(a::kTerminalWindow, "Terminal", [this] { m_shell.toolWindows().activate(tool_windows::kTerminal); });
    if (const std::optional<input::ActionId> id = m_shell.environment().registry.find(a::kTerminalWindow)) {
        const ShellEnvironment& env = m_shell.environment();
        if (const std::optional<input::Sequence> shortcut = env.keymap.primaryShortcut(*id)) {
            const std::string display = input::toDisplayText(*shortcut, env.keymap.layout(), env.keymap.platform());
            spec.shortcutText = fromUtf8(display);
        }
    }
    if (Result<void> added = m_shell.toolWindows().add(std::move(spec)); !added) {
        log::warn("ui", "Terminal tool window: {}", added.error().message());
    }
    addAction(a::kNewTab, "New Terminal Tab", [this] {
        if (TerminalToolWindow* window = showToolWindow(true)) {
            static_cast<void>(window->newTab());
        }
    });
    const auto hasTab = [this] {
        const TerminalToolWindow* window = toolWindow();
        return window != nullptr && window->count() > 0;
    };
    addAction(
        a::kCloseTab, "Close Terminal Tab",
        [this] {
            if (TerminalToolWindow* window = toolWindow()) {
                window->closeTab(window->currentIndex());
            }
        },
        hasTab);
    addAction(
        a::kRenameTab, "Rename Terminal Tab",
        [this] {
            if (TerminalToolWindow* window = showToolWindow(false)) {
                window->beginRename();
            }
        },
        hasTab);
    addAction(a::kOpenInTerminal, "Open in Terminal",
              [this] { static_cast<void>(openInTerminal(m_folderProvider ? m_folderProvider() : std::string())); });
}

TerminalIntegration::~TerminalIntegration() = default;

void TerminalIntegration::addAction(std::string_view id, std::string_view name, std::function<void()> run,
                                    std::function<bool()> enabled) {
    input::ActionSpec spec;
    spec.id = std::string(id);
    spec.name = std::string(name);
    spec.group = "Terminal";
    spec.context = kWindowRule;
    const QPointer<TerminalIntegration> self(this);
    spec.handler = [self, run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) {
        if (self != nullptr && run) {
            run();
        }
    };
    spec.enabled = [self, enabled = std::move(enabled)](const input::ActionContext&) {
        return self != nullptr && (!enabled || enabled());
    };
    if (Result<input::ActionId> added = m_shell.environment().registry.add(std::move(spec)); !added) {
        log::warn("ui", "terminal action {}: {}", id, added.error().message());
    }
}

TerminalToolWindow* TerminalIntegration::toolWindow() const {
    return qobject_cast<TerminalToolWindow*>(m_shell.toolWindows().content(tool_windows::kTerminal));
}

TerminalToolWindow* TerminalIntegration::showToolWindow(bool focus) {
    m_shell.toolWindows().show(tool_windows::kTerminal, focus);
    return toolWindow();
}

Result<int> TerminalIntegration::openInTerminal(const std::string& folder) {
    TerminalToolWindow* window = showToolWindow(true);
    if (window == nullptr) {
        return fail(ErrorCode::NotFound, "the Terminal tool window is not available");
    }
    return window->newTab(folder);
}

} // namespace frost::ui
