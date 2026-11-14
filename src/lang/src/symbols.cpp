// SPDX-License-Identifier: Apache-2.0
//
// Document and workspace symbols (symbols.hpp).

#include "frost/lang/symbols.hpp"

#include "frost/lang/uri.hpp"

#include <algorithm>

namespace frost::lang {
namespace {

[[nodiscard]] std::optional<DocumentSymbol> parseDocumentSymbol(const Json& json) {
    if (!json.isObject()) {
        return std::nullopt;
    }
    DocumentSymbol symbol;
    symbol.name = std::string(json.stringAt("name"));
    symbol.detail = std::string(json.stringAt("detail"));
    if (const Json* kind = json.find("kind"); kind != nullptr) {
        symbol.kind = parseSymbolKind(*kind);
    }
    if (const Json* range = json.find("range"); range != nullptr) { // DocumentSymbol.
        auto whole = parseRange(*range);
        if (!whole) {
            return std::nullopt;
        }
        symbol.range = *whole;
        const Json* selection = json.find("selectionRange");
        symbol.selectionRange = (selection != nullptr ? parseRange(*selection) : std::nullopt).value_or(*whole);
        if (const Json* children = json.find("children"); children != nullptr) {
            for (const Json& child : children->items()) {
                if (auto parsed = parseDocumentSymbol(child)) {
                    symbol.children.push_back(std::move(*parsed));
                }
            }
        }
        return symbol;
    }
    if (const Json* location = json.find("location"); location != nullptr) { // SymbolInformation.
        auto parsed = parseLocation(*location);
        if (!parsed) {
            return std::nullopt;
        }
        symbol.range = parsed->range;
        symbol.selectionRange = parsed->range;
        symbol.detail = std::string(json.stringAt("containerName"));
        return symbol;
    }
    return std::nullopt;
}

void filterInto(const std::vector<DocumentSymbol>& symbols, std::string_view query, std::vector<DocumentSymbol>& out) {
    for (const DocumentSymbol& symbol : symbols) {
        DocumentSymbol kept;
        kept.name = symbol.name;
        kept.detail = symbol.detail;
        kept.kind = symbol.kind;
        kept.range = symbol.range;
        kept.selectionRange = symbol.selectionRange;
        kept.matched = fuzzyMatch(query, symbol.name).has_value();
        filterInto(symbol.children, query, kept.children);
        if (kept.matched || !kept.children.empty()) {
            out.push_back(std::move(kept));
        }
    }
}

void flattenInto(const std::vector<DocumentSymbol>& symbols, std::size_t depth, std::vector<SymbolRow>& out) {
    for (const DocumentSymbol& symbol : symbols) {
        out.push_back(SymbolRow{.depth = depth, .symbol = &symbol});
        flattenInto(symbol.children, depth + 1, out);
    }
}

} // namespace

std::vector<DocumentSymbol> parseDocumentSymbols(const Json& json) {
    std::vector<DocumentSymbol> out;
    for (const Json& item : json.items()) {
        if (auto symbol = parseDocumentSymbol(item)) {
            out.push_back(std::move(*symbol));
        }
    }
    return out;
}

std::vector<DocumentSymbol> filterSymbols(const std::vector<DocumentSymbol>& symbols, std::string_view query) {
    std::vector<DocumentSymbol> out;
    filterInto(symbols, query, out);
    return out;
}

std::vector<SymbolRow> flattenSymbols(const std::vector<DocumentSymbol>& symbols) {
    std::vector<SymbolRow> out;
    flattenInto(symbols, 0, out);
    return out;
}

sched::Task<Result<std::vector<DocumentSymbol>>> requestDocumentSymbols(ILspRequester& server, std::string path,
                                                                        std::stop_token token) {
    auto document = textDocumentIdentifier(path);
    if (!document) {
        co_return std::unexpected(std::move(document.error()));
    }
    auto result = co_await server.request("textDocument/documentSymbol",
                                          Json::object().set("textDocument", std::move(*document)), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseDocumentSymbols(*result);
}

std::vector<WorkspaceSymbol> parseWorkspaceSymbols(const Json& json) {
    std::vector<WorkspaceSymbol> out;
    for (const Json& item : json.items()) {
        const Json* location = item.find("location");
        if (location == nullptr) {
            continue;
        }
        WorkspaceSymbol symbol;
        symbol.name = std::string(item.stringAt("name"));
        symbol.containerName = std::string(item.stringAt("containerName"));
        if (const Json* kind = item.find("kind"); kind != nullptr) {
            symbol.kind = parseSymbolKind(*kind);
        }
        if (auto parsed = parseLocation(*location)) {
            symbol.location = std::move(*parsed);
        } else if (auto path = uriToPath(location->stringAt("uri"))) { // WorkspaceSymbol with {uri} only.
            symbol.location =
                Location{.path = std::move(*path), .uri = std::string(location->stringAt("uri")), .range = {}};
        } else {
            continue;
        }
        out.push_back(std::move(symbol));
    }
    return out;
}

std::vector<WorkspaceSymbol> rankWorkspaceSymbols(std::vector<WorkspaceSymbol> symbols, std::string_view query) {
    std::vector<WorkspaceSymbol> out;
    for (WorkspaceSymbol& symbol : symbols) {
        if (auto match = fuzzyMatch(query, symbol.name)) {
            symbol.match = std::move(*match);
            out.push_back(std::move(symbol));
        }
    }
    std::ranges::stable_sort(out, [](const WorkspaceSymbol& a, const WorkspaceSymbol& b) {
        if (a.match.kind != b.match.kind) {
            return a.match.kind < b.match.kind;
        }
        if (a.match.score != b.match.score) {
            return a.match.score > b.match.score;
        }
        if (a.name.size() != b.name.size()) {
            return a.name.size() < b.name.size();
        }
        if (a.containerName != b.containerName) {
            return a.containerName < b.containerName;
        }
        return a.location.path < b.location.path;
    });
    return out;
}

sched::Task<Result<std::vector<WorkspaceSymbol>>> requestWorkspaceSymbols(ILspRequester& server, std::string query,
                                                                          std::stop_token token) {
    auto result = co_await server.request("workspace/symbol", Json::object().set("query", query), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return rankWorkspaceSymbols(parseWorkspaceSymbols(*result), query);
}

bool isClassLikeSymbol(SymbolKind kind) noexcept {
    return kind == SymbolKind::Class || kind == SymbolKind::Struct || kind == SymbolKind::Interface ||
           kind == SymbolKind::Enum || kind == SymbolKind::Namespace;
}

std::vector<WorkspaceSymbol> filterWorkspaceClasses(std::vector<WorkspaceSymbol> symbols) {
    std::erase_if(symbols, [](const WorkspaceSymbol& s) { return !isClassLikeSymbol(s.kind); });
    return symbols;
}

sched::Task<Result<std::vector<WorkspaceSymbol>>> requestWorkspaceClasses(ILspRequester& server, std::string query,
                                                                          std::stop_token token) {
    auto symbols = co_await requestWorkspaceSymbols(server, std::move(query), std::move(token));
    if (!symbols) {
        co_return std::unexpected(std::move(symbols.error()));
    }
    co_return filterWorkspaceClasses(std::move(*symbols));
}

std::vector<WorkspaceFile> rankWorkspaceFiles(std::span<const std::string> paths, std::string_view query) {
    const bool hasSeparator = query.find('/') != std::string_view::npos || query.find('\\') != std::string_view::npos;
    std::string normalizedQuery(query);
    if (hasSeparator) {
        std::ranges::replace(normalizedQuery, '\\', '/');
    }
    std::vector<WorkspaceFile> out;
    out.reserve(paths.size());
    for (const std::string& path : paths) {
        const std::size_t slash = path.find_last_of("/\\");
        std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
        if (query.empty()) {
            out.push_back(WorkspaceFile{.path = path, .fileName = std::move(base), .match = {}});
            continue;
        }
        std::string subject = base;
        if (hasSeparator) {
            subject = path;
            std::ranges::replace(subject, '\\', '/');
        }
        if (auto match = fuzzyMatch(normalizedQuery, subject)) {
            out.push_back(WorkspaceFile{.path = path, .fileName = std::move(base), .match = std::move(*match)});
        }
    }
    std::ranges::stable_sort(out, [emptyQuery = query.empty()](const WorkspaceFile& a, const WorkspaceFile& b) {
        if (emptyQuery) {
            return a.path < b.path;
        }
        if (a.match.kind != b.match.kind) {
            return a.match.kind < b.match.kind;
        }
        if (a.match.score != b.match.score) {
            return a.match.score > b.match.score;
        }
        if (a.fileName.size() != b.fileName.size()) {
            return a.fileName.size() < b.fileName.size();
        }
        return a.path < b.path;
    });
    return out;
}

} // namespace frost::lang
