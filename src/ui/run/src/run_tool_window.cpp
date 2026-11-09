// SPDX-License-Identifier: Apache-2.0
//
// RunToolWindow (see frost/ui/run_tool_window.hpp): the tabs of the Run tool window v2, the tab
// choice for a new run and the routing of each console's buttons to its run's handlers.

#include "frost/ui/run_tool_window.hpp"

#include "frost/ui/run_console.hpp"

#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::ui {

build::RunOutcome runOutcomeFor(const run::RunResult& result) {
    build::RunOutcome outcome;
    outcome.error = result.error;
    outcome.buildFailed = result.beforeLaunchFailed;
    outcome.stopped = result.stopped;
    outcome.exitCode = result.exitCode;
    outcome.duration = result.duration;
    return outcome;
}

RunToolWindow::RunToolWindow(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("runToolWindow"));
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("runTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tabs);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) { closeTab(index); });
    addTab();
}

RunToolWindow::~RunToolWindow() = default;

RunToolWindow::Tab* RunToolWindow::tabOf(const RunConsole* console) {
    const auto it = std::ranges::find(m_tabState, console, &Tab::console);
    return it == m_tabState.end() ? nullptr : &*it;
}

const RunToolWindow::Tab* RunToolWindow::tabOf(const RunConsole* console) const {
    const auto it = std::ranges::find(m_tabState, console, &Tab::console);
    return it == m_tabState.end() ? nullptr : &*it;
}

RunConsole* RunToolWindow::addTab() {
    auto* console = new RunConsole();
    console->setObjectName(QStringLiteral("runConsole"));
    m_tabState.push_back({.console = console, .key = {}, .title = tr("Run"), .handlers = {}});
    m_tabs->addTab(console, tr("Run"));
    // The console's controls call whatever handlers the tab holds at the time.
    connect(console, &RunConsole::stopRequested, this, [this, console] {
        if (const Tab* tab = tabOf(console); tab && tab->handlers.stop) {
            tab->handlers.stop();
        }
    });
    connect(console, &RunConsole::rerunRequested, this, [this, console] {
        if (const Tab* tab = tabOf(console); tab && tab->handlers.rerun) {
            const auto rerun = tab->handlers.rerun; // The handler may open this tab again.
            rerun();
        } else if (tab == nullptr || tab->key.isEmpty()) {
            emit rerunWithoutRunRequested(); // A tab that has not run anything yet (not a detached one).
        }
    });
    connect(console, &RunConsole::inputSubmitted, this, [this, console](const QString& line) {
        if (const Tab* tab = tabOf(console); tab && tab->handlers.input) {
            tab->handlers.input(line);
        }
    });
    connect(console, &RunConsole::activeChanged, this, [this, console] {
        if (const Tab* tab = tabOf(console)) {
            refreshTitle(*tab);
        }
        emit tabsChanged();
    });
    return console;
}

void RunToolWindow::refreshTitle(const Tab& tab) {
    const int index = m_tabs->indexOf(tab.console);
    if (index < 0) {
        return;
    }
    const bool active = tab.console->active();
    m_tabs->setTabText(index, active ? tr("%1 (running)").arg(tab.title) : tab.title);
    m_tabs->setTabToolTip(index, active ? tr("%1: running").arg(tab.title) : tab.title);
}

RunConsole* RunToolWindow::openRun(const QString& key, const QString& title, RunTabHandlers handlers) {
    RunConsole* chosen = nullptr;
    // 1. The newest inactive tab of the same key.
    for (auto it = m_tabState.rbegin(); it != m_tabState.rend() && chosen == nullptr; ++it) {
        if (!key.isEmpty() && it->key == key && !it->console->active()) {
            chosen = it->console;
        }
    }
    // 2. The empty first tab.
    if (chosen == nullptr && !m_tabState.empty() && !m_tabState.front().console->used()) {
        chosen = m_tabState.front().console;
    }
    // 3. A new tab.
    if (chosen == nullptr) {
        chosen = addTab();
    }
    Tab* tab = tabOf(chosen);
    tab->key = key;
    tab->title = title.isEmpty() ? tr("Run") : title;
    tab->handlers = std::move(handlers);
    m_tabs->setCurrentWidget(chosen);
    chosen->beginRun(title);
    refreshTitle(*tab);
    emit tabsChanged();
    return chosen;
}

RunConsole* RunToolWindow::currentConsole() const {
    auto* current = qobject_cast<RunConsole*>(m_tabs->currentWidget());
    return current != nullptr ? current : m_tabState.front().console;
}

RunConsole* RunToolWindow::consoleFor(const QString& key) const {
    for (auto it = m_tabState.rbegin(); it != m_tabState.rend(); ++it) {
        if (it->key == key) {
            return it->console;
        }
    }
    return nullptr;
}

QString RunToolWindow::keyOf(const RunConsole* console) const {
    const Tab* tab = tabOf(console);
    return tab ? tab->key : QString();
}

std::vector<RunConsole*> RunToolWindow::consoles() const {
    std::vector<RunConsole*> out;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* console = qobject_cast<RunConsole*>(m_tabs->widget(i))) {
            out.push_back(console);
        }
    }
    return out;
}

int RunToolWindow::count() const { return m_tabs->count(); }

bool RunToolWindow::stopCurrent() {
    RunConsole* console = currentConsole();
    const Tab* tab = tabOf(console);
    if (tab == nullptr || !console->active() || !tab->handlers.stop) {
        return false;
    }
    tab->handlers.stop();
    return true;
}

bool RunToolWindow::rerunCurrent() {
    const Tab* tab = tabOf(currentConsole());
    if (tab == nullptr || !tab->handlers.rerun) {
        return false;
    }
    const auto rerun = tab->handlers.rerun;
    rerun();
    return true;
}

void RunToolWindow::detach(const RunConsole* console) {
    if (Tab* tab = tabOf(console)) {
        tab->handlers = {};
    }
}

void RunToolWindow::closeTab(int index) {
    auto* console = qobject_cast<RunConsole*>(m_tabs->widget(index));
    Tab* tab = tabOf(console);
    if (tab == nullptr) {
        return;
    }
    // A closed tab's run ends with it, as in Rider (which asks first; Stop is the honest default here).
    if (console->active() && tab->handlers.stop) {
        tab->handlers.stop();
    }
    std::erase_if(m_tabState, [console](const Tab& entry) { return entry.console == console; });
    m_tabs->removeTab(index);
    console->deleteLater();
    if (m_tabState.empty()) {
        addTab();
    }
    emit tabsChanged();
}

} // namespace frost::ui
