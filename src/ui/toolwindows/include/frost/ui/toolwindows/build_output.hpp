// SPDX-License-Identifier: Apache-2.0
//
// The build output tool window (W1, ADR 0039): BuildOutputLog, the line store that links output
// lines to the diagnostics the build service resolved, and BuildOutputPane, the monospace view that
// streams them live, tints error and warning lines, makes jumpable ones clickable and ends each
// build with a summary line.
//
/*
 * Linking. The service reports each line (onOutput) and, right after a line that holds a new
 * diagnostic, the resolved diagnostic (onDiagnostic); a repeated diagnostic is reported once. The
 * log parses every Stdout and Stderr line with build::parseDiagnosticLine, styles it by severity,
 * and links it to the BuildDiagnostic with the same fields as soon as that arrives (at once for a
 * repeat), so every copy of a diagnostic's line is clickable. A line is clickable when its
 * diagnostic is jumpable (a real file with a line number).
 *
 * Auto-scroll. The pane follows new lines while its scroll bar is at the end; scrolling up stops
 * that, and scrolling back to the end resumes it.
 *
 * Batching. A build can report thousands of lines at once (a translation unit's warnings, a
 * project's link step), each as its own posted event. appendLine and appendSummary record the line
 * in the log at once, but the view receives it on the next event-loop turn together with every
 * line that arrived meanwhile: one edit block, then one scroll. Inserting and scrolling per line
 * held the UI thread for 3 s during one burst of a FrostEditor build (j2_run's freeze report,
 * Debug Qt). editor() inserts the waiting lines first, so what it shows never lags the log.
 *
 * GUI thread only (the log itself is a plain value type with no thread affinity). Memory is
 * O(output); TODO(M4): cap the pane's lines for very long builds.
 */
#pragma once

#include "frost/build/build_service.hpp"

#include <QPoint>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class QPlainTextEdit;
class QTextCursor;
class QToolButton;

namespace frost::ui {

class BuildController;

// A place in a source file to jump to.
struct SourceLocation {
    std::string path; // Absolute.
    int line = 0;     // 1-based.
    int column = 0;   // 1-based; 0 when unknown.

    friend bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

enum class OutputLineStyle : std::uint8_t {
    Plain,
    Status,    // The service's own lines ("Build started: ...").
    Error,     // A line holding an error diagnostic.
    Warning,   // A line holding a warning diagnostic.
    Succeeded, // The summary of a build that succeeded.
    Failed,    // The summary of a build that failed, was cancelled or did not start.
};

struct OutputLine {
    std::string text;
    OutputLineStyle style = OutputLineStyle::Plain;
    std::optional<std::size_t> diagnostic; // Index into BuildOutputLog::diagnostics() once linked.
};

// "Build succeeded in 1.2 s", "Rebuild failed in 3.4 s", "Build cancelled after 0.5 s" or
// "Build not started: <reason>". Pure.
[[nodiscard]] std::string buildSummaryText(const build::BuildOutcome& outcome);

class BuildOutputLog {
public:
    // Appends one line (header comment, "Linking") and returns its index. O(line length).
    std::size_t append(build::BuildOutputKind kind, std::string_view text);
    // Records a diagnostic and returns the lines it linked. O(1) amortised plus the result.
    std::vector<std::size_t> addDiagnostic(const build::BuildDiagnostic& diagnostic);
    // Appends the summary line of `outcome` and returns its index.
    std::size_t appendSummary(const build::BuildOutcome& outcome);
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return m_lines.size(); }
    [[nodiscard]] const OutputLine& line(std::size_t index) const { return m_lines.at(index); }
    [[nodiscard]] const std::vector<build::BuildDiagnostic>& diagnostics() const noexcept { return m_diagnostics; }
    // Where clicking line `index` jumps; nullopt when it is not clickable or out of range.
    [[nodiscard]] std::optional<SourceLocation> locationAt(std::size_t index) const;

private:
    std::vector<OutputLine> m_lines;
    std::vector<build::BuildDiagnostic> m_diagnostics;
    std::unordered_map<std::string, std::size_t> m_diagnosticByKey;           // Diagnostic key -> index.
    std::unordered_map<std::string, std::vector<std::size_t>> m_waitingLines; // Key -> lines not linked yet.
};

class BuildOutputPane final : public QWidget {
    Q_OBJECT

public:
    explicit BuildOutputPane(QWidget* parent = nullptr);
    ~BuildOutputPane() override;

    // Follows `controller` (not owned; nullptr detaches): replays its current build's output,
    // diagnostics and, when it has ended, summary, then streams new events. A new build clears the pane.
    void setController(BuildController* controller);

    // Append to the log at once and to the view on the next event-loop turn (header comment, "Batching").
    void appendLine(build::BuildOutputKind kind, const QString& text);
    void addDiagnostic(const build::BuildDiagnostic& diagnostic);
    void appendSummary(const build::BuildOutcome& outcome);
    // Empties the log and the view, lines still waiting for the view included.
    void clear();

    // Emits openLocation() for line `index` when it is clickable; returns whether it was.
    bool activateLine(std::size_t index);
    // The view's line (block) at a viewport position, if any.
    [[nodiscard]] std::optional<std::size_t> lineAt(QPoint viewportPosition) const;
    [[nodiscard]] bool followsTail() const noexcept { return m_follow; }

    [[nodiscard]] const BuildOutputLog& log() const noexcept { return m_log; }
    // The text view, holding every line of the log (the waiting lines are inserted first).
    [[nodiscard]] QPlainTextEdit* editor();
    [[nodiscard]] QToolButton* clearButton() const noexcept { return m_clear; }

signals:
    void openLocation(const QString& path, int line, int column);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void insertLine(QTextCursor& cursor, std::size_t index);
    void markClickable(std::size_t index);
    void scrollToEndIfFollowing();
    // Queues one flush() for the next event-loop turn unless one is queued.
    void scheduleFlush();
    // Inserts the log's lines the view does not show yet in one edit block, then scrolls once.
    void flush();

    BuildOutputLog m_log;
    std::size_t m_shown = 0;    // The log lines the view holds; the rest wait for flush().
    bool m_flushQueued = false; // A flush() is queued.
    QPlainTextEdit* m_editor = nullptr;
    QToolButton* m_clear = nullptr;
    BuildController* m_controller = nullptr;
    std::vector<QMetaObject::Connection> m_connections;
    bool m_follow = true;
    bool m_appending = false;
    QPoint m_pressPosition;
};

} // namespace frost::ui
