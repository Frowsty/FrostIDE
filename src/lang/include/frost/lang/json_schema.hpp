// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/diagnostics.hpp"
#include "frost/core/mutex.hpp"
#include <map>
namespace frost::lang {
// Offline structural profiles, deliberately separate from engine content.validate (FEC-7).
// No remote schema resolution, shell expansion, or external process. Unknown properties remain
// legal for forward compatibility; bundled documented properties are validated/completed.
[[nodiscard]] bool hasBundledJsonSchema(std::string_view path);
[[nodiscard]] std::string_view bundledJsonSchemaCoverage(std::string_view path);
class JsonSchemaService final : public ILspRequester {
public:
    void updateDocument(std::string path, text::Snapshot snapshot);
    void removeDocument(std::string_view path);
    [[nodiscard]] std::vector<LiveDiagnostic> diagnostics(std::string_view path) const;
    [[nodiscard]] sched::Task<Result<Json>> request(std::string method, Json params, std::stop_token token = {}) override;
    [[nodiscard]] PositionEncoding encoding() const override { return PositionEncoding::Utf16; }
    [[nodiscard]] std::optional<ServerCapabilities> serverCapabilities() const override;
private:
    mutable Mutex m_mutex;
    std::map<std::string, text::Snapshot, std::less<>> m_documents;
};
} // namespace frost::lang
