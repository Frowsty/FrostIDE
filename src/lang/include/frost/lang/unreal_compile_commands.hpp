// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/lang/compile_commands.hpp"
namespace frost::project { class TrustStore; }
namespace frost::lang {
struct UnrealCompileDatabaseSelection {
    std::string projectFile;
    std::string databaseFile; // Explicit generated compile_commands.json; no directory guessing.
    std::string configurationIdentity; // Selected UBT target/platform/configuration/tool identity.
};
// Rebase directory/file fields against the actual producer directory, preserving compiler argv or
// command byte-for-byte. Reject malformed/duplicate entries; never synthesize missing flags.
[[nodiscard]] Result<std::string> normalizeUnrealCompileDatabase(std::string_view bytes, std::string_view originDirectory);
// Worker-only provider. Captured project trust revision and explicit selected profile bind every
// locate. A retained bounded read is normalized into an immutable content-addressed IDE cache;
// clangd sees that snapshot, not a mutable engine-root file. fs/trust must outlive the provider.
class UnrealCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    UnrealCompileCommandsProvider(io::IFileSystem& fs, const project::TrustStore& trust,
        UnrealCompileDatabaseSelection selection, std::string cacheRoot, std::uint64_t trustRevision);
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate() override;
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate(std::stop_token stop) override;
private:
    io::IFileSystem& m_fs;
    const project::TrustStore& m_trust;
    UnrealCompileDatabaseSelection m_selection;
    std::string m_cacheRoot;
    std::uint64_t m_trustRevision;
};
} // namespace frost::lang
