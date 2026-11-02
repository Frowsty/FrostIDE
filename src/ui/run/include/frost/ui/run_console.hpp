// SPDX-License-Identifier: Apache-2.0
//
// RunConsole: the Run tool window's console (W1, ADR 0040), as in Rider: the program's output
// with stderr tinted, FrostIDE's own lines (the command line, why a run did not start) dimmed,
// an input line that writes to the program's stdin, Stop and Rerun buttons, and "Process
// finished with exit code N" at the end. It only displays: RunController feeds it from the
// RunService and turns its signals into service calls.
//
/*
 * Output. appendOutput takes UTF-8 chunks as RunService delivers them (not lines). A CR LF pair
 * becomes one line break, also when the pair is split across two chunks; a lone CR is kept as
 * a line break too (no carriage-return overwriting: that comes with the terminal, M5). The
 * document keeps the last kMaxLines lines. GUI thread only.
 */
#pragma once

#include "frost/build/run_service.hpp"

#include <QString>
#include <QWidget>

#include <string_view>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QToolButton;

namespace frost::ui {

/*
 * The closing line of a run (without a line break): "Process finished with exit code N" when the
 * program ran (Windows exception codes such as 0xC0000005 also in hex), with " (stopped)" after a
 * Stop; otherwise why it did not run ("Build failed", or the error's message). Pure.
 */
[[nodiscard]] QString runFinishedLine(const build::RunOutcome& outcome);

class RunConsole final : public QWidget {
    Q_OBJECT

public:
    // The most lines the output keeps; older ones are dropped.
    static constexpr int kMaxLines = 20000;

    explicit RunConsole(QWidget* parent = nullptr);
    ~RunConsole() override;
    RunConsole(const RunConsole&) = delete ("a QWidget");
    RunConsole& operator=(const RunConsole&) = delete ("a QWidget");

    // Starts showing a new run: clears the output, sets the title, disables input until Running.
    void beginRun(const QString& title);
    // The run reached Running (or Stopping): Stop is enabled; the input line when `acceptsInput`.
    void setRunning(bool running, bool acceptsInput);
    // Appends a chunk of output of `kind` (header comment).
    void appendOutput(build::RunOutputKind kind, std::string_view text);
    // Ends the run: input and Stop off, Rerun on, the closing line (runFinishedLine) appended.
    void finish(const build::RunOutcome& outcome);

    // The output as plain text (tests and copy).
    [[nodiscard]] QString text() const;
    // The widgets, for the controller's tests and the tool window's toolbar.
    [[nodiscard]] QPlainTextEdit* output() const noexcept { return m_output; }
    [[nodiscard]] QLineEdit* input() const noexcept { return m_input; }
    [[nodiscard]] QToolButton* stopButton() const noexcept { return m_stop; }
    [[nodiscard]] QToolButton* rerunButton() const noexcept { return m_rerun; }
    [[nodiscard]] QLabel* title() const noexcept { return m_title; }

signals:
    // The user pressed Stop.
    void stopRequested();
    // The user pressed Rerun.
    void rerunRequested();
    // The user pressed Enter in the input line; `line` has no line break (the controller adds "\n").
    void inputSubmitted(const QString& line);

private:
    void appendText(build::RunOutputKind kind, const QString& text);

    QLabel* m_title = nullptr;
    QToolButton* m_rerun = nullptr;
    QToolButton* m_stop = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QLineEdit* m_input = nullptr;
    // Per stream: whether its last chunk ended in CR (a CR LF split across chunks).
    bool m_pendingCr[3] = {false, false, false};
    // Whether the output does not end in a line break (so the closing line starts a new one).
    bool m_openLine = false;
};

} // namespace frost::ui
