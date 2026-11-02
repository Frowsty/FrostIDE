// SPDX-License-Identifier: Apache-2.0
//
// Shared LSP structures (lsp_types.hpp): locations, markup, symbol kinds and request parameters.

#include "frost/lang/lsp_types.hpp"

#include "frost/lang/uri.hpp"

#include <array>

namespace frost::lang {

std::optional<Location> parseLocation(const Json& json) {
    if (!json.isObject()) {
        return std::nullopt;
    }
    std::string_view uri = json.stringAt("uri");
    const Json* range = json.find("range");
    if (uri.empty()) { // LocationLink.
        uri = json.stringAt("targetUri");
        range = json.find("targetSelectionRange");
        if (range == nullptr) {
            range = json.find("targetRange");
        }
    }
    if (uri.empty() || range == nullptr) {
        return std::nullopt;
    }
    auto parsed = parseRange(*range);
    auto path = uriToPath(uri);
    if (!parsed || !path) {
        return std::nullopt;
    }
    return Location{.path = std::move(*path), .uri = std::string(uri), .range = *parsed};
}

std::vector<Location> parseLocations(const Json& json) {
    std::vector<Location> out;
    if (json.isObject()) {
        if (auto location = parseLocation(json)) {
            out.push_back(std::move(*location));
        }
        return out;
    }
    for (const Json& item : json.items()) {
        if (auto location = parseLocation(item)) {
            out.push_back(std::move(*location));
        }
    }
    return out;
}

bool locationContains(const Location& location, std::string_view path, LspPosition position) {
    return documentKey(location.path) == documentKey(path) && location.range.start <= position &&
           position <= location.range.end;
}

Markup parseMarkup(const Json& json) {
    if (const std::string* text = json.asString()) {
        return Markup{.text = *text, .markdown = true};
    }
    if (json.isObject()) {
        if (const Json* kind = json.find("kind"); kind != nullptr) {
            return Markup{.text = std::string(json.stringAt("value")), .markdown = json.stringAt("kind") == "markdown"};
        }
        if (json.find("language") != nullptr) { // MarkedString {language, value}.
            return Markup{.text = std::string("```") + std::string(json.stringAt("language")) + "\n" +
                                  std::string(json.stringAt("value")) + "\n```",
                          .markdown = true};
        }
        return {};
    }
    if (json.isArray()) {
        Markup out{.text = {}, .markdown = true};
        for (const Json& item : json.items()) {
            const Markup part = parseMarkup(item);
            if (part.text.empty()) {
                continue;
            }
            if (!out.text.empty()) {
                out.text += "\n\n";
            }
            out.text += part.text;
        }
        return out;
    }
    return {};
}

std::string_view toString(SymbolKind kind) noexcept {
    static constexpr std::array<std::string_view, 26> kNames = {
        "file",        "module",      "namespace", "package",   "class",         "method",   "property",
        "field",       "constructor", "enum",      "interface", "function",      "variable", "constant",
        "string",      "number",      "boolean",   "array",     "object",        "key",      "null",
        "enum member", "struct",      "event",     "operator",  "type parameter"};
    const auto index = static_cast<std::size_t>(kind);
    return index >= 1 && index <= kNames.size() ? kNames[index - 1] : std::string_view("symbol");
}

SymbolKind parseSymbolKind(const Json& json) noexcept {
    const std::int64_t value = json.asInteger().value_or(0);
    return value >= 1 && value <= 26 ? static_cast<SymbolKind>(value) : SymbolKind::Variable;
}

Result<Json> textDocumentIdentifier(std::string_view path) {
    auto uri = pathToUri(path);
    if (!uri) {
        return std::unexpected(std::move(uri.error()));
    }
    return Json::object().set("uri", std::move(*uri));
}

Result<Json> textDocumentPosition(std::string_view path, LspPosition position) {
    auto document = textDocumentIdentifier(path);
    if (!document) {
        return std::unexpected(std::move(document.error()));
    }
    return Json::object().set("textDocument", std::move(*document)).set("position", toJson(position));
}

std::uint32_t byteColumn(std::string_view line, std::uint32_t character, PositionEncoding encoding) noexcept {
    if (encoding == PositionEncoding::Utf8) {
        return static_cast<std::uint32_t>(std::min<std::size_t>(character, line.size()));
    }
    std::uint32_t units = 0;
    std::size_t i = 0;
    while (i < line.size() && units < character) {
        const auto lead = static_cast<unsigned char>(line[i]);
        std::size_t length = 1;
        if (lead >= 0xF0) {
            length = 4;
        } else if (lead >= 0xE0) {
            length = 3;
        } else if (lead >= 0xC0) {
            length = 2;
        }
        units += length == 4 ? 2 : 1; // Astral characters take a surrogate pair.
        i = std::min(line.size(), i + length);
    }
    return static_cast<std::uint32_t>(i);
}

} // namespace frost::lang
