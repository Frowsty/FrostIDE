// SPDX-License-Identifier: Apache-2.0
//
// LSP 3.17 structures shared by the navigation and completion models (W2, ADR 0041): locations
// (Location and LocationLink), markup (MarkupContent and the older MarkedString forms), symbol
// kinds, the request parameters most features send, and converting an LSP column to a byte
// column of a line. Written from the specification; all functions are pure and thread-safe.
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

// A place in a file: Location, or the target of a LocationLink.
struct Location {
    std::string path; // Absolute path (from the file URI).
    std::string uri;
    LspRange range; // LocationLink: targetSelectionRange (the name), else targetRange.
    friend bool operator==(const Location&, const Location&) = default;
};

// A Location or LocationLink; nullopt for anything else (or a non-file URI).
[[nodiscard]] std::optional<Location> parseLocation(const Json& json);
// null, one Location, or an array of Location or LocationLink; unusable entries are skipped.
[[nodiscard]] std::vector<Location> parseLocations(const Json& json);

// True when `location` names `path` (documentKey equality) and its range contains `position`
// (start inclusive, end inclusive, so a caret right after a name still counts).
[[nodiscard]] bool locationContains(const Location& location, std::string_view path, LspPosition position);

// Documentation text and whether it is Markdown.
struct Markup {
    std::string text;
    bool markdown = false;
    friend bool operator==(const Markup&, const Markup&) = default;
};

/*
 * MarkupContent ({kind, value}), a MarkedString (a Markdown string, or {language, value}, which
 * becomes a fenced code block) or an array of MarkedStrings (joined by blank lines, Markdown).
 * Anything else gives empty text.
 */
[[nodiscard]] Markup parseMarkup(const Json& json);

// LSP SymbolKind; the numbers are the protocol's.
enum class SymbolKind : std::uint8_t {
    File = 1,
    Module,
    Namespace,
    Package,
    Class,
    Method,
    Property,
    Field,
    Constructor,
    Enum,
    Interface,
    Function,
    Variable,
    Constant,
    String,
    Number,
    Boolean,
    Array,
    Object,
    Key,
    Null,
    EnumMember,
    Struct,
    Event,
    Operator,
    TypeParameter,
};

// "class", "method", "enum member", ...: lower-case English names for tooltips.
[[nodiscard]] std::string_view toString(SymbolKind kind) noexcept;
// A SymbolKind number; out-of-range values (and non-numbers) give Variable.
[[nodiscard]] SymbolKind parseSymbolKind(const Json& json) noexcept;

// {"uri": <uri of path>}. Errors: those of pathToUri (InvalidArgument for a relative path).
[[nodiscard]] Result<Json> textDocumentIdentifier(std::string_view path);
// TextDocumentPositionParams: {"textDocument": {...}, "position": {...}}. Errors: as above.
[[nodiscard]] Result<Json> textDocumentPosition(std::string_view path, LspPosition position);

// The byte column (within `line`, UTF-8 text without its break) of LSP column `character` in
// `encoding`; columns past the end clamp to line.size(). O(line length) for UTF-16.
[[nodiscard]] std::uint32_t byteColumn(std::string_view line, std::uint32_t character,
                                       PositionEncoding encoding) noexcept;

} // namespace frost::lang
