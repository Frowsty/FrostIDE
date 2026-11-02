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
    auto definitions = co_await requestLocations(server, NavigationTarget::Definition, path, position, token);
    if (!definitions) {
        co_return std::unexpected(std::move(definitions.error()));
    }
    if (!definitions->empty() && !anyContains(*definitions, path, position)) {
        co_return GotoResult{.locations = std::move(*definitions), .showUsages = false};
    }
    auto declarations = co_await requestLocations(server, NavigationTarget::Declaration, path, position, token);
    if (!declarations) {
        co_return std::unexpected(std::move(declarations.error()));
    }
    if (!declarations->empty() && !anyContains(*declarations, path, position)) {
        co_return GotoResult{.locations = std::move(*declarations), .showUsages = false};
    }
    // On the symbol's own declaration (or nothing found while the server knows the symbol).
    const bool onSymbol = !definitions->empty() || !declarations->empty();
    co_return GotoResult{.locations = {}, .showUsages = onSymbol};
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

} // namespace frost::lang
