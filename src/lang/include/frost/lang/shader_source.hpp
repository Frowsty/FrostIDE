// SPDX-License-Identifier: Apache-2.0
// Read-only shader source include navigation; actual semantic LSP servers remain registry-owned.
#pragma once
#include "frost/io/file_system.hpp"
#include "frost/lang/lsp_client.hpp"
namespace frost::lang {
struct ShaderIncludeRoot {
    std::string virtualPrefix, directory, physicalDirectory, provenance;
};
struct ShaderLanguageInput {
    std::string path, projectRoot, physicalProjectRoot;
    text::Snapshot source;
    // Explicit authoritative compiler/Unity mappings only. No PackageCache/version-folder guessing.
    std::vector<ShaderIncludeRoot> roots;
};
inline constexpr std::size_t kShaderMaxIncludeRoots = 32;
inline constexpr std::size_t kShaderMaxCandidates = 64;
[[nodiscard]] Result<Json> queryShaderSource(io::IFileSystem& fs, const ShaderLanguageInput& input,
                                             std::string_view method, const Json& params, std::stop_token stop = {});
class ShaderSourceService final : public ILspRequester {
public:
    // Caller captures source/root identities on owner thread. fs/scheduler outlive request tasks;
    // tasks retain values, never this service or a GUI object. Read-only, no processes/writes.
    ShaderSourceService(io::IFileSystem& fs, sched::IScheduler& scheduler) : m_fs(fs), m_scheduler(scheduler) {}
    void setInput(ShaderLanguageInput input);
    [[nodiscard]] sched::Task<Result<Json>> request(std::string method, Json params,
                                                    std::stop_token stop = {}) override;
    PositionEncoding encoding() const override { return PositionEncoding::Utf16; }
    std::optional<ServerCapabilities> serverCapabilities() const override;

private:
    static sched::Task<Result<Json>> answer(io::IFileSystem& fs, sched::IScheduler& scheduler,
                                            ShaderLanguageInput input, std::string method, Json params,
                                            std::stop_token stop);
    io::IFileSystem& m_fs;
    sched::IScheduler& m_scheduler;
    mutable Mutex m_mutex;
    ShaderLanguageInput m_input FROST_GUARDED_BY(m_mutex);
};
} // namespace frost::lang
