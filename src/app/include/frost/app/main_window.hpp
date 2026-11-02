// SPDX-License-Identifier: Apache-2.0
//
// MainWindow: FrostIDE's top-level window, the Rider-style shell (frost::ui::ShellWindow, ADR 0038)
// built on the composition root (frost::app::Workbench, ADR 0044), which attaches the navigation
// workbench (ADR 0042), the tool windows and the language features to it. main() builds the
// workbench over the persistent stores first and passes its environment; default-constructed
// (tests, the self-test), the window owns an in-memory workbench of its own.
#pragma once

#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include <memory>

namespace frost::app {

class Workbench;

class MainWindow final : public ui::ShellWindow {
    Q_OBJECT

public:
    /*
     * Creates the window hidden over its own workbench, attached: settings and trust in memory,
     * nothing persisted, no clangd, the OS keyboard layout followed. Call show() to display it.
     * Must run on the GUI thread.
     */
    explicit MainWindow(QWidget* parent = nullptr);
    // Creates the window over an environment the caller owns (Workbench::environment()); the caller
    // attaches it (Workbench::attach) and destroys it before the workbench.
    explicit MainWindow(ui::ShellEnvironment environment, QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete ("QObject instances have identity and cannot be copied");
    MainWindow& operator=(const MainWindow&) = delete ("QObject instances have identity and cannot be copied");

    // The navigation workbench of the owned composition root, or nullptr (not owned).
    [[nodiscard]] ui::Workbench* workbench() const noexcept;
    // The composition root this window owns (default construction), or nullptr.
    [[nodiscard]] Workbench* ownedWorkbench() const noexcept;

private:
    struct Owned;
    MainWindow(std::unique_ptr<Owned> owned, QWidget* parent);

    // The owned workbench and its stores; destroyed after the shell is torn down (~MainWindow).
    std::unique_ptr<Owned> m_owned;
};

} // namespace frost::app
