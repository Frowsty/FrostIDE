// SPDX-License-Identifier: Apache-2.0
//
// The Debug tool window (id "debug", Alt+5; PLAN.md M4, ADR 0050) and the Evaluate Expression dialog
// (Alt+F8), as in Rider: a toolbar (Resume F9, Pause, Stop Ctrl+F2, Step Over F8, Step Into F7, Step
// Out Shift+F8, Run to Cursor Alt+F9, Evaluate Alt+F8), the stop's reason, the thread combo over the
// Frames list, the Variables tree, the Watches, and the program's console (a RunConsole).
/*
 * The window only displays and forwards: DebugController holds the session, the models read it.
 * The toolbar's buttons call the controller directly; the same commands are the registered actions
 * the keymap binds (debug_actions.hpp), so the shortcuts work wherever focus is. GUI thread only.
 */
#pragma once

#include <QDialog>
#include <QString>
#include <QWidget>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QTabWidget;
class QToolBar;
class QTreeView;

namespace frost::ui {

class DebugController;
class FramesModel;
class RunConsole;
class VariablesModel;

class DebugToolWindow final : public QWidget {
    Q_OBJECT

public:
    // `controller` must outlive the window; the window's console becomes the controller's.
    explicit DebugToolWindow(DebugController& controller, QWidget* parent = nullptr);
    ~DebugToolWindow() override;
    DebugToolWindow(const DebugToolWindow&) = delete ("a QWidget");
    DebugToolWindow& operator=(const DebugToolWindow&) = delete ("a QWidget");

    // The parts, for tests.
    [[nodiscard]] QToolBar* toolbar() const noexcept { return m_toolbar; }
    [[nodiscard]] QAction* resumeAction() const noexcept { return m_resume; }
    [[nodiscard]] QAction* pauseAction() const noexcept { return m_pause; }
    [[nodiscard]] QAction* stopAction() const noexcept { return m_stop; }
    [[nodiscard]] QAction* stepOverAction() const noexcept { return m_stepOver; }
    [[nodiscard]] QAction* stepIntoAction() const noexcept { return m_stepInto; }
    [[nodiscard]] QAction* stepOutAction() const noexcept { return m_stepOut; }
    [[nodiscard]] QLabel* reasonLabel() const noexcept { return m_reason; }
    [[nodiscard]] QComboBox* threadCombo() const noexcept { return m_threads; }
    [[nodiscard]] QListView* framesView() const noexcept { return m_framesView; }
    [[nodiscard]] QTreeView* variablesView() const noexcept { return m_variablesView; }
    [[nodiscard]] QTreeView* watchesView() const noexcept { return m_watchesView; }
    [[nodiscard]] QLineEdit* watchInput() const noexcept { return m_watchInput; }
    [[nodiscard]] FramesModel* framesModel() const noexcept { return m_frames; }
    [[nodiscard]] VariablesModel* variablesModel() const noexcept { return m_variables; }
    [[nodiscard]] VariablesModel* watchesModel() const noexcept { return m_watches; }
    [[nodiscard]] RunConsole* console() const noexcept { return m_console; }
    [[nodiscard]] QTabWidget* tabs() const noexcept { return m_tabs; }

    // Copies the value of the Variables or Watches row with focus to the clipboard.
    void copySelectedValue();
    // Opens the Evaluate Expression dialog with `expression` (non-modal; it deletes itself).
    void openEvaluate(const QString& expression = {});

private:
    void updateActions();
    void updateThreads();
    void onFrameActivated(int row);

    DebugController& m_controller;
    QToolBar* m_toolbar = nullptr;
    QAction* m_resume = nullptr;
    QAction* m_pause = nullptr;
    QAction* m_stop = nullptr;
    QAction* m_stepOver = nullptr;
    QAction* m_stepInto = nullptr;
    QAction* m_stepOut = nullptr;
    QAction* m_runToCursor = nullptr;
    QAction* m_evaluate = nullptr;
    QLabel* m_reason = nullptr;
    QTabWidget* m_tabs = nullptr;
    QComboBox* m_threads = nullptr;
    QListView* m_framesView = nullptr;
    QLabel* m_loadedLabel = nullptr;
    QPlainTextEdit* m_loadedSource = nullptr;
    QTreeView* m_variablesView = nullptr;
    QTreeView* m_watchesView = nullptr;
    QLineEdit* m_watchInput = nullptr;
    FramesModel* m_frames = nullptr;
    VariablesModel* m_variables = nullptr;
    VariablesModel* m_watches = nullptr;
    RunConsole* m_console = nullptr;
    bool m_fillingThreads = false;
};

// Evaluate Expression (Alt+F8): an expression line and the result as an expandable tree.
class EvaluateDialog final : public QDialog {
    Q_OBJECT

public:
    EvaluateDialog(DebugController& controller, const QString& expression, QWidget* parent = nullptr);
    ~EvaluateDialog() override;
    EvaluateDialog(const EvaluateDialog&) = delete ("a QWidget");
    EvaluateDialog& operator=(const EvaluateDialog&) = delete ("a QWidget");

    // Evaluates the expression line (context "repl"); the result replaces the previous one.
    void evaluate();

    [[nodiscard]] QLineEdit* expression() const noexcept { return m_expression; }
    [[nodiscard]] QTreeView* resultView() const noexcept { return m_resultView; }
    [[nodiscard]] VariablesModel* result() const noexcept { return m_result; }

private:
    QLineEdit* m_expression = nullptr;
    QTreeView* m_resultView = nullptr;
    VariablesModel* m_result = nullptr;
};

} // namespace frost::ui
