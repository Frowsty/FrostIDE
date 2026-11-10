// SPDX-License-Identifier: Apache-2.0
//
// Document links, folding ranges and clangd's AST (document_features.hpp).

#include "frost/lang/document_features.hpp"

#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"

#include <utility>

namespace frost::lang {
namespace {

// A non-negative integer member that fits 32 bits, else nullopt.
[[nodiscard]] std::optional<std::uint32_t> unsignedAt(const Json& object, std::string_view key) {
    const std::optional<std::int64_t> value = object.integerAt(key);
    if (!value || *value < 0 || *value > static_cast<std::int64_t>(UINT32_MAX)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

[[nodiscard]] FoldingRangeKind parseFoldingKind(std::string_view kind) noexcept {
    if (kind == "comment") {
        return FoldingRangeKind::Comment;
    }
    if (kind == "imports") {
        return FoldingRangeKind::Imports;
    }
    if (kind == "region") {
        return FoldingRangeKind::Region;
    }
    return FoldingRangeKind::Other;
}

// One ASTNode at `depth` (0 for the root); children past kMaxAstDepth are dropped.
[[nodiscard]] Result<AstNode> parseAstObject(const Json& json, std::size_t depth) {
    if (!json.isObject()) {
        return fail(ErrorCode::InvalidArgument, "ast: a node is not an object");
    }
    AstNode node;
    node.role = std::string(json.stringAt("role"));
    node.kind = std::string(json.stringAt("kind"));
    if (node.role.empty() && node.kind.empty()) {
        return fail(ErrorCode::InvalidArgument, "ast: a node has neither role nor kind");
    }
    node.detail = std::string(json.stringAt("detail"));
    node.arcana = std::string(json.stringAt("arcana"));
    if (const Json* range = json.find("range"); range != nullptr) {
        node.range = parseRange(*range);
    }
    if (const Json* children = json.find("children"); children != nullptr && children->isArray()) {
        if (depth + 1 >= kMaxAstDepth) {
            node.truncated = children->size() > 0;
            return node;
        }
        node.children.reserve(children->size());
        for (const Json& child : children->items()) {
            auto parsed = parseAstObject(child, depth + 1);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            node.children.push_back(std::move(*parsed));
        }
    }
    return node;
}

} // namespace

Result<std::vector<DocumentLink>> parseDocumentLinks(const Json& json) {
    std::vector<DocumentLink> out;
    if (json.isNull()) {
        return out;
    }
    if (!json.isArray()) {
        return fail(ErrorCode::InvalidArgument, "documentLink: the answer is not an array");
    }
    out.reserve(json.size());
    for (const Json& item : json.items()) {
        const Json* rangeJson = item.find("range");
        const std::optional<LspRange> range = rangeJson != nullptr ? parseRange(*rangeJson) : std::nullopt;
        if (!range) {
            continue;
        }
        DocumentLink link;
        link.range = *range;
        link.uri = std::string(item.stringAt("target"));
        if (!link.uri.empty() && link.uri.starts_with("file:")) {
            if (Result<std::string> path = uriToPath(link.uri)) {
                link.path = std::move(*path);
            }
        }
        if (const Json* tooltip = item.find("tooltip"); tooltip != nullptr && tooltip->asString() != nullptr) {
            link.tooltip = *tooltip->asString();
        }
        out.push_back(std::move(link));
    }
    return out;
}

sched::Task<Result<std::vector<DocumentLink>>> requestDocumentLinks(ILspRequester& server, std::string path,
                                                                    std::stop_token token) {
    auto document = textDocumentIdentifier(path);
    if (!document) {
        co_return std::unexpected(std::move(document.error()));
    }
    auto answered =
        co_await server.request("textDocument/documentLink", Json::object().set("textDocument", *document), token);
    if (!answered) {
        co_return std::unexpected(std::move(answered.error()));
    }
    co_return parseDocumentLinks(*answered);
}

Result<std::vector<FoldingRange>> parseFoldingRanges(const Json& json) {
    std::vector<FoldingRange> out;
    if (json.isNull()) {
        return out;
    }
    if (!json.isArray()) {
        return fail(ErrorCode::InvalidArgument, "foldingRange: the answer is not an array");
    }
    out.reserve(json.size());
    for (const Json& item : json.items()) {
        const std::optional<std::uint32_t> startLine = unsignedAt(item, "startLine");
        const std::optional<std::uint32_t> endLine = unsignedAt(item, "endLine");
        if (!startLine || !endLine || *endLine < *startLine) {
            continue;
        }
        FoldingRange range;
        range.startLine = *startLine;
        range.endLine = *endLine;
        range.startCharacter = unsignedAt(item, "startCharacter");
        range.endCharacter = unsignedAt(item, "endCharacter");
        if (range.startLine == range.endLine && range.startCharacter && range.endCharacter &&
            *range.endCharacter < *range.startCharacter) {
            continue;
        }
        range.kind = parseFoldingKind(item.stringAt("kind"));
        out.push_back(range);
    }
    return out;
}

sched::Task<Result<std::vector<FoldingRange>>> requestFoldingRanges(ILspRequester& server, std::string path,
                                                                    std::stop_token token) {
    auto document = textDocumentIdentifier(path);
    if (!document) {
        co_return std::unexpected(std::move(document.error()));
    }
    auto answered =
        co_await server.request("textDocument/foldingRange", Json::object().set("textDocument", *document), token);
    if (!answered) {
        co_return std::unexpected(std::move(answered.error()));
    }
    co_return parseFoldingRanges(*answered);
}

Json foldingRangeClientCapabilities() {
    return Json::object().set("dynamicRegistration", false).set("lineFoldingOnly", false);
}

Json documentLinkClientCapabilities() {
    return Json::object().set("dynamicRegistration", false).set("tooltipSupport", true);
}

Result<std::optional<AstNode>> parseAstNode(const Json& json) {
    if (json.isNull()) {
        return std::optional<AstNode>{};
    }
    auto node = parseAstObject(json, 0);
    if (!node) {
        return std::unexpected(std::move(node.error()));
    }
    return std::optional<AstNode>(std::move(*node));
}

std::size_t astNodeCount(const AstNode& node) noexcept {
    std::size_t count = 1;
    for (const AstNode& child : node.children) {
        count += astNodeCount(child);
    }
    return count;
}

sched::Task<Result<std::optional<AstNode>>> requestAst(ILspRequester& server, std::string path, LspRange range,
                                                       std::stop_token token) {
    auto document = textDocumentIdentifier(path);
    if (!document) {
        co_return std::unexpected(std::move(document.error()));
    }
    Json params = Json::object().set("textDocument", *document).set("range", toJson(range));
    auto answered = co_await server.request("textDocument/ast", std::move(params), token);
    if (!answered) {
        co_return std::unexpected(std::move(answered.error()));
    }
    co_return parseAstNode(*answered);
}

} // namespace frost::lang
