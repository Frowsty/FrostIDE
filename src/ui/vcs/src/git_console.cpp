// SPDX-License-Identifier: Apache-2.0
//
// GitConsoleView (frost/ui/vcs/git_console.hpp).

#include "frost/ui/vcs/git_console.hpp"

#include "frost/ui/vcs/git_controller.hpp"
#include "frost/vcs/git_runner.hpp"

#include <QFontDatabase>

namespace frost::ui {

GitConsoleView::GitConsoleView(GitController& controller, QWidget* parent)
    : QPlainTextEdit(parent), m_controller(&controller) {
    setObjectName(QStringLiteral("gitConsole"));
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setMaximumBlockCount(kMaxLines);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    connect(&controller, &GitController::outputLine, this, &GitConsoleView::appendLine);
}

void GitConsoleView::appendLine(int stream, const QString& line) {
    if (static_cast<vcs::GitStream>(stream) == vcs::GitStream::Command) {
        appendPlainText(QStringLiteral("> ") + line);
    } else {
        appendPlainText(line);
    }
}

} // namespace frost::ui
