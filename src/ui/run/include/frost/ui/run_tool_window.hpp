// SPDX-License-Identifier: Apache-2.0
//
// RunToolWindow: the Run tool window v2 (RUN-3, absorbs RUN-4; PLAN.md Appendix C R2), as in
// Rider: one tab per run, each a RunConsole (streamed output, the input line, Stop, Rerun and the
// exit-code line). Whoever starts a run (RunController for run configurations and the MSBuild
// startup project, the CMake integration for CMake targets) opens its tab here and feeds it.
//
/*
 * Tabs. openRun(key, ...) picks the tab for a new run of `key` (a configuration id, a project):
 *   1. the newest tab of the same key whose run is not active (a rerun replaces its old output,
 *      as Rider reuses a configuration's tab);
 *   2. else the window's first tab if no run ever used it (the empty tab the window starts with);
 *   3. else a new tab.
 * Two runs of different keys, or a run of a key whose tab is still active, get tabs of their own.
 * The chosen tab becomes current, its console's beginRun(title) runs, and its handlers replace the
 * tab's previous ones: the console's Stop, Rerun and input line call them. A tab's title is the
 * run's title with " (running)" while its console is active().
 *
 * Closing a tab stops its run first (its stop handler) and removes it; closing the last tab leaves
 * a fresh empty one, so currentConsole() is never null.
 *
 * Thread safety: a QWidget; GUI thread only. Lifetime: the consoles are children of the window.
 */
#pragma once

#include "frost/build/run_service.hpp"
#include "frost/run/run_manager.hpp"

#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

class QTabWidget;

namespace frost::ui {

class RunConsole;

// What a tab's buttons and input line call (header comment); every member optional.
struct RunTabHandlers {
    std::function<void()> stop{};
    std::function<void()> rerun{};
    std::function<void(const QString& line)> input{};
};

// The RunOutcome the console's closing line reads (run_console.hpp) for a RunManager result: the
// exit code, a Stop, a failed or cancelled before-launch step (shown like a failed build) or the
// launch error. Pure.
[[nodiscard]] build::RunOutcome runOutcomeFor(const run::RunResult& result);

class RunToolWindow final : public QWidget {
    Q_OBJECT

public:
    explicit RunToolWindow(QWidget* parent = nullptr);
    ~RunToolWindow() override;
    RunToolWindow(const RunToolWindow&) = delete ("a QWidget");
    RunToolWindow& operator=(const RunToolWindow&) = delete ("a QWidget");

    // Opens the tab of a new run (header comment, "Tabs") and returns its console, already begun.
    RunConsole* openRun(const QString& key, const QString& title, RunTabHandlers handlers = {});

    // The console of the current tab; never null.
    [[nodiscard]] RunConsole* currentConsole() const;
    // The newest tab's console for `key`, or null.
    [[nodiscard]] RunConsole* consoleFor(const QString& key) const;
    // The key `console`'s tab was last opened with ("" for the empty first tab or a foreign console).
    [[nodiscard]] QString keyOf(const RunConsole* console) const;
    // Every tab's console, left to right.
    [[nodiscard]] std::vector<RunConsole*> consoles() const;
    [[nodiscard]] int count() const;
    [[nodiscard]] QTabWidget* tabs() const noexcept { return m_tabs; }

    // Calls the current tab's stop handler if its run is active; returns whether it did.
    bool stopCurrent();
    // Calls the current tab's rerun handler if it has one; returns whether it did.
    bool rerunCurrent();
    // Closes tab `index` (header comment). Out of range: nothing.
    void closeTab(int index);

signals:
    // A tab was opened or closed, or a run in one began or finished.
    void tabsChanged();
    // Rerun was clicked in a tab with no rerun handler (the empty first tab): the owner runs the
    // selected configuration (RunController::rerun).
    void rerunWithoutRunRequested();

private:
    struct Tab {
        RunConsole* console = nullptr;
        QString key;
        QString title;
        RunTabHandlers handlers;
    };

    Tab* tabOf(const RunConsole* console);
    const Tab* tabOf(const RunConsole* console) const;
    RunConsole* addTab();
    void refreshTitle(const Tab& tab);

    QTabWidget* m_tabs = nullptr;
    std::vector<Tab> m_tabState;
};

} // namespace frost::ui
