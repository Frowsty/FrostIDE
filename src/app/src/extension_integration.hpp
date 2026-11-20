// SPDX-License-Identifier: Apache-2.0
// Owner-thread public extension contribution catalog and explicit command/DAP product actions.
#pragma once
#include "frost/lang/extension_manifest.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
namespace frost::app {
class Workbench;
class ExtensionIntegration final : public QObject {
public:
    explicit ExtensionIntegration(Workbench& workbench);
    ~ExtensionIntegration() override;
    void poll();
    void shutdown();
    [[nodiscard]] Result<void> setCatalog(std::string manifest);
    [[nodiscard]] Result<std::string> composeLanguageRegistry(std::string_view base) const;
    [[nodiscard]] lang::Json snapshot() const;
    [[nodiscard]] Result<void> debugCurrentFile(std::string contributionId);
    [[nodiscard]] Result<void> executeCommand(std::string contributionId);

private:
    void configure();
    void chooseDebug();
    void chooseCommand();
    void revoke();
    static sched::Task<void> reportCommand(QPointer<ExtensionIntegration> self, std::uint64_t generation,
                                           sched::Task<Result<lang::Json>> task);
    Workbench& m_workbench;
    QTimer m_poll;
    std::optional<lang::ExtensionCatalog> m_catalog;
    std::string m_manifest, m_root, m_physicalRoot;
    std::uint64_t m_revision{}, m_generation{}, m_debugGeneration{};
    std::stop_source m_stop;
    std::shared_ptr<std::atomic<bool>> m_active = std::make_shared<std::atomic<bool>>(true);
    std::vector<sched::JoinHandle<void>> m_tasks;
    bool m_stopped{}, m_physicalIdentityRevoked{};
};
} // namespace frost::app
