// SPDX-License-Identifier: Apache-2.0
// Plain-text status-bar projection of the persistent engine session's actual connection state.
#include "frost/ui/shell/engine_state.hpp"

namespace frost::ui {

EngineConnectionIndicator::EngineConnectionIndicator(QWidget* parent) : QLabel(parent) {
    setObjectName(QStringLiteral("engineConnectionIndicator"));
    setTextFormat(Qt::PlainText);
    setAccessibleName(tr("FrostEngine connection"));
    setState(EngineConnectionState::Disconnected);
}

void EngineConnectionIndicator::setState(EngineConnectionState state, const QString& detail) {
    m_state = state;
    QString status;
    switch (state) {
    case EngineConnectionState::Disconnected: status = tr("FrostEngine: Disconnected"); break;
    case EngineConnectionState::Connecting: status = tr("FrostEngine: Connecting"); break;
    case EngineConnectionState::Connected: status = tr("FrostEngine: Connected"); break;
    case EngineConnectionState::StaleDiscovery: status = tr("FrostEngine: Stale discovery"); break;
    case EngineConnectionState::WrongProject: status = tr("FrostEngine: Wrong project"); break;
    case EngineConnectionState::IncompatibleContract: status = tr("FrostEngine: Incompatible contract"); break;
    case EngineConnectionState::DebuggerPaused: status = tr("FrostEngine: Paused in debugger"); break;
    case EngineConnectionState::Paused: status = tr("FrostEngine: Paused"); break;
    }
    setText(status);
    setToolTip(QStringLiteral("<qt>") + status.toHtmlEscaped() +
               (detail.isEmpty() ? QString() : QStringLiteral("<br/>") + detail.toHtmlEscaped()) +
               QStringLiteral("</qt>"));
    setAccessibleDescription(detail.isEmpty() ? status : status + QStringLiteral(": ") + detail);
}

} // namespace frost::ui
