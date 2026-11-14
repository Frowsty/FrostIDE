// SPDX-License-Identifier: Apache-2.0
// Session selection and Stop All around the established debugger tool window; no synthetic data.
#include "frost/ui/debug_sessions_window.hpp"

#include "frost/ui/debug_tool_window.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>
namespace frost::ui {
DebugSessionsWindow::DebugSessionsWindow(DebugSessionRegistry& sessions, QWidget* parent)
    : QWidget(parent), m_sessions(sessions) {
    setObjectName(QStringLiteral("debugSessionsWindow"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* row = new QHBoxLayout;
    m_selector = new QComboBox(this);
    m_selector->setObjectName(QStringLiteral("debugSessionSelector"));
    m_selector->setAccessibleName(tr("Debugger session"));
    m_stopAll = new QPushButton(tr("Stop All"), this);
    m_stopAll->setObjectName(QStringLiteral("debugStopAll"));
    row->addWidget(m_selector, 1);
    row->addWidget(m_stopAll);
    layout->addLayout(row);
    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);
    connect(&sessions, &DebugSessionRegistry::sessionsChanged, this, &DebugSessionsWindow::refresh);
    connect(&sessions, &DebugSessionRegistry::sessionStateChanged, this, [this](DebugSessionId) { refresh(); });
    connect(&sessions, &DebugSessionRegistry::selectedChanged, this, &DebugSessionsWindow::select);
    connect(&sessions, &DebugSessionRegistry::sessionRemoving, this, [this](DebugSessionId id) {
        const auto found = m_views.find(id);
        if (found != m_views.end()) {
            delete found->second;
            m_views.erase(found);
        }
    });
    connect(m_selector, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0)
            static_cast<void>(m_sessions.select(m_selector->itemData(index).toULongLong()));
    });
    connect(m_stopAll, &QPushButton::clicked, &sessions, &DebugSessionRegistry::stopAll);
    refresh();
}
DebugToolWindow* DebugSessionsWindow::view(DebugSessionId id) const {
    const auto found = m_views.find(id);
    return found == m_views.end() ? nullptr : found->second;
}
void DebugSessionsWindow::setMixedDebugWidget(QWidget* widget) {
    if (m_mixed == widget)
        return;
    delete m_mixed;
    m_mixed = widget;
    if (widget)
        static_cast<QVBoxLayout*>(layout())->insertWidget(1, widget);
}
void DebugSessionsWindow::refresh() {
    const QSignalBlocker block(m_selector);
    m_selector->clear();
    for (const auto& entry : m_sessions.sessions()) {
        if (!m_views.contains(entry.id)) {
            auto* window = new DebugToolWindow(*entry.controller, m_stack);
            m_views.emplace(entry.id, window);
            m_stack->addWidget(window);
        }
        const auto state = entry.controller->statusText();
        m_selector->addItem(state.isEmpty() ? entry.name : entry.name + QStringLiteral(" — ") + state,
                            QVariant::fromValue<qulonglong>(entry.id));
    }
    m_stopAll->setEnabled(m_sessions.anyDebugging() || m_sessions.hasPendingWork());
    select(m_sessions.selectedId());
}
void DebugSessionsWindow::select(DebugSessionId id) {
    const QSignalBlocker block(m_selector);
    m_selector->setCurrentIndex(m_selector->findData(QVariant::fromValue<qulonglong>(id)));
    if (auto* window = view(id))
        m_stack->setCurrentWidget(window);
}
} // namespace frost::ui
