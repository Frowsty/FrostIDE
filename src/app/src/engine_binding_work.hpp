// SPDX-License-Identifier: Apache-2.0
// Worker-only captured inputs for FEC-13; no GUI object is accessed by analysis or LSP coroutines.
#pragma once
#include "frost/document/refactoring_transaction.hpp"
#include "frost/engine/binding_queries.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/project/trust_store.hpp"

#include <atomic>
#include <map>

namespace frost::app::binding_work {
[[nodiscard]] bool luaSource(std::string_view path) noexcept;
// No captured document or filesystem entry (including dangling links) may be shadowed.
[[nodiscard]] Result<void> admitBindingProjectionPath(io::IFileSystem& fs,
                                                      std::span<const engine::BindingDocumentCapture> documents,
                                                      std::string_view path);
struct DocumentPath {
    std::string physical;
    engine::BindingDocumentRole role = engine::BindingDocumentRole::ExternalReadOnly;
};
[[nodiscard]] Result<DocumentPath> resolveDocumentPath(io::IFileSystem& fs, std::string_view workspace,
                                                       std::string_view engine, std::string_view metadata,
                                                       std::string_view candidate);
[[nodiscard]] Result<std::string> readDocumentSnapshot(io::IFileSystem& fs, std::string_view physical,
                                                       std::size_t alreadyCapturedBytes);
struct RenameDefinitionAdmission {
    bool bound = false;
    std::string physicalMetadata;
};
// Worker-only actual semantic definition classification. Missing optional metadata is a normal
// no-engine state; failures of a definition or previously expected metadata never ordinary-route.
[[nodiscard]] Result<RenameDefinitionAdmission>
admitRenameDefinitions(io::IFileSystem& fs, const rpc::Json& definitions, std::string_view metadata,
                       std::string_view expectedPhysicalMetadata = {}, bool metadataRequired = false);
// Pure configuration snapshot evidence, not filesystem inference; no metadata I/O on GUI.
[[nodiscard]] bool expectsEngineMetadata(const rpc::Json& configuration, std::string_view metadataDirectory);
enum class Operation : std::uint8_t { Refresh, Usages, Preview };
struct Capture {
    io::IFileSystem* fs = nullptr;
    io::IProcessLauncher* launcher = nullptr;
    sched::IScheduler* scheduler = nullptr;
    project::TrustStore* trust = nullptr;
    std::shared_ptr<engine::EngineLuaHostProvider> host;
    std::shared_ptr<lang::ClangdService> native;
    std::shared_ptr<lang::LuaService> lua;
    std::shared_ptr<std::atomic<bool>> active;
    std::stop_token stop;
    std::uint64_t revision = 0, trustRevision = 0;
    std::string root, tool, stateDirectory, luaStateDirectory, requestTag;
    lang::CompileCommandsLocation database;
    engine::BindingProviderCapture nativeProvider, luaProvider;
    io::ProcessId nativePid = 0, luaPid = 0;
    unsigned nativeRestarts = 0, luaRestarts = 0;
    std::uint64_t luaConfigurationChanges = 0;
    std::vector<engine::BindingDocumentCapture> openDocuments;
    std::vector<lang::AnalysisDocumentLease> analysisDocuments;
    std::vector<std::string> candidates;
    std::string activePath, selectedLuaIdentity, newName;
    ByteOffset caret;
    bool renameNative = false;
    bool standardRoute = false;
    Operation operation = Operation::Refresh;
    [[nodiscard]] bool current() const;
};
struct Graph {
    std::shared_ptr<const engine::BindingIndex> index;
    std::string physicalRoot, engineRoot, databasePath, databaseBytes, metadataPath, program;
    std::string lexicalEngineRoot, physicalProgram;
    // Every original spelling is checked again, including aliases of parsed inputs and open buffers.
    std::map<std::string, std::string, std::less<>> physicalReceipts;
    IClock::SystemTime programMtime{};
    std::uint64_t engineTrustRevision = 0;
    std::vector<engine::BindingDocumentCapture> documents;
    bool embeddedLuaProfileVerified = true;
    std::vector<engine::BindingNameLiteral> embeddedLua;
    std::vector<std::string> embeddedLuaRefusals;
    std::map<std::string, std::string, std::less<>> inputHashes;
    [[nodiscard]] const engine::BindingDocumentCapture* generatedDocument() const;
    [[nodiscard]] bool verifyReceipts(io::IFileSystem& fs) const;
    [[nodiscard]] bool verify(const Capture& capture) const;
};
// Worker-only bounded document capture, split from provider admission for direct filesystem tests.
// Callers enforce Capture::current around this work; receipts are acquired once before file capture.
[[nodiscard]] Result<void> captureOpenDocumentReceipts(io::IFileSystem& fs, Graph& graph,
                                                       std::span<const engine::BindingDocumentCapture> openDocuments,
                                                       std::stop_token stop = {});
[[nodiscard]] Result<void> captureDocumentSnapshot(io::IFileSystem& fs, Graph& graph,
                                                   std::span<const engine::BindingDocumentCapture> openDocuments,
                                                   std::string path, std::size_t& bytes);
struct ResultData {
    std::shared_ptr<Graph> graph;
    std::vector<engine::BindingUsage> usages;
    std::optional<engine::BindingSelectedRenamePreview> preview;
    std::shared_ptr<const document::RefactoringPlan> applyPlan;
    // GUI staging only: acquire exact temporary native document ownership, then resume with
    // a newly captured lease/version. Workers never block waiting for a GUI callback.
    std::vector<engine::BindingDocumentCapture> nativeDocumentsToSynchronize;
    bool ordinaryUsages = false; // No proven binding edge; caller may explicitly use stock usages.
    std::string selectedIdentity;
    std::string previewDisplay;
    rpc::Json previewSummary;
};
[[nodiscard]] Result<std::shared_ptr<Graph>> buildGraph(const Capture& capture);
[[nodiscard]] sched::Task<Result<ResultData>> perform(Capture capture, std::shared_ptr<Graph> reusable = {});
} // namespace frost::app::binding_work
