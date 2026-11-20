// SPDX-License-Identifier: Apache-2.0
// Explicit user-operated native AI Agent panel, independent from end-user Agent Terminal auth.
#pragma once
#include <QObject>
#include <memory>
namespace frost::app {
class Workbench;
class NativeAgentChat final : public QObject {
public:
    explicit NativeAgentChat(Workbench& owner);
    ~NativeAgentChat() override;
    void show();
    void stop();
private:
    struct State;
    std::unique_ptr<State> m;
};
}
