// SPDX-License-Identifier: Apache-2.0
//
// CMakeToolWindow: the content of the CMake tool window (ADR 0046), CLion's "CMake" tab: a row with
// the profile selector, Reload and Stop, a progress row, an error banner, and tabs for configure
// output, presets, File API targets and typed cache edits. It binds to a CMakeController; the W1 workbench docks
// it with its ToolWindowManager.
/*
 * Behaviour: each configure clears the output and then streams cmake's lines (status lines from
 * the service included). The banner shows CMakeController::problem() and hides when it is empty.
 * Reload is disabled and Stop enabled while the folder is busy (a configure or build runs).
 * Choosing a profile asks the controller to switch; a refused switch puts the selector back.
 *
 * Progress (CMK-15): while the service reports an activity (CMakeController::progress()), the
 * progress row shows its text with the seconds since it began and an indeterminate bar (cmake
 * reports no fraction); Stop follows the activity's cancellable flag. The row hides when idle.
 * The elapsed time is refreshed by a one-second timer that runs only while the row is shown.
 *
 * Presets: the Presets tab lists the folder's presets per kind exactly as `cmake
 * --list-presets=all` does (name and displayName; hidden and disabled presets left out), refreshed
 * when the profiles change.
 *
 * Children carry object names for tests and styling: cmakeProfiles, cmakeReload, cmakeStop,
 * cmakeProgress, cmakeProgressLabel, cmakeErrorBanner, cmakeTabs, cmakeOutput, cmakePresets.
 * Cache values become accepted only through a successful configure and new File API reply.
 * Threads: UI thread only. The controller must outlive the window.
 */
#pragma once

#include "frost/ui/cmake/cmake_controller.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QTabWidget;
class QTimer;
class QToolButton;
class QTreeWidget;

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
    // The progress row's container, its text ("Configuring Debug (3 s)") and its bar.
    [[nodiscard]] QWidget* progressRow() const noexcept { return m_progressRow; }
    [[nodiscard]] QLabel* progressLabel() const noexcept { return m_progressLabel; }
    [[nodiscard]] QProgressBar* progressBar() const noexcept { return m_progressBar; }
    // The Presets tab: one top-level item per listed kind, one child per preset (columns: name,
    // display name).
    [[nodiscard]] QTreeWidget* presets() const noexcept { return m_presets; }

signals:
    void createKitRequested();

private:
    void refreshModel();
    void refreshProfiles();
    void refreshProblem(const QString& problem);
    void refreshBusy(bool busy);
    void refreshProgress();
    void refreshPresets();

    CMakeController& m_controller;
    QComboBox* m_profiles = nullptr;
    QToolButton* m_reload = nullptr;
    QToolButton* m_stop = nullptr;
    QWidget* m_progressRow = nullptr;
    QLabel* m_progressLabel = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QTimer* m_elapsed = nullptr;
    QLabel* m_banner = nullptr;
    QTabWidget* m_tabs = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QTreeWidget* m_presets = nullptr;
    QTreeWidget* m_targets = nullptr;
    QTreeWidget* m_cache = nullptr;
    QToolButton* m_applyCache = nullptr;
    bool m_updatingProfiles = false;
};

} // namespace frost::ui::cmake
