// SPDX-License-Identifier: Apache-2.0
//
// BuildOutputLog and BuildOutputPane (frost/ui/toolwindows/build_output.hpp).

#include "frost/ui/toolwindows/build_output.hpp"

#include "frost/build/diagnostic.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "tool_icons.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

#include <chrono>
#include <format>
#include <utility>

namespace frost::ui {
namespace {

// Every field of a diagnostic, so equal diagnostics share a key.
std::string diagnosticKey(const build::Diagnostic& d) {
    return std::format("{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}", static_cast<int>(d.severity), d.code, d.message,
                       d.file, d.line, d.column, d.project);
}

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

} // namespace

std::string buildSummaryText(const build::BuildOutcome& outcome) {
    const std::string_view action = build::toString(outcome.target.action);
    if (outcome.error) {
        return std::format("{} not started: {}", action, outcome.error->message());
    }
    const std::string seconds = std::format("{:.1f} s", std::chrono::duration<double>(outcome.result.duration).count());
    if (outcome.result.cancelled) {
        return std::format("{} cancelled after {}", action, seconds);
    }
    return std::format("{} {} in {}", action, outcome.result.success ? "succeeded" : "failed", seconds);
}

// ---- BuildOutputLog.

std::size_t BuildOutputLog::append(build::BuildOutputKind kind, std::string_view text) {
    if (!text.empty() && text.back() == '\r') {
        text.remove_suffix(1);
    }
    const std::size_t index = m_lines.size();
    OutputLine line{std::string(text), OutputLineStyle::Plain, std::nullopt};
    if (kind == build::BuildOutputKind::Status) {
        line.style = OutputLineStyle::Status;
    } else if (const std::optional<build::Diagnostic> parsed = build::parseDiagnosticLine(text)) {
        if (parsed->severity == build::Severity::Error) {
            line.style = OutputLineStyle::Error;
        } else if (parsed->severity == build::Severity::Warning) {
            line.style = OutputLineStyle::Warning;
        }
        std::string key = diagnosticKey(*parsed);
        if (const auto known = m_diagnosticByKey.find(key); known != m_diagnosticByKey.end()) {
            line.diagnostic = known->second;
        } else {
            m_waitingLines[std::move(key)].push_back(index);
        }
    }
    m_lines.push_back(std::move(line));
    return index;
}

std::vector<std::size_t> BuildOutputLog::addDiagnostic(const build::BuildDiagnostic& diagnostic) {
    std::string key = diagnosticKey(diagnostic.diagnostic);
    const std::size_t index = m_diagnostics.size();
    m_diagnostics.push_back(diagnostic);
    std::vector<std::size_t> linked;
    if (const auto waiting = m_waitingLines.find(key); waiting != m_waitingLines.end()) {
        linked = std::move(waiting->second);
        m_waitingLines.erase(waiting);
        for (const std::size_t line : linked) {
            m_lines[line].diagnostic = index;
        }
    }
    m_diagnosticByKey.try_emplace(std::move(key), index);
    return linked;
}

std::size_t BuildOutputLog::appendSummary(const build::BuildOutcome& outcome) {
    m_lines.push_back(OutputLine{buildSummaryText(outcome),
                                 outcome.succeeded() ? OutputLineStyle::Succeeded : OutputLineStyle::Failed,
                                 std::nullopt});
    return m_lines.size() - 1;
}

void BuildOutputLog::clear() noexcept {
    m_lines.clear();
    m_diagnostics.clear();
    m_diagnosticByKey.clear();
    m_waitingLines.clear();
}

std::optional<SourceLocation> BuildOutputLog::locationAt(std::size_t index) const {
    if (index >= m_lines.size() || !m_lines[index].diagnostic) {
        return std::nullopt;
    }
    const build::BuildDiagnostic& d = m_diagnostics[*m_lines[index].diagnostic];
    if (!d.jumpable || d.path.empty()) {
        return std::nullopt;
    }
    return SourceLocation{d.path, d.diagnostic.line, d.diagnostic.column};
}

// ---- BuildOutputPane.

BuildOutputPane::BuildOutputPane(QWidget* parent)
    : QWidget(parent), m_editor(new QPlainTextEdit(this)), m_clear(new QToolButton(this)) {
    setObjectName(QStringLiteral("buildOutput"));
    m_editor->setObjectName(QStringLiteral("buildOutputText"));
    m_editor->setReadOnly(true);
    m_editor->setUndoRedoEnabled(false);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setFamilies({QStringLiteral("JetBrains Mono"), font.family()});
    m_editor->setFont(font);
    m_editor->viewport()->setMouseTracking(true);
    m_editor->viewport()->installEventFilter(this);

    detail::IconCache icons;
    m_clear->setObjectName(QStringLiteral("clearOutput"));
    m_clear->setText(tr("Clear"));
    m_clear->setToolTip(tr("Clear All"));
    m_clear->setIcon(icons.get(icons::kActionsRemove));
    m_clear->setAutoRaise(true);
    connect(m_clear, &QToolButton::clicked, this, &BuildOutputPane::clear);

    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->addWidget(m_clear);
    toolbar->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(toolbar);
    layout->addWidget(m_editor);

    QScrollBar* bar = m_editor->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int value) {
        if (!m_appending) {
            m_follow = value >= bar->maximum();
        }
    });
}

BuildOutputPane::~BuildOutputPane() = default;

void BuildOutputPane::setController(BuildController* controller) {
    for (const QMetaObject::Connection& connection : m_connections) {
        disconnect(connection);
    }
    m_connections.clear();
    m_controller = controller;
    clear();
    if (m_controller == nullptr) {
        return;
    }
    for (const BuildOutputEntry& entry : m_controller->currentOutput()) {
        appendLine(entry.kind, entry.text);
    }
    for (const build::BuildDiagnostic& diagnostic : m_controller->diagnostics()) {
        addDiagnostic(diagnostic);
    }
    if (!m_controller->isBuilding() && m_controller->lastOutcome() && !m_controller->currentOutput().empty()) {
        appendSummary(*m_controller->lastOutcome());
    }
    m_connections.push_back(connect(m_controller, &BuildController::buildStarted, this, &BuildOutputPane::clear));
    m_connections.push_back(connect(m_controller, &BuildController::outputLine, this, &BuildOutputPane::appendLine));
    m_connections.push_back(
        connect(m_controller, &BuildController::diagnosticAdded, this, &BuildOutputPane::addDiagnostic));
    m_connections.push_back(
        connect(m_controller, &BuildController::buildFinished, this, &BuildOutputPane::appendSummary));
}

void BuildOutputPane::insertLine(const OutputLine& line) {
    const std::size_t index = m_log.size() - 1;
    QTextCharFormat format;
    switch (line.style) {
    case OutputLineStyle::Plain: break;
    case OutputLineStyle::Status:
        format.setForeground(
            detail::themeColor(ColorKey::LabelSecondaryForeground, palette().color(QPalette::PlaceholderText)));
        break;
    case OutputLineStyle::Error: format.setForeground(detail::severityColor(build::Severity::Error)); break;
    case OutputLineStyle::Warning: format.setForeground(detail::severityColor(build::Severity::Warning)); break;
    case OutputLineStyle::Succeeded:
        format.setForeground(detail::iconRoleColor(IconRole::Success, QColor(0x30, 0x90, 0x40)));
        format.setFontWeight(QFont::Bold);
        break;
    case OutputLineStyle::Failed:
        format.setForeground(detail::severityColor(build::Severity::Error));
        format.setFontWeight(QFont::Bold);
        break;
    }
    if (m_log.locationAt(index)) {
        format.setFontUnderline(true);
    }
    QTextCursor cursor(m_editor->document());
    cursor.movePosition(QTextCursor::End);
    if (index > 0) {
        cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
    cursor.insertText(qs(line.text), format);
}

void BuildOutputPane::markClickable(std::size_t index) {
    const QTextBlock block = m_editor->document()->findBlockByNumber(static_cast<int>(index));
    if (!block.isValid()) {
        return;
    }
    QTextCursor cursor(block);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    QTextCharFormat underline;
    underline.setFontUnderline(true);
    cursor.mergeCharFormat(underline);
}

void BuildOutputPane::scrollToEndIfFollowing() {
    if (m_follow) {
        QScrollBar* bar = m_editor->verticalScrollBar();
        bar->setValue(bar->maximum());
    }
}

void BuildOutputPane::appendLine(build::BuildOutputKind kind, const QString& text) {
    m_appending = true;
    const std::size_t index = m_log.append(kind, toUtf8(text));
    insertLine(m_log.line(index));
    scrollToEndIfFollowing();
    m_appending = false;
}

void BuildOutputPane::addDiagnostic(const build::BuildDiagnostic& diagnostic) {
    for (const std::size_t line : m_log.addDiagnostic(diagnostic)) {
        if (m_log.locationAt(line)) {
            markClickable(line);
        }
    }
}

void BuildOutputPane::appendSummary(const build::BuildOutcome& outcome) {
    m_appending = true;
    const std::size_t index = m_log.appendSummary(outcome);
    insertLine(m_log.line(index));
    scrollToEndIfFollowing();
    m_appending = false;
}

void BuildOutputPane::clear() {
    m_appending = true;
    m_log.clear();
    m_editor->clear();
    m_follow = true;
    m_appending = false;
}

bool BuildOutputPane::activateLine(std::size_t index) {
    const std::optional<SourceLocation> location = m_log.locationAt(index);
    if (!location) {
        return false;
    }
    emit openLocation(qs(location->path), location->line, location->column);
    return true;
}

std::optional<std::size_t> BuildOutputPane::lineAt(QPoint viewportPosition) const {
    const QTextCursor cursor = m_editor->cursorForPosition(viewportPosition);
    if (cursor.isNull()) {
        return std::nullopt;
    }
    // cursorForPosition picks the nearest block; a point below the last line is no line.
    const QRect rect = m_editor->cursorRect(cursor);
    if (viewportPosition.y() < rect.top() || viewportPosition.y() > rect.bottom()) {
        return std::nullopt;
    }
    const int block = cursor.block().blockNumber();
    if (block < 0 || static_cast<std::size_t>(block) >= m_log.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(block);
}

bool BuildOutputPane::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_editor->viewport()) {
        return QWidget::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_pressPosition = mouse->position().toPoint();
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint position = mouse->position().toPoint();
        // A click, not the end of a drag that selected text.
        if (mouse->button() == Qt::LeftButton &&
            (position - m_pressPosition).manhattanLength() < QApplication::startDragDistance() &&
            !m_editor->textCursor().hasSelection()) {
            if (const std::optional<std::size_t> line = lineAt(position)) {
                activateLine(*line);
            }
        }
        break;
    }
    case QEvent::MouseMove: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const std::optional<std::size_t> line = lineAt(mouse->position().toPoint());
        const bool clickable = line && m_log.locationAt(*line);
        m_editor->viewport()->setCursor(clickable ? Qt::PointingHandCursor : Qt::IBeamCursor);
        break;
    }
    default: break;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace frost::ui
