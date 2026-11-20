// SPDX-License-Identifier: Apache-2.0
// LTR-17 local, pinned and license-governed stdio language server onboarding.
#pragma once
#include "frost/io/file_system.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/project/trust_store.hpp"

#include <atomic>
namespace frost::lang {
struct LanguageServerArtifact {
    std::string path, sha256;
};
struct LanguageServerDefinition {
    std::string id, name, languageId, version, executable, license, licensePath, licenseSha256;
    std::vector<std::string> extensions, arguments;
    std::vector<LanguageServerArtifact> artifacts;
    Json configuration = Json::object(), initializationOptions;
    bool enabled = false;
};
class LanguageServerRegistry {
public:
    // Pure, bounded version-1 manifest reader; no discovery, install, launch or inferred licenses.
    // Duplicate selectors and built-in language overrides fail. O(manifest bytes + n log n).
    [[nodiscard]] static Result<LanguageServerRegistry> parse(std::string_view manifest);
    [[nodiscard]] const std::vector<LanguageServerDefinition>& definitions() const noexcept { return m_definitions; }
    [[nodiscard]] const LanguageServerDefinition* forPath(std::string_view path) const;

private:
    std::vector<LanguageServerDefinition> m_definitions;
};
// Same simple SPDX expressions as tools/licensecheck.py ALLOWLIST. No project-controlled exceptions.
[[nodiscard]] bool registryLicenseAllowed(std::string_view spdx) noexcept;
// All named artifacts and license evidence must be bounded regular files with exact SHA256 pins.
// Caller runs this blocking read on a worker. It proves bytes, not authenticity of declared version/license.
[[nodiscard]] Result<void> verifyLanguageServer(io::IFileSystem& fs, const LanguageServerDefinition& definition);
[[nodiscard]] LuaServiceOptions registeredLanguageOptions(const LanguageServerDefinition& definition, std::string root);
// Immutable per-workspace admission. Every supervised launch/restart checks trust revision, exact
// root identity and artifact bytes before delegating to IProcessLauncher. No shell or PATH lookup.
// Its fs/launcher/trust references outlive all service stop tasks. Concurrent launches are safe.
class RegistryProcessLauncher final : public io::IProcessLauncher {
public:
    RegistryProcessLauncher(io::IProcessLauncher& launcher, io::IFileSystem& fs, project::TrustStore& trust,
                            LanguageServerDefinition definition, std::string root, std::uint64_t revision,
                            std::string physicalRoot);
    void revoke() noexcept { m_active.store(false); }
    [[nodiscard]] Result<void> admitWorkspace() const;
    [[nodiscard]] Result<void> admitDocument(std::string_view path) const;
    [[nodiscard]] Result<void> admitEdit(const Json& workspaceEdit) const;
    [[nodiscard]] Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override;
    [[nodiscard]] Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                                         const io::AsyncContext& context) override;

private:
    [[nodiscard]] Result<void> admit(const io::ProcessSpec& spec);
    io::IProcessLauncher& m_launcher;
    io::IFileSystem& m_fs;
    project::TrustStore& m_trust;
    LanguageServerDefinition m_definition;
    std::string m_root, m_physicalRoot;
    std::uint64_t m_revision;
    std::atomic<bool> m_active{true};
};
// Retained requester boundary: each request/reply and incoming applyEdit rechecks the captured
// workspace identity/trust. Documents outside that physical workspace never reach this server.
class RegistryLspRequester final : public ILspRequester {
public:
    RegistryLspRequester(std::shared_ptr<LuaService> service, std::shared_ptr<RegistryProcessLauncher> admission,
                         std::function<bool(std::function<void()>)> dispatch = {})
        : m_service(std::move(service)), m_admission(std::move(admission)), m_dispatch(std::move(dispatch)) {}
    [[nodiscard]] sched::Task<Result<Json>> request(std::string method, Json params,
                                                    std::stop_token token = {}) override;
    [[nodiscard]] PositionEncoding encoding() const override { return m_service->encoding(); }
    [[nodiscard]] std::optional<ServerCapabilities> serverCapabilities() const override;
    void setApplyEditHandler(ApplyEditHandler handler) override;

private:
    static sched::Task<Result<Json>> invoke(std::shared_ptr<LuaService> service,
                                            std::shared_ptr<RegistryProcessLauncher> admission, std::string method,
                                            Json params, std::stop_token token);
    std::shared_ptr<LuaService> m_service;
    std::shared_ptr<RegistryProcessLauncher> m_admission;
    std::function<bool(std::function<void()>)> m_dispatch;
};
} // namespace frost::lang
