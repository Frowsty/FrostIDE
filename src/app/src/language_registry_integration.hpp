// SPDX-License-Identifier: Apache-2.0
// Owner-thread application registry onboarding and supervised generic LSP sessions.
#pragma once
#include "frost/lang/language_registry.hpp"

#include <QObject>
#include <QTimer>

#include <map>
namespace frost::app {
class Workbench;
class DocumentEditorView;
class LanguageRegistryIntegration final : public QObject {
public:
    explicit LanguageRegistryIntegration(Workbench& workbench);
    ~LanguageRegistryIntegration() override;
    void poll();
    void shutdown();
    void sync(DocumentEditorView& view);
    void close(std::string_view path);
    void saved(std::string_view path);
    void configure();
    [[nodiscard]] Result<void> setManifest(std::string manifest);
    [[nodiscard]] std::shared_ptr<lang::LuaService> service(std::string_view path) const;
    [[nodiscard]] std::vector<lang::LiveDiagnostic> problems() const;
    [[nodiscard]] lang::Json snapshot() const;

private:
    struct Entry;
    void retire();
    void publish();
    Workbench& m_workbench;
    QTimer m_poll;
    std::string m_root, m_manifest, m_activePath, m_physicalRoot;
    std::uint64_t m_revision{}, m_generation{};
    bool m_stopped{}, m_physicalIdentityRevoked{};
    std::optional<lang::LanguageServerRegistry> m_registry;
    std::map<std::string, std::shared_ptr<Entry>> m_entries;
    std::vector<sched::JoinHandle<void>> m_retired;
};
} // namespace frost::app
