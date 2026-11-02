// SPDX-License-Identifier: Apache-2.0
//
// RunConsole (see frost/ui/run_console.hpp): the layout, the tints, CR LF handling and the
// closing line.

#include "frost/ui/run_console.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstdint>

namespace frost::ui {

namespace {

// Whether the palette is a dark one (its base is darker than its text).
bool isDark(const QPalette& palette) {
    return palette.color(QPalette::Base).lightness() < palette.color(QPalette::Text).lightness();
}

// stderr's tint: the themes have no error colour for consoles yet, so a red that reads on the
// palette's base (Rider uses about the same pair).
QColor stderrColor(const QPalette& palette) {
    return isDark(palette) ? QColor(0xF0, 0x6B, 0x68) : QColor(0xC7, 0x22, 0x2D);
}

// FrostIDE's own lines: the palette's dimmed text.
QColor systemColor(const QPalette& palette) { return palette.color(QPalette::PlaceholderText); }

std::size_t streamIndex(build::RunOutputKind kind) noexcept { return static_cast<std::size_t>(kind); }

} // namespace

QString runFinishedLine(const build::RunOutcome& outcome) {
    if (outcome.exitCode) {
        const int code = *outcome.exitCode;
        const auto bits = static_cast<std::uint32_t>(code);
        QString line = QStringLiteral("Process finished with exit code %1").arg(code);
        // NTSTATUS-style codes (an access violation is 0xC0000005) read better in hex.
        if (bits >= 0xC0000000U) {
            line += QStringLiteral(" (0x%1)").arg(QString::number(bits, 16).toUpper());
        }
        if (outcome.stopped) {
            line += QStringLiteral(" (stopped)");
        }
        return line;
    }
    if (outcome.buildFailed) {
        return outcome.stopped ? QStringLiteral("Build cancelled; the program was not started")
                               : QStringLiteral("Build failed; the program was not started");
    }
    if (outcome.stopped) {
        return QStringLiteral("Stopped before the program started");
    }
    if (outcome.error) {
        return QStringLiteral("The program was not started: %1").arg(QString::fromStdString(outcome.error->message()));
    }
    return QStringLiteral("Process finished");
}

RunConsole::RunConsole(QWidget* parent) : QWidget(parent) {
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("runConsoleTitle"));
    m_rerun = new QToolButton(this);
    m_rerun->setObjectName(QStringLiteral("runConsoleRerun"));
    m_rerun->setText(tr("Rerun"));
    m_rerun->setToolTip(tr("Rerun (stops the program first)"));
    m_stop = new QToolButton(this);
    m_stop->setObjectName(QStringLiteral("runConsoleStop"));
    m_stop->setText(tr("Stop"));
    m_stop->setToolTip(tr("Stop the program and everything it started"));
    m_stop->setEnabled(false);
    m_rerun->setEnabled(false);

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("runConsoleOutput"));
    m_output->setReadOnly(true);
    m_output->setUndoRedoEnabled(false);
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_output->setMaximumBlockCount(kMaxLines);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("runConsoleInput"));
    m_input->setPlaceholderText(tr("Input to the program; Enter sends the line"));
    m_input->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_input->setEnabled(false);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(m_title, 1);
    header->addWidget(m_rerun);
    header->addWidget(m_stop);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addLayout(header);
    layout->addWidget(m_output, 1);
    layout->addWidget(m_input);

    connect(m_stop, &QToolButton::clicked, this, &RunConsole::stopRequested);
    connect(m_rerun, &QToolButton::clicked, this, &RunConsole::rerunRequested);
    connect(m_input, &QLineEdit::returnPressed, this, [this] {
        const QString line = m_input->text();
        m_input->clear();
        // The typed line is echoed like a terminal would, in the input colour (stdout's).
        appendText(build::RunOutputKind::Stdout, line + QLatin1Char('\n'));
        emit inputSubmitted(line);
    });
}

RunConsole::~RunConsole() = default;

void RunConsole::beginRun(const QString& title) {
    m_output->clear();
    m_title->setText(title);
    m_pendingCr[0] = m_pendingCr[1] = m_pendingCr[2] = false;
    m_openLine = false;
    m_input->clear();
    m_input->setEnabled(false);
    m_stop->setEnabled(true); // A run in its build can be stopped too.
    m_rerun->setEnabled(true);
}

void RunConsole::setRunning(bool running, bool acceptsInput) {
    m_stop->setEnabled(running);
    m_input->setEnabled(running && acceptsInput);
}

void RunConsole::appendOutput(build::RunOutputKind kind, std::string_view text) {
    QString chunk = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    bool& pendingCr = m_pendingCr[streamIndex(kind)];
    // A CR that ended the previous chunk was shown as a line break: drop the LF that completes it.
    if (pendingCr && chunk.startsWith(QLatin1Char('\n'))) {
        chunk.remove(0, 1);
    }
    pendingCr = chunk.endsWith(QLatin1Char('\r'));
    chunk.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    chunk.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    appendText(kind, chunk);
}

void RunConsole::finish(const build::RunOutcome& outcome, const QString& summary) {
    m_stop->setEnabled(false);
    m_input->setEnabled(false);
    m_rerun->setEnabled(true);
    QString line = summary.isEmpty() ? runFinishedLine(outcome) : summary;
    appendText(build::RunOutputKind::System,
               (m_openLine ? QStringLiteral("\n\n") : QStringLiteral("\n")) + line + QLatin1Char('\n'));
}

QString RunConsole::text() const { return m_output->toPlainText(); }

void RunConsole::appendText(build::RunOutputKind kind, const QString& text) {
    if (text.isEmpty()) {
        return;
    }
    QScrollBar* bar = m_output->verticalScrollBar();
    const bool atEnd = bar->value() == bar->maximum();
    QTextCharFormat format;
    const QPalette& palette = m_output->palette();
    switch (kind) {
    case build::RunOutputKind::Stdout: format.setForeground(palette.color(QPalette::Text)); break;
    case build::RunOutputKind::Stderr: format.setForeground(stderrColor(palette)); break;
    case build::RunOutputKind::System: format.setForeground(systemColor(palette)); break;
    }
    QTextCursor cursor(m_output->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text, format);
    m_openLine = !text.endsWith(QLatin1Char('\n'));
    if (atEnd) {
        bar->setValue(bar->maximum()); // Follow the output unless the user scrolled up.
    }
}

} // namespace frost::ui
