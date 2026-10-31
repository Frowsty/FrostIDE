// SPDX-License-Identifier: Apache-2.0
//
// MainWindow: FrostIDE's top-level window. In M0 it is a minimal shell (menu
// bar, central placeholder, status bar) that proves the Qt toolchain, the
// manifest and the self-test; the Rider-style shell replaces it in M1.
#pragma once

#include <QMainWindow>

namespace frost::app {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    // Creates the window hidden; call show() to display it. Must run on the GUI thread.
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete ("QObject instances have identity and cannot be copied");
    MainWindow& operator=(const MainWindow&) = delete ("QObject instances have identity and cannot be copied");
};

} // namespace frost::app
