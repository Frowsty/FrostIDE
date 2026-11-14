// SPDX-License-Identifier: Apache-2.0
//
// Code Vision providers (UX-4, CPP-9): semantic declaration counts supplied by language index
// queries. A count is absent when the server cannot answer it; callers must not invent a value.
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/sched/task.hpp"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace frost::lang {

struct CodeVisionCounts {
    std::optional<std::size_t> usages;          // textDocument/references, declaration excluded.
    std::optional<std::size_t> overrides;       // textDocument/implementation for a callable.
    std::optional<std::size_t> implementations; // typeHierarchy/subtypes for a type.
};

// A declaration whose counts are requested. Position must be its server-reported selection range.
struct CodeVisionSymbol {
    LspPosition position;
    SymbolKind kind = SymbolKind::Variable;
};

class ICodeVisionProvider {
public:
    virtual ~ICodeVisionProvider() = default;
    // Queries one declaration. Counts derive from LSP index answers; unsupported metrics stay
    // empty and request errors propagate. The server must outlive the returned task. O(server work).
    [[nodiscard]] virtual sched::Task<Result<CodeVisionCounts>>
    counts(ILspRequester& server, std::string path, CodeVisionSymbol symbol, std::stop_token token = {}) const = 0;
};

// CPP-9 provider backed by clangd's workspace index through references, implementation and type
// hierarchy requests. It performs no source-text scanning.
class CppCodeVisionProvider final : public ICodeVisionProvider {
public:
    // Uses clangd's index-backed references, implementations and type hierarchy. No file reads or
    // source-text scans. Thread-safe; the server must outlive the returned task.
    [[nodiscard]] sched::Task<Result<CodeVisionCounts>>
    counts(ILspRequester& server, std::string path, CodeVisionSymbol symbol, std::stop_token token = {}) const override;
};

} // namespace frost::lang
