// SPDX-License-Identifier: Apache-2.0
//
// MainWindow: FrostIDE's top-level window, the Rider-style shell (frost::ui::ShellWindow, ADR 0038)
// over the application's services. Default-constructed, it owns its own AppServices (tests and the
// self-test); main() builds the services first and passes their environment.
#pragma once

#include "frost/app/app_services.hpp"
#include "frost/ui/shell/shell_window.hpp"

#include <memory>

namespace frost::app {

class MainWindow final : public ui::ShellWindow {
    Q_OBJECT

public:
    // Creates the window hidden with its own AppServices (no appearance manager, the OS keyboard
    // layout followed); call show() to display it. Must run on the GUI thread.
    explicit MainWindow(QWidget* parent = nullptr);
    // Creates the window over services the caller owns; they must outlive the window.
    explicit MainWindow(ui::ShellEnvironment environment, QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete ("QObject instances have identity and cannot be copied");
    MainWindow& operator=(const MainWindow&) = delete ("QObject instances have identity and cannot be copied");

private:
    MainWindow(std::unique_ptr<AppServices> services, QWidget* parent);

    // Owned services (default construction only); torn down after the shell (~MainWindow).
    std::unique_ptr<AppServices> m_services;
};

} // namespace frost::app
