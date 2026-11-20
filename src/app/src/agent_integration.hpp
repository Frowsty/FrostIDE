// SPDX-License-Identifier: Apache-2.0
// Consent-scoped C1 editor tools and opt-in unmodified CLI Agent Terminal ownership.
#pragma once
#include <QObject>
#include <memory>
namespace frost::app {
class Workbench;
class AgentIntegration final : public QObject {
public:
    explicit AgentIntegration(Workbench& owner);
    ~AgentIntegration() override;
    void enable(bool launchTerminal);
    void stop();
    void mention();
    bool active() const;
private:
    struct State;
    std::unique_ptr<State> m;
};
}
