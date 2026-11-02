// SPDX-License-Identifier: Apache-2.0
// Workbench integration for FrostEngine automation controls and results.
#pragma once
#include <QObject>

#include <memory>
namespace frost::app {
class Workbench;
class EngineIntegration final : public QObject {
public:
    explicit EngineIntegration(Workbench& workbench);
    ~EngineIntegration() override;
    void cancel();
    // Stops accepting work, cancels requests and drains them before the scheduler is destroyed.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
