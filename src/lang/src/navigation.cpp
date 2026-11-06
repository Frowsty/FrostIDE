// SPDX-License-Identifier: Apache-2.0
//
// Navigation requests, usage grouping and document highlights (navigation.hpp).

#include "frost/lang/navigation.hpp"

#include "frost/io/file_system.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"

#include <algorithm>

namespace frost::lang {
namespace {

[[nodiscard]] bool anyContains(const std::vector<Location>& locations, std::string_view path, LspPosition position) {
    return std::ranges::any_of(locations,
                               [&](const Location& location) { return locationContains(location, path, position); });
}

} // namespace

std::string_view methodFor(NavigationTarget target) noexcept {
    switch (target) {
    case NavigationTarget::Definition: return "textDocument/definition";
    case NavigationTarget::Declaration: return "textDocument/declaration";
    case NavigationTarget::Implementation: return "textDocument/implementation";
    case NavigationTarget::TypeDefinition: return "textDocument/typeDefinition";
    }
    return "textDocument/definition";
}

sched::Task<Result<std::vector<Location>>> requestLocations(ILspRequester& server, NavigationTarget target,
                                                            std::string path, LspPosition position,
                                                            std::stop_token token) {
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    auto result = co_await server.request(std::string(methodFor(target)), std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseLocations(*result);
}

sched::Task<Result<GotoResult>> gotoDeclarationOrUsages(ILspRequester& server, std::string path, LspPosition position,
                                                        std::stop_token token) {
    auto declarations = co_await requestLocations(server, NavigationTarget::Declaration, path, position, token);
    if (!declarations && declarations.error().code() == ErrorCode::Cancelled) {
        co_return std::unexpected(std::move(declarations.error()));
    }
    if (declarations && !declarations->empty()) {
        if (!anyContains(*declarations, path, position)) {
            co_return GotoResult{.locations = std::move(*declarations), .showUsages = false};
        }
        // Caret is already on the symbol's declaration: run find usages.
        co_return GotoResult{.locations = {}, .showUsages = true};
    }
    auto definitions = co_await requestLocations(server, NavigationTarget::Definition, path, position, token);
    if (!definitions) {
        co_return std::unexpected(std::move(definitions.error()));
    }
    if (!definitions->empty() && !anyContains(*definitions, path, position)) {
        co_return GotoResult{.locations = std::move(*definitions), .showUsages = false};
    }
    // On the symbol's own declaration/definition (or nothing found while the server knows the symbol).
    const bool onSymbol = !definitions->empty();
    co_return GotoResult{.locations = {}, .showUsages = onSymbol};
}

sched::Task<Result<std::vector<Location>>> gotoImplementation(ILspRequester& server, std::string path,
                                                              LspPosition position, std::stop_token token) {
    auto implementations = co_await requestLocations(server, NavigationTarget::Implementation, path, position, token);
    if (!implementations && implementations.error().code() == ErrorCode::Cancelled) {
        co_return std::unexpected(std::move(implementations.error()));
    }
    if (implementations) {
        std::erase_if(*implementations,
                      [&](const Location& location) { return locationContains(location, path, position); });
        if (!implementations->empty()) {
            co_return std::move(*implementations);
        }
    }
    auto definitions = co_await requestLocations(server, NavigationTarget::Definition, path, position, token);
    if (!definitions) {
        if (!implementations) {
            co_return std::unexpected(std::move(implementations.error()));
        }
        co_return std::unexpected(std::move(definitions.error()));
    }
    std::erase_if(*definitions, [&](const Location& location) { return locationContains(location, path, position); });
    co_return std::move(*definitions);
}

sched::Task<Result<std::optional<std::string>>> requestSwitchSourceHeader(ILspRequester& server, std::string path,
                                                                          std::stop_token token) {
    auto params = textDocumentIdentifier(path);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    auto result = co_await server.request("textDocument/switchSourceHeader", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    const std::string* uri = result->asString();
    if (uri == nullptr || uri->empty()) {
        co_return std::optional<std::string>();
    }
    auto counterpart = uriToPath(*uri);
    if (!counterpart) {
        co_return std::unexpected(std::move(counterpart.error()));
    }
    co_return std::optional<std::string>(std::move(*counterpart));
}

sched::Task<Result<std::vector<Location>>> requestReferences(ILspRequester& server, std::string path,
                                                             LspPosition position, bool includeDeclaration,
                                                             std::stop_token token) {
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    params->set("context", Json::object().set("includeDeclaration", includeDeclaration));
    auto result = co_await server.request("textDocument/references", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseLocations(*result);
}

std::optional<std::string> FileLineSource::line(std::string_view path, std::uint32_t index) {
    const std::string key = documentKey(path);
    auto it = m_files.find(key);
    if (it == m_files.end()) {
        std::optional<text::Snapshot> snapshot = m_openDocument ? m_openDocument(path) : std::nullopt;
        if (!snapshot) {
            if (auto content = m_fs.readFile(path)) {
                if (auto buffer = text::TextBuffer::fromUtf8(std::move(*content))) {
                    snapshot = buffer->snapshot();
                }
            }
        }
        it = m_files.emplace(key, std::move(snapshot)).first;
    }
    if (!it->second) {
        return std::nullopt;
    }
    auto info = it->second->lineInfo(LineIndex{index});
    if (!info) {
        return std::nullopt;
    }
    return it->second->text(info->content);
}

std::vector<UsageGroup> groupUsages(std::span<const Location> locations, const LineSource& lines,
                                    PositionEncoding encoding) {
    std::vector<Location> sorted(locations.begin(), locations.end());
    std::ranges::stable_sort(sorted, [](const Location& a, const Location& b) {
        const std::string ka = documentKey(a.path);
        const std::string kb = documentKey(b.path);
        if (ka != kb) {
            return ka < kb;
        }
        return a.range.start < b.range.start;
    });
    std::vector<UsageGroup> groups;
    for (Location& location : sorted) {
        if (groups.empty() || documentKey(groups.back().path) != documentKey(location.path)) {
            groups.push_back(UsageGroup{.path = location.path, .usages = {}});
        }
        Usage usage;
        usage.line = location.range.start.line + 1;
        std::string text = lines ? lines(location.path, location.range.start.line).value_or(std::string()) : "";
        const std::uint32_t begin = byteColumn(text, location.range.start.character, encoding);
        std::uint32_t end = begin;
        if (location.range.end.line == location.range.start.line) {
            end = std::max(begin, byteColumn(text, location.range.end.character, encoding));
        } else {
            end = static_cast<std::uint32_t>(text.size());
        }
        usage.column = begin + 1;
        const std::size_t indent = std::min(text.find_first_not_of(" \t"), text.size());
        const auto cut = static_cast<std::uint32_t>(std::min<std::size_t>(indent, begin));
        usage.lineText = text.substr(cut);
        usage.highlightBegin = begin - cut;
        usage.highlightEnd = end - cut;
        usage.location = std::move(location);
        groups.back().usages.push_back(std::move(usage));
    }
    return groups;
}

std::vector<DocumentHighlight> parseDocumentHighlights(const Json& json) {
    std::vector<DocumentHighlight> out;
    for (const Json& item : json.items()) {
        const Json* range = item.find("range");
        if (range == nullptr) {
            continue;
        }
        auto parsed = parseRange(*range);
        if (!parsed) {
            continue;
        }
        const std::int64_t kind = item.integerAt("kind").value_or(1);
        out.push_back(DocumentHighlight{
            .range = *parsed, .kind = kind >= 1 && kind <= 3 ? static_cast<HighlightKind>(kind) : HighlightKind::Text});
    }
    std::ranges::sort(
        out, [](const DocumentHighlight& a, const DocumentHighlight& b) { return a.range.start < b.range.start; });
    return out;
}

sched::Task<Result<std::vector<DocumentHighlight>>>
requestDocumentHighlights(ILspRequester& server, std::string path, LspPosition position, std::stop_token token) {
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    auto result = co_await server.request("textDocument/documentHighlight", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseDocumentHighlights(*result);
}

namespace {

[[nodiscard]] std::vector<LspRange> parseRangeArray(const Json* array) {
    std::vector<LspRange> out;
    if (array == nullptr || !array->isArray()) {
        return out;
    }
    for (const Json& elem : array->items()) {
        if (auto parsed = parseRange(elem)) {
            out.push_back(*parsed);
        }
    }
    return out;
}

template<typename Item>
[[nodiscard]] Json hierarchyItemToJson(const Item& item) {
    Json out = Json::object()
                   .set("name", item.name)
                   .set("kind", static_cast<int>(item.kind))
                   .set("uri", item.uri)
                   .set("range", toJson(item.range))
                   .set("selectionRange", toJson(item.selectionRange));
    if (!item.detail.empty()) {
        out.set("detail", item.detail);
    }
    if (!item.data.isNull()) {
        out.set("data", item.data);
    }
    return out;
}

template<typename Item>
[[nodiscard]] std::optional<Item> parseHierarchyItemImpl(const Json& json) {
    if (!json.isObject()) {
        return std::nullopt;
    }
    const std::string_view name = json.stringAt("name");
    const std::string_view uri = json.stringAt("uri");
    const Json* range = json.find("range");
    const Json* selectionRange = json.find("selectionRange");
    if (name.empty() || uri.empty() || range == nullptr) {
        return std::nullopt;
    }
    auto parsedPath = uriToPath(uri);
    auto parsedRange = parseRange(*range);
    if (!parsedPath || !parsedRange) {
        return std::nullopt;
    }
    LspRange parsedSelection = *parsedRange;
    if (selectionRange != nullptr) {
        if (auto sel = parseRange(*selectionRange)) {
            parsedSelection = *sel;
        }
    }
    Item out;
    out.name = std::string(name);
    if (const Json* kind = json.find("kind")) {
        out.kind = parseSymbolKind(*kind);
    }
    out.detail = std::string(json.stringAt("detail"));
    out.path = std::move(*parsedPath);
    out.uri = std::string(uri);
    out.range = *parsedRange;
    out.selectionRange = parsedSelection;
    if (const Json* data = json.find("data")) {
        out.data = *data;
    }
    return out;
}

} // namespace

Json toJson(const CallHierarchyItem& item) { return hierarchyItemToJson(item); }

Json toJson(const TypeHierarchyItem& item) { return hierarchyItemToJson(item); }

std::optional<CallHierarchyItem> parseCallHierarchyItem(const Json& json) {
    return parseHierarchyItemImpl<CallHierarchyItem>(json);
}

std::vector<CallHierarchyItem> parseCallHierarchyItems(const Json& json) {
    std::vector<CallHierarchyItem> out;
    if (json.isArray()) {
        for (const Json& elem : json.items()) {
            if (auto item = parseCallHierarchyItem(elem)) {
                out.push_back(std::move(*item));
            }
        }
    } else if (auto single = parseCallHierarchyItem(json)) {
        out.push_back(std::move(*single));
    }
    return out;
}

std::vector<CallHierarchyIncomingCall> parseIncomingCalls(const Json& json) {
    std::vector<CallHierarchyIncomingCall> out;
    for (const Json& elem : json.items()) {
        if (!elem.isObject()) {
            continue;
        }
        const Json* from = elem.find("from");
        if (from == nullptr) {
            continue;
        }
        if (auto item = parseCallHierarchyItem(*from)) {
            out.push_back(CallHierarchyIncomingCall{.from = std::move(*item),
                                                    .fromRanges = parseRangeArray(elem.find("fromRanges"))});
        }
    }
    return out;
}

std::vector<CallHierarchyOutgoingCall> parseOutgoingCalls(const Json& json) {
    std::vector<CallHierarchyOutgoingCall> out;
    for (const Json& elem : json.items()) {
        if (!elem.isObject()) {
            continue;
        }
        const Json* to = elem.find("to");
        if (to == nullptr) {
            continue;
        }
        if (auto item = parseCallHierarchyItem(*to)) {
            out.push_back(CallHierarchyOutgoingCall{.to = std::move(*item),
                                                    .fromRanges = parseRangeArray(elem.find("fromRanges"))});
        }
    }
    return out;
}

std::optional<TypeHierarchyItem> parseTypeHierarchyItem(const Json& json) {
    return parseHierarchyItemImpl<TypeHierarchyItem>(json);
}

std::vector<TypeHierarchyItem> parseTypeHierarchyItems(const Json& json) {
    std::vector<TypeHierarchyItem> out;
    if (json.isArray()) {
        for (const Json& elem : json.items()) {
            if (auto item = parseTypeHierarchyItem(elem)) {
                out.push_back(std::move(*item));
            }
        }
    } else if (auto single = parseTypeHierarchyItem(json)) {
        out.push_back(std::move(*single));
    }
    return out;
}

sched::Task<Result<std::vector<CallHierarchyItem>>>
requestPrepareCallHierarchy(ILspRequester& server, std::string path, LspPosition position, std::stop_token token) {
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    auto result = co_await server.request("textDocument/prepareCallHierarchy", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseCallHierarchyItems(*result);
}

sched::Task<Result<std::vector<CallHierarchyIncomingCall>>>
requestIncomingCalls(ILspRequester& server, CallHierarchyItem item, std::stop_token token) {
    Json params = Json::object().set("item", toJson(item));
    auto result = co_await server.request("callHierarchy/incomingCalls", std::move(params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseIncomingCalls(*result);
}

sched::Task<Result<std::vector<CallHierarchyOutgoingCall>>>
requestOutgoingCalls(ILspRequester& server, CallHierarchyItem item, std::stop_token token) {
    Json params = Json::object().set("item", toJson(item));
    auto result = co_await server.request("callHierarchy/outgoingCalls", std::move(params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseOutgoingCalls(*result);
}

sched::Task<Result<std::vector<TypeHierarchyItem>>>
requestPrepareTypeHierarchy(ILspRequester& server, std::string path, LspPosition position, std::stop_token token) {
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    auto result = co_await server.request("textDocument/prepareTypeHierarchy", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseTypeHierarchyItems(*result);
}

sched::Task<Result<std::vector<TypeHierarchyItem>>> requestSupertypes(ILspRequester& server, TypeHierarchyItem item,
                                                                      std::stop_token token) {
    Json params = Json::object().set("item", toJson(item));
    auto result = co_await server.request("typeHierarchy/supertypes", std::move(params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseTypeHierarchyItems(*result);
}

sched::Task<Result<std::vector<TypeHierarchyItem>>> requestSubtypes(ILspRequester& server, TypeHierarchyItem item,
                                                                    std::stop_token token) {
    Json params = Json::object().set("item", toJson(item));
    auto result = co_await server.request("typeHierarchy/subtypes", std::move(params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseTypeHierarchyItems(*result);
}

} // namespace frost::lang
