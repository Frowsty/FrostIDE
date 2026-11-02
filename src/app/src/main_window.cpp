// SPDX-License-Identifier: Apache-2.0
//
// MainWindow (frost/app/main_window.hpp). The private constructor reads the environment from the
// services before the base class is built and keeps them alive in m_services; ~MainWindow destroys
// the workbench, then tears the shell down before m_services is destroyed.

#include "frost/app/main_window.hpp"

#include <utility>

namespace frost::app {

MainWindow::MainWindow(QWidget* parent) : MainWindow(std::make_unique<AppServices>(), parent) {}

MainWindow::MainWindow(AppServices& services, QWidget* parent) : ui::ShellWindow(services.environment(), parent) {
    m_workbench = std::make_unique<ui::Workbench>(*this, services.workbenchServices());
}

MainWindow::MainWindow(ui::ShellEnvironment environment, QWidget* parent) : ui::ShellWindow(environment, parent) {}

MainWindow::MainWindow(std::unique_ptr<AppServices> services, QWidget* parent)
    : ui::ShellWindow(services->environment(), parent), m_services(std::move(services)) {
    m_workbench = std::make_unique<ui::Workbench>(*this, m_services->workbenchServices());
}

MainWindow::~MainWindow() {
    // The workbench saves the session and stops its watcher while the shell is whole; then the
    // editors and the dispatcher, which refer to the services, go.
    m_workbench.reset();
    teardown();
}

} // namespace frost::app
