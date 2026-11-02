// SPDX-License-Identifier: Apache-2.0
//
// InsightStatusIndicator (ADR 0047): the status bar's problems indicator for the active editor,
// "2 errors, 1 warning" (or "No problems", "Analyzing...") with a severity icon. A plain
// QToolButton: no painting of its own, so it looks like the status bar's other items.
#pragma once

#include "frost/ui/editor/editor_insight.hpp"

#include <QIcon>
#include <QMetaObject>
#include <QPointer>
#include <QToolButton>

namespace frost::ui {

class EditorView;

/*
 * Follows one EditorView's insightSummary() (setView) or shows a summary it is given (setSummary).
 * As a child widget it hides while the followed view has no insight (insightEnabled() false). A click
 * emits problemsRequested (the workbench opens the Problems tool window). The icons are optional
 * (the workbench passes its theme's status.error / warning / info / ok / loading icons); without
 * them the indicator is text only. GUI thread only; does not own the view.
 */
class InsightStatusIndicator final : public QToolButton {
    Q_OBJECT

public:
    explicit InsightStatusIndicator(QWidget* parent = nullptr);

    // Follows `view` (nullptr: none). The previous view's connection is dropped.
    void setView(EditorView* view);
    [[nodiscard]] EditorView* view() const noexcept { return m_view.data(); }
    // Shows `summary` (also called on each of the followed view's insightChanged).
    void setSummary(const InsightSummary& summary);
    [[nodiscard]] const InsightSummary& summary() const noexcept { return m_summary; }
    // The icons per state; null icons are skipped.
    void setIcons(QIcon clean, QIcon analyzing, QIcon error, QIcon warning, QIcon info);
    // Whether it has something to say: no view followed, or the view's insight is enabled.
    [[nodiscard]] bool relevant() const noexcept;

signals:
    void problemsRequested();

private:
    void refresh();

    QPointer<EditorView> m_view;
    QMetaObject::Connection m_connection;
    InsightSummary m_summary;
    QIcon m_clean;
    QIcon m_analyzing;
    QIcon m_error;
    QIcon m_warning;
    QIcon m_info;
};

} // namespace frost::ui
