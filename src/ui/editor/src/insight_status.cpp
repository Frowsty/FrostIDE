// SPDX-License-Identifier: Apache-2.0
//
// InsightStatusIndicator (frost/ui/editor/insight_status.hpp): the status bar's problems indicator.

#include "frost/ui/editor/insight_status.hpp"

#include "frost/ui/editor/editor_view.hpp"

#include <utility>

namespace frost::ui {

InsightStatusIndicator::InsightStatusIndicator(QWidget* parent) : QToolButton(parent) {
    setObjectName(QStringLiteral("insightStatusIndicator"));
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(this, &QToolButton::clicked, this, &InsightStatusIndicator::problemsRequested);
    refresh();
}

void InsightStatusIndicator::setView(EditorView* view) {
    if (m_connection) {
        disconnect(m_connection);
    }
    m_view = view;
    if (view != nullptr) {
        m_connection = connect(view, &EditorView::insightChanged, this, [this] {
            if (m_view) {
                setSummary(m_view->insightSummary());
            }
        });
        m_summary = view->insightSummary();
    } else {
        m_summary = InsightSummary{};
    }
    refresh();
}

void InsightStatusIndicator::setSummary(const InsightSummary& summary) {
    m_summary = summary;
    refresh();
}

void InsightStatusIndicator::setIcons(QIcon clean, QIcon analyzing, QIcon error, QIcon warning, QIcon info) {
    m_clean = std::move(clean);
    m_analyzing = std::move(analyzing);
    m_error = std::move(error);
    m_warning = std::move(warning);
    m_info = std::move(info);
    refresh();
}

void InsightStatusIndicator::refresh() {
    const QString text = QString::fromStdString(describe(m_summary));
    setText(text);
    setToolTip(text);
    QIcon icon;
    switch (m_summary.worst().value_or(InsightSeverity::Info)) {
    case InsightSeverity::Error: icon = m_error; break;
    case InsightSeverity::Warning: icon = m_warning; break;
    case InsightSeverity::WeakWarning:
    case InsightSeverity::Info: icon = m_info; break;
    }
    if (m_summary.clean()) {
        icon = m_summary.analyzing ? m_analyzing : m_clean;
    }
    setIcon(icon);
    // Shown while it says something; only as a child (never a top-level window of its own).
    if (parentWidget() != nullptr) {
        setVisible(relevant());
    }
}

bool InsightStatusIndicator::relevant() const noexcept { return m_view.isNull() || m_view->insightEnabled(); }

} // namespace frost::ui
