// SPDX-License-Identifier: Apache-2.0
//
// vocabulary.cpp: frost::syntax's capture vocabulary (ADR 0023, D8): HighlightKind capture names,
// OutlineKind capture suffixes and their LSP 3.17 SymbolKind values.

#include "frost/syntax/highlight.hpp"
#include "frost/syntax/outline.hpp"

#include <array>
#include <cstddef>

namespace frost::syntax {

namespace {

// Indexed by HighlightKind.
constexpr std::array<std::string_view, kHighlightKindCount> kHighlightNames = {
    "comment",
    "comment.doc",
    "keyword",
    "keyword.directive",
    "operator",
    "punctuation",
    "punctuation.bracket",
    "string",
    "string.escape",
    "character",
    "number",
    "boolean",
    "constant",
    "type",
    "type.builtin",
    "namespace",
    "function",
    "function.call",
    "macro",
    "variable",
    "parameter",
    "property",
    "attribute",
    "label",
    "tag",
    "tag.attribute",
    "markup.heading",
    "markup.link",
    "markup.raw",
    "markup.list",
    "markup.quote",
};

struct OutlineInfo {
    std::string_view suffix;
    std::uint8_t lspKind; // LSP 3.17 SymbolKind.
};

// Indexed by OutlineKind. SymbolKind: File 1, Module 2, Namespace 3, Package 4, Class 5, Method 6,
// Property 7, Field 8, Constructor 9, Enum 10, Interface 11, Function 12, Variable 13, Constant 14,
// String 15, Number 16, Boolean 17, Array 18, Object 19, Key 20, Null 21, EnumMember 22, Struct 23,
// Event 24, Operator 25, TypeParameter 26.
constexpr std::array<OutlineInfo, kOutlineKindCount> kOutlineInfo = {{
    {"namespace", 3},    {"class", 5},     {"struct", 23},   {"union", 23},      {"interface", 11}, {"enum", 10},
    {"enum_member", 22}, {"function", 12}, {"method", 6},    {"constructor", 9}, {"destructor", 6}, {"field", 8},
    {"property", 7},     {"event", 24},    {"variable", 13}, {"constant", 14},   {"macro", 14},     {"type_alias", 26},
    {"command", 12},     {"target", 2},    {"key", 20},      {"element", 19},    {"heading", 15},
}};

} // namespace

std::string_view captureName(HighlightKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kHighlightNames.size() ? kHighlightNames[index] : std::string_view{};
}

std::optional<HighlightKind> highlightKindFromCapture(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kHighlightNames.size(); ++i) {
        if (kHighlightNames[i] == name) {
            return static_cast<HighlightKind>(i);
        }
    }
    return std::nullopt;
}

std::uint8_t toLspSymbolKind(OutlineKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kOutlineInfo.size() ? kOutlineInfo[index].lspKind : std::uint8_t{13};
}

std::string_view outlineCaptureSuffix(OutlineKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kOutlineInfo.size() ? kOutlineInfo[index].suffix : std::string_view{};
}

} // namespace frost::syntax
