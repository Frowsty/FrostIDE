// SPDX-License-Identifier: Apache-2.0
// Debug tool window's session selector; every session retains its own existing debugger view.
#pragma once
#include "frost/ui/debug_session_registry.hpp"

#include <QWidget>

#include <map>
class QComboBox;
class QStackedWidget;
class QPushButton;
namespace frost::ui {
class DebugToolWindow;
// GUI-thread view. Registry outlives this widget; each per-session console/model keeps its state.
class DebugSessionsWindow final : public QWidget {
    Q_OBJECT
public:
    explicit DebugSessionsWindow(DebugSessionRegistry& sessions, QWidget* parent = nullptr);
    [[nodiscard]] DebugToolWindow* view(DebugSessionId id) const;
    void setMixedDebugWidget(QWidget* widget);

private:
    void refresh();
    void select(DebugSessionId id);
    DebugSessionRegistry& m_sessions;
    QComboBox* m_selector = nullptr;
    QStackedWidget* m_stack = nullptr;
    QPushButton* m_stopAll = nullptr;
    QWidget* m_mixed = nullptr;
    std::map<DebugSessionId, DebugToolWindow*> m_views;
};
} // namespace frost::ui
