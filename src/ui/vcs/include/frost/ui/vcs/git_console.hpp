// SPDX-License-Identifier: Apache-2.0
//
// GitConsoleView (M5, ADR 0048): the "git" console, a read-only log of every command the git
// service ran ("> git fetch --all --prune") and the lines git printed, newest at the bottom. The
// workbench docks it (the Git tool window's Console tab). GUI thread only; the controller must
// outlive the view.
#pragma once

#include <QPlainTextEdit>
#include <QPointer>

namespace frost::ui {

class GitController;

class GitConsoleView final : public QPlainTextEdit {
    Q_OBJECT

public:
    // The newest lines kept.
    static constexpr int kMaxLines = 5000;

    explicit GitConsoleView(GitController& controller, QWidget* parent = nullptr);

    // Appends one line as the controller delivers it (`stream`: a vcs::GitStream).
    void appendLine(int stream, const QString& line);

private:
    QPointer<GitController> m_controller;
};

} // namespace frost::ui
