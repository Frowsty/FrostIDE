// SPDX-License-Identifier: Apache-2.0
//
// The status bar widgets (PLAN.md row 242, ADR 0038): the background-processes indicator, the
// caret position (line:column, with the selection size), the line separator, the encoding, the
// indent and the read-only lock of the active editor, and the notifications counter. They are
// permanent QStatusBar widgets, so the status message on the left never hides them.
#pragma once

#include "frost/ui/shell/editor_host.hpp"

#include <QObject>
#include <QString>

#include <vector>

class QStatusBar;
class QToolButton;
class QWidget;

namespace frost::ui {

class BackgroundTaskManager;
class NotificationCenter;
class ProcessesIndicator;

/*
 * Creates the widgets in `bar` and updates them. The editor widgets are hidden while no editor is
 * active. The manager and the center must outlive this object. GUI thread only.
 */
class ShellStatusBar final : public QObject {
    Q_OBJECT

public:
    ShellStatusBar(QStatusBar& bar, BackgroundTaskManager& tasks, NotificationCenter& notifications,
                   QObject* parent = nullptr);

    // Shows `status` (nullptr: no active editor).
    void showEditor(const EditorStatus* status);

    // "12:5", or "12:5 (3 chars)" with a selection. Translated.
    [[nodiscard]] static QString caretText(const EditorStatus& status);
    // "4 spaces" or "Tab". Translated.
    [[nodiscard]] static QString indentText(const IndentStatus& indent);

    [[nodiscard]] QToolButton* caret() const noexcept { return m_caret; }
    [[nodiscard]] QToolButton* lineSeparator() const noexcept { return m_separator; }
    [[nodiscard]] QToolButton* encoding() const noexcept { return m_encoding; }
    [[nodiscard]] QToolButton* indent() const noexcept { return m_indent; }
    [[nodiscard]] QToolButton* readOnly() const noexcept { return m_readOnly; }
    [[nodiscard]] QToolButton* notifications() const noexcept { return m_notifications; }
    [[nodiscard]] ProcessesIndicator* processes() const noexcept { return m_processes; }
    // The focusable widgets, left to right (keyboard navigation, Alt+Page Down).
    [[nodiscard]] std::vector<QWidget*> focusOrder() const;

signals:
    // The caret widget was clicked (the shell opens Go to Line).
    void caretClicked();
    // The notifications counter was clicked (the shell toggles the Notifications tool window).
    void notificationsClicked();

private:
    void updateNotifications();

    NotificationCenter& m_center;
    ProcessesIndicator* m_processes;
    QToolButton* m_caret;
    QToolButton* m_separator;
    QToolButton* m_encoding;
    QToolButton* m_indent;
    QToolButton* m_readOnly;
    QToolButton* m_notifications;
};

} // namespace frost::ui
