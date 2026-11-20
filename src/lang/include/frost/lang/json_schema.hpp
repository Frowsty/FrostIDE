// SPDX-License-Identifier: Apache-2.0
// Defines schema-backed language services for workspace JSON files.
#pragma once
#include "frost/core/mutex.hpp"
#include "frost/lang/json_engine_references.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/snapshot_validator.hpp"

#include <map>
namespace frost::lang {
// Offline structural profiles, deliberately separate from engine content.validate (FEC-7).
// No remote schema resolution, shell expansion, or external process. Unknown properties remain
// legal for forward compatibility; bundled documented properties are validated/completed.
[[nodiscard]] bool hasBundledJsonSchema(std::string_view path);
[[nodiscard]] std::string_view bundledJsonSchemaCoverage(std::string_view path);
[[nodiscard]] Result<std::vector<SnapshotValidationIssue>>
inspectJsonSnapshot(std::string_view path, const text::Snapshot& source, const Json* engineSchema = nullptr);
class JsonSchemaService final : public ILspRequester {
public:
    // Caller authenticates/rechecks the selected engine identity. One selected project at a time;
    // clear on disconnect/trust/project change. Only the documented bounded structural subset is
    // interpreted locally; authoritative semantic diagnostics still come from document.validate.
    [[nodiscard]] Result<void> installEngineSchemas(std::string projectRoot, const Json& actualExport);
    // Retains actual schema.info row shape only while this captured table Schema reference matches.
    [[nodiscard]] Result<void> installEngineTableSchema(std::string absolutePath, const text::Snapshot&,
                                                        const Json& actualExport);
    void clearEngineSchemas();
    // Actual index callbacks own their immutable listing. Empty root/provider revokes all queries.
    void setAssetReferences(std::string projectRoot, JsonAssetReferenceProvider);
    using DeclarationReader = std::function<Result<text::Snapshot>(std::string_view root, std::string_view path)>;
    // Worker-safe bounded/contained source reader; captured open document text takes priority.
    void setDeclarationReader(DeclarationReader);
    [[nodiscard]] bool handles(std::string_view path) const;
    [[nodiscard]] std::uint64_t schemaGeneration() const;
    void updateDocument(std::string path, text::Snapshot snapshot);
    void removeDocument(std::string_view path);
    [[nodiscard]] std::optional<Json> snapshotValidationSchema(std::string_view path) const;
    [[nodiscard]] std::vector<SnapshotValidationIssue> snapshotIssues(std::string_view path) const;
    [[nodiscard]] sched::Task<Result<Json>> request(std::string method, Json params,
                                                    std::stop_token token = {}) override;
    [[nodiscard]] PositionEncoding encoding() const override { return PositionEncoding::Utf16; }
    [[nodiscard]] std::optional<ServerCapabilities> serverCapabilities() const override;

private:
    [[nodiscard]] std::optional<Json> engineSchemaLocked(std::string_view path) const;
    mutable Mutex m_mutex;
    std::map<std::string, text::Snapshot, std::less<>> m_documents;
    std::string m_engineRoot;
    std::map<std::string, Json, std::less<>> m_engineSchemas;
    std::map<std::string, std::pair<Json, Json>, std::less<>> m_engineTableSchemas;
    std::string m_assetRoot;
    JsonAssetReferenceProvider m_assetReferences;
    DeclarationReader m_declarationReader;
    std::uint64_t m_schemaGeneration = 0;
};
} // namespace frost::lang
