// SPDX-License-Identifier: Apache-2.0
//
// CMakeToolWindow: the content of the CMake tool window (ADR 0046), CLion's "CMake" tab: a row with
// the profile selector, Reload and Stop, an error banner, and the configure output. It binds to a
// CMakeController; the W1 workbench docks it with its ToolWindowManager.
/*
 * Behaviour: each configure clears the output and then streams cmake's lines (status lines from
 * the service included). The banner shows CMakeController::problem() and hides when it is empty.
 * Reload is disabled and Stop enabled while the folder is busy (a configure or build runs).
 * Choosing a profile asks the controller to switch; a refused switch puts the selector back.
 *
 * Children carry object names for tests and styling: cmakeProfiles, cmakeReload, cmakeStop,
 * cmakeErrorBanner, cmakeOutput. Threads: UI thread only. The controller must outlive the window.
 */
#pragma once

#include "frost/ui/cmake/cmake_controller.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QToolButton;

namespace frost::ui::cmake {

class CMakeToolWindow final : public QWidget {
    Q_OBJECT

public:
    explicit CMakeToolWindow(CMakeController& controller, QWidget* parent = nullptr);

    [[nodiscard]] QComboBox* profileSelector() const noexcept { return m_profiles; }
    [[nodiscard]] QToolButton* reloadButton() const noexcept { return m_reload; }
    [[nodiscard]] QToolButton* stopButton() const noexcept { return m_stop; }
    [[nodiscard]] QLabel* errorBanner() const noexcept { return m_banner; }
    [[nodiscard]] QPlainTextEdit* output() const noexcept { return m_output; }

private:
    void refreshProfiles();
    void refreshProblem(const QString& problem);
    void refreshBusy(bool busy);

    CMakeController& m_controller;
    QComboBox* m_profiles = nullptr;
    QToolButton* m_reload = nullptr;
    QToolButton* m_stop = nullptr;
    QLabel* m_banner = nullptr;
    QPlainTextEdit* m_output = nullptr;
    bool m_updatingProfiles = false;
};

} // namespace frost::ui::cmake
