// SPDX-License-Identifier: Apache-2.0
//
// MainWindow: FrostIDE's top-level window, the Rider-style shell (frost::ui::ShellWindow, ADR 0038)
// with the navigation workbench (frost::ui::Workbench, ADR 0042) over the application's services.
// Default-constructed, it owns its own in-memory AppServices (tests and the self-test); main() builds
// the persistent services first and passes them.
#pragma once

#include "frost/app/app_services.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include <memory>

namespace frost::app {

class MainWindow final : public ui::ShellWindow {
    Q_OBJECT

public:
    // Creates the window hidden with its own AppServices (no appearance manager, the OS keyboard
    // layout followed, nothing persisted); call show() to display it. Must run on the GUI thread.
    explicit MainWindow(QWidget* parent = nullptr);
    // Creates the window and its workbench over services the caller owns; they must outlive the window.
    explicit MainWindow(AppServices& services, QWidget* parent = nullptr);
    // Creates a bare shell over an environment the caller owns (no workbench).
    explicit MainWindow(ui::ShellEnvironment environment, QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete ("QObject instances have identity and cannot be copied");
    MainWindow& operator=(const MainWindow&) = delete ("QObject instances have identity and cannot be copied");

    // The navigation workbench, or nullptr for a bare shell.
    [[nodiscard]] ui::Workbench* workbench() const noexcept { return m_workbench.get(); }

private:
    MainWindow(std::unique_ptr<AppServices> services, QWidget* parent);

    // Owned services (default construction only); torn down after the shell (~MainWindow).
    std::unique_ptr<AppServices> m_services;
    // Destroyed first, while the shell and the services are whole.
    std::unique_ptr<ui::Workbench> m_workbench;
};

} // namespace frost::app
