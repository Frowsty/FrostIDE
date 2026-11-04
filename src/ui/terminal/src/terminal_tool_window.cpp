// SPDX-License-Identifier: Apache-2.0
//
// TerminalToolWindow and TerminalIntegration (frost/ui/terminal/terminal_tool_window.hpp).

#include "frost/ui/terminal/terminal_tool_window.hpp"

#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/io/path.hpp"
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

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QShowEvent>
#include <QSplitter>
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

std::string joinOrEmpty(std::string_view dir, std::string_view name) {
    auto joined = io::joinPath(dir, name);
    return joined ? std::move(*joined) : std::string();
}

std::string searchPathEntries(std::string_view requested, std::string_view pathEnv,
                              const std::function<bool(const std::string&)>& usable) {
    std::string_view rest = pathEnv;
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    while (!rest.empty()) {
        const std::size_t end = rest.find(sep);
        std::string_view entry = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
            entry = entry.substr(1, entry.size() - 2);
        }
        if (!entry.empty() && io::isAbsolutePath(entry)) {
            if (const std::string candidate = joinOrEmpty(entry, requested); usable(candidate)) {
                return candidate;
            }
        }
    }
    return {};
}

std::string resolveShellExecutable(std::string_view requested, const term::ShellProbe& probe,
                                   std::string_view excludeRoot) {
    if (requested.empty() || io::isAbsolutePath(requested)) {
        return std::string(requested);
    }
    const auto usable = [&](const std::string& path) {
        return !path.empty() && io::isAbsolutePath(path) && !term::isInsideRoot(path, excludeRoot) &&
               probe.isExecutable && probe.isExecutable(path);
    };
    if (auto programFiles = probe.environment("ProgramFiles")) {
        if (requested == "pwsh.exe") {
            if (const std::string pwsh = joinOrEmpty(joinOrEmpty(*programFiles, "PowerShell\\7"), "pwsh.exe");
                usable(pwsh)) {
                return pwsh;
            }
        } else if (requested == "bash.exe") {
            for (const char* sub : {"Git\\bin", "Git\\usr\\bin"}) {
                if (const std::string bash = joinOrEmpty(joinOrEmpty(*programFiles, sub), "bash.exe"); usable(bash)) {
                    return bash;
                }
            }
        }
    }
    if (!probe.systemDirectory.empty()) {
        if (requested == "powershell.exe") {
            const std::string ps =
                joinOrEmpty(joinOrEmpty(probe.systemDirectory, "WindowsPowerShell\\v1.0"), "powershell.exe");
            if (usable(ps)) {
                return ps;
            }
        }
        if (const std::string sysCandidate = joinOrEmpty(probe.systemDirectory, requested); usable(sysCandidate)) {
            return sysCandidate;
        }
    }
    if (auto path = probe.environment("PATH")) {
        if (const std::string fromPath = searchPathEntries(requested, *path, usable); !fromPath.empty()) {
            return fromPath;
        }
    }
    if (!probe.systemDirectory.empty()) {
        if (const std::string fallback = joinOrEmpty(probe.systemDirectory, requested); !fallback.empty()) {
            return fallback;
        }
    }
#ifdef _WIN32
    return "C:\\Windows\\System32\\" + std::string(requested);
#else
    return "/bin/" + std::string(requested);
#endif
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

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(0);
    setupHeaderBar(*header);
    setupFindBar();

    m_stack = new QStackedWidget(this);
    m_message = new QLabel(m_stack);
    m_message->setObjectName(QStringLiteral("terminalMessage"));
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_stack->addWidget(m_message);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_findBar);
    layout->addWidget(m_stack, 1);

    if (m_env.appearance != nullptr) {
        connect(m_env.appearance, &AppearanceManager::appearanceChanged, this, [this] {
            for (const Tab& tab : m_tabState) {
                for (const QPointer<TerminalView>& pane : tab.panes) {
                    if (pane != nullptr) {
                        applyAppearance(*pane);
                    }
                }
            }
        });
    }
    showMessage(QString());
}

TerminalToolWindow::~TerminalToolWindow() = default;

void TerminalToolWindow::setupHeaderBar(QHBoxLayout& header) {
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

    m_shellMenu = new QMenu(this);
    m_shellMenu->setObjectName(QStringLiteral("terminalShellMenu"));
    m_shellMenu->addAction(text("Default Shell"), this, [this] { static_cast<void>(newTab()); });
    m_shellMenu->addSeparator();
    const struct ShellEntry {
        const char* label;
        const char* exe;
    } shells[] = {
        {"PowerShell (pwsh.exe)", "pwsh.exe"},
        {"Windows PowerShell (powershell.exe)", "powershell.exe"},
        {"Command Prompt (cmd.exe)", "cmd.exe"},
        {"Git Bash (bash.exe)", "bash.exe"},
        {"WSL (wsl.exe)", "wsl.exe"},
    };
    for (const auto& entry : shells) {
        m_shellMenu->addAction(text(entry.label), this,
                               [this, exe = std::string(entry.exe)] { static_cast<void>(newTabWithShell(exe)); });
    }
    m_newTab->setMenu(m_shellMenu);
    m_newTab->setPopupMode(QToolButton::MenuButtonPopup);

    m_shellSelector = new QToolButton(this);
    m_shellSelector->setObjectName(QStringLiteral("terminalShellSelector"));
    m_shellSelector->setText(QStringLiteral("▾"));
    m_shellSelector->setToolTip(text("Select Shell Profile"));
    m_shellSelector->setAutoRaise(true);
    m_shellSelector->setFocusPolicy(Qt::NoFocus);
    m_shellSelector->setMenu(m_shellMenu);
    m_shellSelector->setPopupMode(QToolButton::InstantPopup);

    m_splitRight = new QToolButton(this);
    m_splitRight->setObjectName(QStringLiteral("terminalSplitRight"));
    m_splitRight->setText(text("Split Right"));
    m_splitRight->setToolTip(text("Split Terminal Right"));
    m_splitRight->setAutoRaise(true);
    m_splitRight->setFocusPolicy(Qt::NoFocus);

    header.addWidget(m_tabs);
    header.addWidget(m_newTab);
    header.addWidget(m_shellSelector);
    header.addWidget(m_splitRight);
    header.addStretch(1);

    connect(m_newTab, &QToolButton::clicked, this, [this] { static_cast<void>(newTab()); });
    connect(m_splitRight, &QToolButton::clicked, this, [this] { static_cast<void>(splitCurrentTab()); });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, [this](int index) { closeTab(index); });
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < count() && m_tabState[static_cast<std::size_t>(index)].splitter != nullptr) {
            m_stack->setCurrentWidget(m_tabState[static_cast<std::size_t>(index)].splitter);
        }
    });
    connect(m_tabs, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index >= 0) {
            setCurrentIndex(index);
            beginRename();
        }
    });
}

void TerminalToolWindow::setupFindBar() {
    m_findBar = new QWidget(this);
    m_findBar->setObjectName(QStringLiteral("terminalFindBar"));
    auto* findLayout = new QHBoxLayout(m_findBar);
    findLayout->setContentsMargins(6, 2, 6, 2);
    findLayout->setSpacing(4);

    m_findInput = new QLineEdit(m_findBar);
    m_findInput->setObjectName(QStringLiteral("terminalFindInput"));
    m_findInput->setPlaceholderText(text("Find in terminal..."));
    m_findInput->installEventFilter(this);

    m_findPrev = new QToolButton(m_findBar);
    m_findPrev->setObjectName(QStringLiteral("terminalFindPrev"));
    m_findPrev->setText(text("Prev"));
    m_findPrev->setToolTip(text("Previous Match (Shift+Enter)"));
    m_findPrev->setAutoRaise(true);

    m_findNext = new QToolButton(m_findBar);
    m_findNext->setObjectName(QStringLiteral("terminalFindNext"));
    m_findNext->setText(text("Next"));
    m_findNext->setToolTip(text("Next Match (Enter)"));
    m_findNext->setAutoRaise(true);

    m_findStatus = new QLabel(m_findBar);
    m_findStatus->setObjectName(QStringLiteral("terminalFindStatus"));

    m_findClose = new QToolButton(m_findBar);
    m_findClose->setObjectName(QStringLiteral("terminalFindClose"));
    m_findClose->setText(QStringLiteral("×"));
    m_findClose->setToolTip(text("Close Find Bar (Esc)"));
    m_findClose->setAutoRaise(true);

    findLayout->addWidget(m_findInput, 1);
    findLayout->addWidget(m_findPrev);
    findLayout->addWidget(m_findNext);
    findLayout->addWidget(m_findStatus);
    findLayout->addWidget(m_findClose);
    m_findBar->hide();

    connect(m_findInput, &QLineEdit::textChanged, this, [this] { static_cast<void>(runFind(true, true)); });
    connect(m_findInput, &QLineEdit::returnPressed, this, [this] { static_cast<void>(findNext()); });
    connect(m_findNext, &QToolButton::clicked, this, [this] { static_cast<void>(findNext()); });
    connect(m_findPrev, &QToolButton::clicked, this, [this] { static_cast<void>(findPrevious()); });
    connect(m_findClose, &QToolButton::clicked, this, &TerminalToolWindow::hideFindBar);
}

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

Result<TerminalView*> TerminalToolWindow::createAndStartView(QWidget* parentWidget, const std::string& shellOverride,
                                                             const std::string& workingDirectory) {
    if (m_stopped || (m_env.available && !m_env.available())) {
        return fail(ErrorCode::Cancelled, "terminal is shutting down");
    }
    const auto failWith = [this](Error error) -> Result<TerminalView*> {
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
    request.configuredShell = shellOverride.empty()
                                  ? settings.shell
                                  : resolveShellExecutable(shellOverride, m_env.probe, context.projectRoot);
    request.workingDirectory = workingDirectory;
    Result<term::TerminalLaunch> launch = term::planTerminalLaunch(request, m_env.probe);
    if (!launch) {
        return failWith(std::move(launch.error()));
    }

    auto* view = new TerminalView(parentWidget);
    applyAppearance(*view);
    if (auto* splitter = qobject_cast<QSplitter*>(parentWidget)) {
        splitter->addWidget(view);
    }
    // The grid follows the parent's size before the shell starts, so it starts at the right size.
    view->resize(parentWidget->size());
    if (Result<void> started = view->start(*m_env.launcher, launch->spec, settings.scrollbackLines); !started) {
        delete view;
        return failWith(std::move(started.error()));
    }
    attachPaneSignals(view);
    return view;
}

void TerminalToolWindow::attachPaneSignals(TerminalView* pane) {
    connect(pane, &TerminalView::titleChanged, this, [this, pane] { refreshTab(indexOf(pane)); });
    connect(pane, &TerminalView::findRequested, this, &TerminalToolWindow::showFindBar);
    // A pane whose shell exited closes, as in IntelliJ (after the last output was painted).
    connect(pane, &TerminalView::exited, this,
            [this, pane] { QTimer::singleShot(0, this, [this, pane] { onPaneExited(pane); }); });
}

void TerminalToolWindow::onPaneExited(TerminalView* pane) {
    const int tabIdx = indexOf(pane);
    if (tabIdx < 0) {
        return;
    }
    Tab& tab = m_tabState[static_cast<std::size_t>(tabIdx)];
    if (tab.panes.size() <= 1) {
        closeTab(tabIdx);
        return;
    }
    std::erase(tab.panes, QPointer<TerminalView>(pane));
    if (tab.view == pane) {
        tab.view = tab.panes.empty() ? nullptr : tab.panes.front();
    }
    delete pane;
    if (tab.view != nullptr) {
        tab.view->setFocus(Qt::OtherFocusReason);
    }
    refreshTab(tabIdx);
    emit tabsChanged();
}

Result<int> TerminalToolWindow::newTab(const std::string& workingDirectory) {
    return newTabWithShell({}, workingDirectory);
}

Result<int> TerminalToolWindow::newTabWithShell(const std::string& shellExecutable,
                                                const std::string& workingDirectory) {
    auto* splitter = new QSplitter(Qt::Horizontal, m_stack);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(2);
    m_stack->addWidget(splitter);
    splitter->resize(m_stack->size());

    Result<TerminalView*> created = createAndStartView(splitter, shellExecutable, workingDirectory);
    if (!created) {
        delete splitter;
        return std::unexpected(std::move(created.error()));
    }
    TerminalView* view = *created;
    Tab tab;
    tab.splitter = splitter;
    tab.view = view;
    tab.panes.push_back(view);
    tab.defaultName = m_nextNumber == 1 ? text("Local") : text("Local (%1)").arg(m_nextNumber);
    ++m_nextNumber;
    m_tabState.push_back(std::move(tab));
    const int index = m_tabs->addTab(m_tabState.back().defaultName);
    m_openedOnce = true;
    setCurrentIndex(index);
    view->setFocus(Qt::OtherFocusReason);
    emit tabsChanged();
    return index;
}

Result<TerminalView*> TerminalToolWindow::splitTab(int index, const std::string& workingDirectory) {
    if (index < 0 || index >= count()) {
        return fail(ErrorCode::NotFound, "no terminal tab at index");
    }
    Tab& tab = m_tabState[static_cast<std::size_t>(index)];
    if (tab.splitter == nullptr) {
        return fail(ErrorCode::Internal, "terminal tab splitter is missing");
    }
    Result<TerminalView*> created = createAndStartView(tab.splitter, {}, workingDirectory);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }
    TerminalView* pane = *created;
    tab.panes.push_back(pane);
    setCurrentIndex(index);
    pane->setFocus(Qt::OtherFocusReason);
    emit tabsChanged();
    return pane;
}

Result<TerminalView*> TerminalToolWindow::splitCurrentTab(const std::string& workingDirectory) {
    if (count() == 0) {
        Result<int> opened = newTab(workingDirectory);
        if (!opened) {
            return std::unexpected(std::move(opened.error()));
        }
    }
    return splitTab(currentIndex(), workingDirectory);
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
    // Deleting the splitter deletes its TerminalView panes, closing their sessions.
    if (tab.splitter != nullptr) {
        delete tab.splitter.data();
    } else {
        delete tab.view.data();
    }
    if (count() == 0) {
        showMessage(QString());
    } else if (const int cur = currentIndex(); cur >= 0 && cur < count()) {
        if (QSplitter* splitter = m_tabState[static_cast<std::size_t>(cur)].splitter) {
            m_stack->setCurrentWidget(splitter);
        }
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

void TerminalToolWindow::showFindBar() {
    if (m_findBar == nullptr) {
        return;
    }
    m_findBar->show();
    if (TerminalView* active = currentView(); active != nullptr && active->hasSelection()) {
        const QString sel = active->selectedText().trimmed();
        if (!sel.isEmpty() && !sel.contains(QLatin1Char('\n'))) {
            m_findInput->setText(sel);
        }
    }
    m_findInput->selectAll();
    m_findInput->setFocus(Qt::ShortcutFocusReason);
}

void TerminalToolWindow::hideFindBar() {
    if (m_findBar == nullptr) {
        return;
    }
    m_findBar->hide();
    if (TerminalView* active = currentView()) {
        active->setFocus(Qt::OtherFocusReason);
    }
}

bool TerminalToolWindow::isFindBarVisible() const noexcept { return m_findBar != nullptr && !m_findBar->isHidden(); }

bool TerminalToolWindow::findNext() { return runFind(true, false); }

bool TerminalToolWindow::findPrevious() { return runFind(false, false); }

bool TerminalToolWindow::runFind(bool forward, bool stayAtCurrent) {
    TerminalView* active = currentView();
    const QString query = m_findInput != nullptr ? m_findInput->text() : QString();
    if (active == nullptr || query.isEmpty()) {
        if (active != nullptr) {
            active->clearSelection();
        }
        if (m_findStatus != nullptr) {
            m_findStatus->clear();
        }
        return false;
    }
    const bool found = active->findText(query, forward, stayAtCurrent);
    if (m_findStatus != nullptr) {
        m_findStatus->setText(found ? text("Found") : text("No matches"));
    }
    return found;
}

void TerminalToolWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F && (event->modifiers() & Qt::ControlModifier) != 0 &&
        (event->modifiers() & (Qt::AltModifier | Qt::ShiftModifier)) == 0) {
        showFindBar();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && isFindBarVisible()) {
        hideFindBar();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
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
    if (watched == m_findInput && event->type() == QEvent::KeyPress) {
        const auto* keyEvent = static_cast<const QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            hideFindBar();
            return true;
        }
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
            (keyEvent->modifiers() & Qt::ShiftModifier) != 0) {
            static_cast<void>(findPrevious());
            return true;
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
    if (QSplitter* splitter = m_tabState[static_cast<std::size_t>(index)].splitter) {
        m_stack->setCurrentWidget(splitter);
    }
}

TerminalView* TerminalToolWindow::view(int index) const {
    return index >= 0 && index < count() ? m_tabState[static_cast<std::size_t>(index)].view.data() : nullptr;
}

TerminalView* TerminalToolWindow::currentView() const {
    const int idx = currentIndex();
    if (idx < 0 || idx >= count()) {
        return nullptr;
    }
    const Tab& tab = m_tabState[static_cast<std::size_t>(idx)];
    for (const QPointer<TerminalView>& pane : tab.panes) {
        if (pane != nullptr && pane->hasFocus()) {
            return pane.data();
        }
    }
    return tab.view.data();
}

int TerminalToolWindow::paneCount(int index) const {
    if (index < 0 || index >= count()) {
        return 0;
    }
    return static_cast<int>(m_tabState[static_cast<std::size_t>(index)].panes.size());
}

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
        for (const QPointer<TerminalView>& pane : m_tabState[i].panes) {
            if (pane == view) {
                return static_cast<int>(i);
            }
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
    addAction(
        a::kSplitRight, "Split Terminal Right",
        [this] {
            if (TerminalToolWindow* window = showToolWindow(true)) {
                static_cast<void>(window->splitCurrentTab());
            }
        },
        hasTab);
    addAction(
        a::kFind, "Find in Terminal",
        [this] {
            if (TerminalToolWindow* window = showToolWindow(false)) {
                window->showFindBar();
            }
        },
        hasTab);
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
