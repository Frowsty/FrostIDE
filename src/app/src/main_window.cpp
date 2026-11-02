// SPDX-License-Identifier: Apache-2.0
//
// MainWindow (frost/app/main_window.hpp). The private constructor reads the environment from the
// services before the base class is built and keeps them alive in m_services; ~MainWindow tears
// the shell down before m_services is destroyed.

#include "frost/app/main_window.hpp"

#include <utility>

namespace frost::app {

MainWindow::MainWindow(QWidget* parent) : MainWindow(std::make_unique<AppServices>(), parent) {}

MainWindow::MainWindow(ui::ShellEnvironment environment, QWidget* parent) : ui::ShellWindow(environment, parent) {}

MainWindow::MainWindow(std::unique_ptr<AppServices> services, QWidget* parent)
    : ui::ShellWindow(services->environment(), parent), m_services(std::move(services)) {}

MainWindow::~MainWindow() {
    // The editors and the dispatcher refer to m_services; they go first.
    teardown();
}

} // namespace frost::app
