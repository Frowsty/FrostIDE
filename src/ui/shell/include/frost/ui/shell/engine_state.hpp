// SPDX-License-Identifier: Apache-2.0
// Push-driven engine connection display and toolbar control state. No engine requests originate here.
#pragma once

#include <QLabel>
#include <QString>

#include <cstdint>

namespace frost::ui {

// Supplied by the app/session adapter; a command reply alone must not change this state.
enum class EngineConnectionState : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    StaleDiscovery,
    WrongProject,
    IncompatibleContract,
    DebuggerPaused,
    Paused
};

// Policy flags are supplied by the schema policy, never inferred from a command name in the UI.
struct EngineControlState {
    bool connected = false;
    bool playing = false;
    bool paused = false;
    bool debuggerPaused = false;
    bool canPlay = false;
    bool canPause = false;
    bool canStep = false;
    bool canStop = false;
    bool canSimulate = false;
};

// Parent-owned widget; all methods GUI thread only, O(detail length), no timers or transport.
class EngineConnectionIndicator final : public QLabel {
public:
    explicit EngineConnectionIndicator(QWidget* parent = nullptr);
    // Detail is the real session's reason; shown as plain text in the tooltip.
    void setState(EngineConnectionState state, const QString& detail = {});
    [[nodiscard]] EngineConnectionState state() const noexcept { return m_state; }

private:
    EngineConnectionState m_state = EngineConnectionState::Disconnected;
};

} // namespace frost::ui
