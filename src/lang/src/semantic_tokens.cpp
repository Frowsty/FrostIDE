// SPDX-License-Identifier: Apache-2.0
//
// Semantic tokens and inactive regions (semantic_tokens.hpp): the legend, the relative stream
// decoder, delta edits, the full/delta request and the inactive-regions notification.

#include "frost/lang/semantic_tokens.hpp"

#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

namespace frost::lang {
namespace {

constexpr std::array<std::string_view, kSemanticTokenTypeCount> kTypeNames{
    "namespace", "type",       "class",  "enum",     "interface", "struct",  "typeParameter", "parameter", "variable",
    "property",  "enumMember", "event",  "function", "method",    "macro",   "keyword",       "modifier",  "comment",
    "string",    "number",     "regexp", "operator", "decorator", "concept", "bracket",       "label",     "unknown",
};

constexpr std::array<std::string_view, kSemanticModifierCount> kModifierNames{
    "declaration",
    "definition",
    "readonly",
    "static",
    "deprecated",
    "abstract",
    "async",
    "modification",
    "documentation",
    "defaultLibrary",
    "deduced",
    "virtual",
    "dependentName",
    "usedAsMutableReference",
    "usedAsMutablePointer",
    "constructorOrDestructor",
    "userDefined",
    "functionScope",
    "classScope",
    "fileScope",
    "globalScope",
};

// The non-negative integers of a JSON array that fit in 32 bits; nullopt for anything else.
[[nodiscard]] std::optional<std::vector<std::uint32_t>> integers(const Json* list) {
    if (list == nullptr || !list->isArray()) {
        return std::nullopt;
    }
    std::vector<std::uint32_t> out;
    out.reserve(list->size());
    for (const Json& item : list->items()) {
        const std::optional<std::int64_t> value = item.asInteger();
        if (!value || *value < 0 || *value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint32_t>(*value));
    }
    return out;
}

[[nodiscard]] Json nameList(std::span<const std::string_view> names) {
    Json out = Json::array();
    for (const std::string_view name : names) {
        out.push(Json(name));
    }
    return out;
}

} // namespace

std::string_view toString(SemanticTokenType type) noexcept {
    const auto index = static_cast<std::size_t>(type);
    return index < kTypeNames.size() ? kTypeNames[index] : std::string_view("unknown");
}

SemanticTokenType parseSemanticTokenType(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kTypeNames.size(); ++i) {
        if (kTypeNames[i] == name) {
            return static_cast<SemanticTokenType>(i);
        }
    }
    return SemanticTokenType::Unknown;
}

std::string_view semanticModifierName(std::size_t index) noexcept {
    return index < kModifierNames.size() ? kModifierNames[index] : std::string_view();
}

std::uint32_t parseSemanticModifier(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kModifierNames.size(); ++i) {
        if (kModifierNames[i] == name) {
            return 1U << i;
        }
    }
    return 0;
}

std::optional<SemanticTokensLegend> parseSemanticTokensLegend(const Json& legend) {
    const Json* types = legend.find("tokenTypes");
    const Json* modifiers = legend.find("tokenModifiers");
    if (types == nullptr || !types->isArray() || modifiers == nullptr || !modifiers->isArray()) {
        return std::nullopt;
    }
    SemanticTokensLegend out;
    for (const Json& type : types->items()) {
        const std::string* name = type.asString();
        out.types.push_back(name != nullptr ? parseSemanticTokenType(*name) : SemanticTokenType::Unknown);
    }
    for (const Json& modifier : modifiers->items()) {
        const std::string* name = modifier.asString();
        out.modifierBits.push_back(name != nullptr ? parseSemanticModifier(*name) : 0U);
    }
    return out;
}

std::optional<SemanticTokensSupport> semanticTokensSupport(const ServerCapabilities& capabilities) {
    const Json* provider = capabilities.raw.find("semanticTokensProvider");
    if (provider == nullptr || !provider->isObject()) {
        return std::nullopt;
    }
    const Json* legend = provider->find("legend");
    if (legend == nullptr) {
        return std::nullopt;
    }
    std::optional<SemanticTokensLegend> parsed = parseSemanticTokensLegend(*legend);
    if (!parsed) {
        return std::nullopt;
    }
    SemanticTokensSupport out;
    out.legend = std::move(*parsed);
    if (const Json* full = provider->find("full"); full != nullptr) {
        if (const std::optional<bool> flag = full->asBool()) {
            out.full = *flag;
        } else if (full->isObject()) {
            out.full = true;
            out.delta = full->boolAt("delta").value_or(false);
        }
    }
    return out;
}

Result<std::vector<SemanticToken>> decodeSemanticTokens(std::span<const std::uint32_t> data,
                                                        const SemanticTokensLegend& legend,
                                                        const text::Snapshot& snapshot, PositionEncoding encoding) {
    if (data.size() % 5 != 0) {
        return fail(ErrorCode::Corrupt,
                    std::format("semantic tokens: {} integers is not a multiple of five", data.size()));
    }
    std::vector<SemanticToken> out;
    out.reserve(data.size() / 5);
    std::uint64_t line = 0;
    std::uint64_t start = 0;
    const std::uint64_t lines = snapshot.lineCount();
    for (std::size_t i = 0; i < data.size(); i += 5) {
        const std::uint32_t deltaLine = data[i];
        const std::uint32_t deltaStart = data[i + 1];
        const std::uint32_t length = data[i + 2];
        const std::uint32_t type = data[i + 3];
        const std::uint32_t bits = data[i + 4];
        line += deltaLine;
        start = deltaLine != 0 ? deltaStart : start + deltaStart;
        if (line >= lines || start > std::numeric_limits<std::uint32_t>::max()) {
            continue; // Past the text the server saw (a stale or malformed stream).
        }
        const auto row = static_cast<std::uint32_t>(line);
        const auto column = static_cast<std::uint32_t>(start);
        const std::uint64_t endColumn =
            std::min<std::uint64_t>(start + length, std::numeric_limits<std::uint32_t>::max());
        const Result<ByteOffset> begin = fromLspPosition(snapshot, LspPosition{row, column}, encoding);
        const Result<ByteOffset> end =
            fromLspPosition(snapshot, LspPosition{row, static_cast<std::uint32_t>(endColumn)}, encoding);
        if (!begin || !end || *end <= *begin) {
            continue;
        }
        SemanticToken token;
        token.range = text::ByteRange{*begin, *end};
        token.type = type < legend.types.size() ? legend.types[type] : SemanticTokenType::Unknown;
        for (std::size_t bit = 0; bit < legend.modifierBits.size() && bit < 32; ++bit) {
            if ((bits & (1U << bit)) != 0) {
                token.modifiers |= legend.modifierBits[bit];
            }
        }
        out.push_back(token);
    }
    return out;
}

Result<std::vector<std::uint32_t>> applySemanticTokensEdits(std::span<const std::uint32_t> previous,
                                                            std::span<const SemanticTokensEdit> edits) {
    std::vector<const SemanticTokensEdit*> order;
    order.reserve(edits.size());
    for (const SemanticTokensEdit& edit : edits) {
        if (static_cast<std::uint64_t>(edit.start) + edit.deleteCount > previous.size()) {
            return fail(ErrorCode::OutOfRange, std::format("semantic tokens delta: edit {}+{} past {} integers",
                                                           edit.start, edit.deleteCount, previous.size()));
        }
        order.push_back(&edit);
    }
    std::ranges::sort(order, {}, [](const SemanticTokensEdit* edit) { return edit->start; });
    for (std::size_t i = 1; i < order.size(); ++i) {
        if (static_cast<std::uint64_t>(order[i - 1]->start) + order[i - 1]->deleteCount > order[i]->start) {
            return fail(ErrorCode::InvalidArgument, "semantic tokens delta: overlapping edits");
        }
    }
    std::vector<std::uint32_t> out(previous.begin(), previous.end());
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const SemanticTokensEdit& edit = **it;
        const auto first = out.begin() + edit.start;
        out.erase(first, first + edit.deleteCount);
        out.insert(out.begin() + edit.start, edit.data.begin(), edit.data.end());
    }
    return out;
}

Result<SemanticTokensResult> parseSemanticTokensResult(const Json& result) {
    if (!result.isObject()) {
        return fail(ErrorCode::InvalidArgument, "semantic tokens: the result is not an object");
    }
    SemanticTokensResult out;
    out.resultId = std::string(result.stringAt("resultId"));
    if (const Json* data = result.find("data"); data != nullptr) {
        out.data = integers(data);
        if (!out.data) {
            return fail(ErrorCode::InvalidArgument, "semantic tokens: data is not an integer array");
        }
        return out;
    }
    const Json* edits = result.find("edits");
    if (edits == nullptr || !edits->isArray()) {
        return fail(ErrorCode::InvalidArgument, "semantic tokens: neither data nor edits");
    }
    std::vector<SemanticTokensEdit> parsed;
    for (const Json& edit : edits->items()) {
        const std::optional<std::int64_t> start = edit.integerAt("start");
        const std::optional<std::int64_t> deleteCount = edit.integerAt("deleteCount");
        if (!start || !deleteCount || *start < 0 || *deleteCount < 0 ||
            *start > std::numeric_limits<std::uint32_t>::max() ||
            *deleteCount > std::numeric_limits<std::uint32_t>::max()) {
            return fail(ErrorCode::InvalidArgument, "semantic tokens: a malformed edit");
        }
        SemanticTokensEdit item;
        item.start = static_cast<std::uint32_t>(*start);
        item.deleteCount = static_cast<std::uint32_t>(*deleteCount);
        if (const Json* data = edit.find("data"); data != nullptr && !data->isNull()) {
            std::optional<std::vector<std::uint32_t>> values = integers(data);
            if (!values) {
                return fail(ErrorCode::InvalidArgument, "semantic tokens: an edit's data is not an integer array");
            }
            item.data = std::move(*values);
        }
        parsed.push_back(std::move(item));
    }
    out.edits = std::move(parsed);
    return out;
}

sched::Task<Result<SemanticTokensData>> requestSemanticTokens(ILspRequester& server, std::string path,
                                                              std::optional<SemanticTokensData> previous, bool useDelta,
                                                              std::stop_token token) {
    auto document = textDocumentIdentifier(path);
    if (!document) {
        co_return std::unexpected(std::move(document.error()));
    }
    if (useDelta && previous && !previous->resultId.empty()) {
        Json params = Json::object().set("textDocument", *document).set("previousResultId", previous->resultId);
        auto answered = co_await server.request("textDocument/semanticTokens/full/delta", std::move(params), token);
        if (!answered) {
            co_return std::unexpected(std::move(answered.error()));
        }
        if (!answered->isNull()) {
            auto parsed = parseSemanticTokensResult(*answered);
            if (!parsed) {
                co_return std::unexpected(std::move(parsed.error()));
            }
            if (parsed->data) {
                co_return SemanticTokensData{std::move(parsed->resultId), std::move(*parsed->data)};
            }
            auto applied = applySemanticTokensEdits(previous->data, *parsed->edits);
            if (applied) {
                co_return SemanticTokensData{std::move(parsed->resultId), std::move(*applied)};
            }
            // The edits do not fit what we hold: fall through to one full request.
        }
    }
    auto answered = co_await server.request("textDocument/semanticTokens/full",
                                            Json::object().set("textDocument", *document), token);
    if (!answered) {
        co_return std::unexpected(std::move(answered.error()));
    }
    if (answered->isNull()) {
        co_return SemanticTokensData{};
    }
    auto parsed = parseSemanticTokensResult(*answered);
    if (!parsed) {
        co_return std::unexpected(std::move(parsed.error()));
    }
    if (!parsed->data) {
        co_return fail(ErrorCode::InvalidArgument, "semantic tokens: a full request answered with edits");
    }
    co_return SemanticTokensData{std::move(parsed->resultId), std::move(*parsed->data)};
}

Json semanticTokensClientCapabilities() {
    return Json::object()
        .set("dynamicRegistration", false)
        .set("requests", Json::object().set("range", false).set("full", Json::object().set("delta", true)))
        .set("tokenTypes", nameList(std::span<const std::string_view>(kTypeNames.data(), kTypeNames.size() - 1)))
        .set("tokenModifiers", nameList(kModifierNames))
        .set("formats", Json::array({Json("relative")}))
        .set("overlappingTokenSupport", false)
        .set("multilineTokenSupport", false)
        .set("serverCancelSupport", true)
        .set("augmentsSyntaxTokens", true);
}

Json inactiveRegionsClientCapabilities() { return Json::object().set("inactiveRegions", true); }

Result<InactiveRegions> decodeInactiveRegions(const Json& params) {
    const Json* document = params.find("textDocument");
    if (document == nullptr || !document->isObject() || document->stringAt("uri").empty()) {
        return fail(ErrorCode::InvalidArgument, "inactiveRegions: no textDocument.uri");
    }
    InactiveRegions out;
    out.uri = std::string(document->stringAt("uri"));
    auto path = uriToPath(out.uri);
    if (!path) {
        return std::unexpected(std::move(path.error().withContext("inactiveRegions")));
    }
    out.file = std::move(*path);
    const Json* regions = params.find("regions");
    if (regions == nullptr || !regions->isArray()) {
        return fail(ErrorCode::InvalidArgument, "inactiveRegions: no regions array");
    }
    for (const Json& region : regions->items()) {
        if (const std::optional<LspRange> range = parseRange(region)) {
            out.lspRanges.push_back(*range);
        }
    }
    return out;
}

void mapInactiveRegions(InactiveRegions& regions, const text::Snapshot& snapshot, PositionEncoding encoding) {
    regions.ranges.clear();
    for (const LspRange& range : regions.lspRanges) {
        const Result<ByteOffset> begin = fromLspPosition(snapshot, range.start, encoding);
        if (!begin) {
            continue;
        }
        Result<ByteOffset> end = fromLspPosition(snapshot, range.end, encoding);
        if (!end) {
            end = snapshot.size(); // An end past the last line ends at the text's end.
        }
        if (*end > *begin) {
            regions.ranges.push_back(text::ByteRange{*begin, *end});
        }
    }
    std::ranges::sort(regions.ranges, {}, &text::ByteRange::begin);
    regions.textVersion = snapshot.version();
}

} // namespace frost::lang
