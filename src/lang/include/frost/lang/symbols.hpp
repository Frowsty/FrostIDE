// SPDX-License-Identifier: Apache-2.0
//
// File structure (Ctrl+F12: textDocument/documentSymbol as a tree with speed search) and go to
// symbol (Ctrl+Alt+Shift+N: workspace/symbol, ranked by the fuzzy matcher) for W2 (ADR 0041).
/*
 * Trees. Servers answer documentSymbol with a DocumentSymbol tree (FrostIDE announces
 * hierarchical support) or a flat SymbolInformation list; both become DocumentSymbol nodes
 * (the flat form without children). Speed search keeps a node when its name matches the query
 * or when one of its descendants does, so the matches keep their context; the matched nodes
 * are flagged for highlighting. flattenSymbols lists the tree depth-first for list views.
 *
 * Ranking (rankWorkspaceSymbols): by match kind, then fuzzy score, then the shorter name, then
 * the container and the path, so "fooBar" ranks foo_bar's exact prefix before a fuzzy hit.
 * Threads: the requests are coroutines; the rest is pure.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/fuzzy.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/sched/task.hpp"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

struct DocumentSymbol {
    std::string name;
    std::string detail; // The type or signature, when the server sends it.
    SymbolKind kind = SymbolKind::Variable;
    LspRange range;          // The whole declaration.
    LspRange selectionRange; // The name (where navigation goes).
    std::vector<DocumentSymbol> children;
    bool matched = false; // Set by filterSymbols: the name matched the query.
};

// A DocumentSymbol tree or a SymbolInformation list (see the header comment).
[[nodiscard]] std::vector<DocumentSymbol> parseDocumentSymbols(const Json& json);

// The tree pruned to the matches of `query` and their ancestors; an empty query keeps everything.
[[nodiscard]] std::vector<DocumentSymbol> filterSymbols(const std::vector<DocumentSymbol>& symbols,
                                                        std::string_view query);

// One row of a flattened tree: depth (0 for top-level) and the node.
struct SymbolRow {
    std::size_t depth = 0;
    const DocumentSymbol* symbol = nullptr;
};

// Depth-first, in document order. The rows point into `symbols`.
[[nodiscard]] std::vector<SymbolRow> flattenSymbols(const std::vector<DocumentSymbol>& symbols);

// textDocument/documentSymbol. Errors: request's.
[[nodiscard]] sched::Task<Result<std::vector<DocumentSymbol>>>
requestDocumentSymbols(ILspRequester& server, std::string path, std::stop_token token = {});

struct WorkspaceSymbol {
    std::string name;
    std::string containerName; // "ns::Class", or "".
    SymbolKind kind = SymbolKind::Variable;
    Location location;
    FuzzyMatch match; // Set by rankWorkspaceSymbols.
};

// SymbolInformation[] or WorkspaceSymbol[] (a location without a range points at the file start).
[[nodiscard]] std::vector<WorkspaceSymbol> parseWorkspaceSymbols(const Json& json);

// The symbols matching `query`, ranked (see the header comment).
[[nodiscard]] std::vector<WorkspaceSymbol> rankWorkspaceSymbols(std::vector<WorkspaceSymbol> symbols,
                                                                std::string_view query);

// workspace/symbol, ranked for `query`. Errors: request's.
[[nodiscard]] sched::Task<Result<std::vector<WorkspaceSymbol>>>
requestWorkspaceSymbols(ILspRequester& server, std::string query, std::stop_token token = {});

} // namespace frost::lang
