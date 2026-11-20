// SPDX-License-Identifier: Apache-2.0
// Per-consent/session C2 MCP endpoint over the existing current-user local IPC transport.
#pragma once
#include "frost/core/error.hpp"

#include <QJsonObject>
#include <QObject>

#include <functional>
#include <memory>
namespace frost::app {
class Workbench;
class AgentTools final : public QObject {
public:
    AgentTools(Workbench& owner, std::function<bool()> admission, QObject* parent = nullptr);
    ~AgentTools() override;
    [[nodiscard]] Result<void> start(const QString& root, const QString& configDirectory);
    void stop();
    [[nodiscard]] QString mcpConfig() const;
    [[nodiscard]] QString hookSettings() const;
    // Native chat uses the same tool dispatcher, never a bypass around scoped approvals.
    void request(const QJsonObject& message, std::function<void(QJsonObject)> reply);

private:
    struct State;
    std::unique_ptr<State> m;
};
} // namespace frost::app
