// SPDX-License-Identifier: Apache-2.0
//
// The per-document language-server features of CPP-5 that have no model elsewhere: document links
// (textDocument/documentLink, LSP 3.17), folding ranges (textDocument/foldingRange, LSP 3.17) and
// clangd's AST dump (textDocument/ast, clangd 12+, clangd.llvm.org/extensions#ast). Written from the
// LSP 3.17 specification and clangd's public extension documentation; Qt-free.
/*
 * Document links. clangd answers with one link per #include whose header it resolved: the range of
 * the quoted or angled file name and the header's file URI as `target`. Links without a target
 * (a server that resolves them lazily with documentLink/resolve) are kept with an empty path;
 * FrostIDE announces no resolve support, so clangd always sends the target. Only file: targets
 * become paths; other schemes stay in `uri` with `path` empty.
 *
 * Folding ranges. clangd sends startLine/endLine (0-based) and, as FrostIDE announces
 * lineFoldingOnly = false, startCharacter/endCharacter in the negotiated position encoding. A range
 * whose end comes before its start is dropped. The kind is "comment", "imports", "region" or absent.
 *
 * AST. clangd answers textDocument/ast ({textDocument, range}) with the smallest node that encloses
 * the range, as ASTNode {role, kind, detail?, arcana?, range?, children?}, or null when the range
 * is outside every node (for example a blank line between functions). The tree is parsed
 * recursively up to kMaxAstDepth levels; deeper children are dropped and the node is marked
 * `truncated`, so a pathological answer cannot exhaust the stack.
 *
 * Threads: the parsers are pure and thread-safe; the requests are coroutines that run on the
 * requester's scheduler and support cancellation through their stop_token.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/sched/task.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace frost::lang {

// --- Document links ---

struct DocumentLink {
    LspRange range;      // The linked text (an #include's file name).
    std::string uri;     // The target as the server sent it ("" when it sent none).
    std::string path;    // The target's absolute path, "" for non-file targets.
    std::string tooltip; // The server's tooltip, if any.
    friend bool operator==(const DocumentLink&, const DocumentLink&) = default;
};

// DocumentLink[] (null: none). Entries without a valid range are dropped. Errors: InvalidArgument
// (neither null nor an array). O(links).
[[nodiscard]] Result<std::vector<DocumentLink>> parseDocumentLinks(const Json& json);

// textDocument/documentLink for `path`. Errors: the requester's, InvalidArgument (path, answer).
[[nodiscard]] sched::Task<Result<std::vector<DocumentLink>>>
requestDocumentLinks(ILspRequester& server, std::string path, std::stop_token token = {});

// --- Folding ranges ---

enum class FoldingRangeKind : std::uint8_t { Other, Comment, Imports, Region };

struct FoldingRange {
    std::uint32_t startLine = 0;
    std::optional<std::uint32_t> startCharacter; // In the negotiated position encoding.
    std::uint32_t endLine = 0;
    std::optional<std::uint32_t> endCharacter;
    FoldingRangeKind kind = FoldingRangeKind::Other;
    friend bool operator==(const FoldingRange&, const FoldingRange&) = default;
};

// FoldingRange[] (null: none). Ranges without both lines, or ending before they start, are dropped.
// Errors: InvalidArgument (neither null nor an array). O(ranges).
[[nodiscard]] Result<std::vector<FoldingRange>> parseFoldingRanges(const Json& json);

// textDocument/foldingRange for `path`. Errors: the requester's, InvalidArgument (path, answer).
[[nodiscard]] sched::Task<Result<std::vector<FoldingRange>>>
requestFoldingRanges(ILspRequester& server, std::string path, std::stop_token token = {});

// The textDocument.foldingRange client capability (lsp_client.cpp announces it).
[[nodiscard]] Json foldingRangeClientCapabilities();
// The textDocument.documentLink client capability (no resolve, no tooltips needed).
[[nodiscard]] Json documentLinkClientCapabilities();

// --- clangd's AST ---

inline constexpr std::size_t kMaxAstDepth = 256;

struct AstNode {
    std::string role;   // "expression", "statement", "declaration", "type", ...
    std::string kind;   // "BinaryOperator", "CXXRecord", ...
    std::string detail; // A name or operator, when clangd sends one.
    std::string arcana; // clangd's dump line for the node (the -ast-dump text).
    std::optional<LspRange> range;
    std::vector<AstNode> children;
    bool truncated = false; // Children past kMaxAstDepth were dropped.
    friend bool operator==(const AstNode&, const AstNode&) = default;
};

// An ASTNode; nullopt for null. Errors: InvalidArgument (not an object, or no role and kind).
[[nodiscard]] Result<std::optional<AstNode>> parseAstNode(const Json& json);

// The number of nodes in `node`'s tree (itself included). O(nodes).
[[nodiscard]] std::size_t astNodeCount(const AstNode& node) noexcept;

// textDocument/ast for `range` of `path` (clangd's extension; ServerCapabilities::ast). nullopt
// when no node encloses the range. Errors: the requester's, parseAstNode's.
[[nodiscard]] sched::Task<Result<std::optional<AstNode>>> requestAst(ILspRequester& server, std::string path,
                                                                     LspRange range, std::stop_token token = {});

} // namespace frost::lang
