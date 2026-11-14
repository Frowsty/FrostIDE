// SPDX-License-Identifier: Apache-2.0
// Actual native/Lua same-pause presentation, supplied by the app's authenticated coordinator.
#pragma once
#include "frost/debug/debug_session.hpp"

#include <QWidget>

#include <cstdint>
#include <optional>
#include <string>
class QLabel;
class QPushButton;
class QTreeWidget;
namespace frost::ui {
struct MixedDebugStack {
    std::uint64_t sessionId = 0;
    std::int64_t threadId = 0;
    std::string title;
    debug::FrameList frames;
};
struct MixedDebugSnapshot {
    QString status;
    bool canCapture = false, canResume = false;
    std::int64_t processId = 0;
    std::optional<debug::dap::ScriptPauseIdentity> pause;
    std::optional<MixedDebugStack> native, lua;
};
// Displays supplied real frames only. No guessed interleaving or implicit debugger commands.
class MixedDebugWindow final : public QWidget {
    Q_OBJECT
public:
    explicit MixedDebugWindow(QWidget* parent = nullptr);
    void setSnapshot(const MixedDebugSnapshot& snapshot);
signals:
    void captureRequested();
    void resumeRequested();
    // Stable session/thread/frame identities let the app validate current pause before navigation.
    void frameRequested(std::uint64_t sessionId, std::int64_t threadId, std::int64_t frameId);

private:
    QLabel* m_status = nullptr;
    QPushButton *m_capture = nullptr, *m_resume = nullptr;
    QTreeWidget *m_native = nullptr, *m_lua = nullptr;
};
} // namespace frost::ui
