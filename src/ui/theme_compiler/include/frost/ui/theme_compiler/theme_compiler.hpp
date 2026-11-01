// SPDX-License-Identifier: Apache-2.0
//
// The .theme.json compiler: turns a FrostIDE theme file into ThemeData and a v1 theme blob at
// build time (frost-themec), with strict validation and sorted, path-addressed diagnostics
// (ADR 0015 decisions 2 and 3). Uses QJsonDocument (Qt Core) plus a Qt-free pre-scan for
// duplicate member names, which QJsonDocument silently collapses.
//
/*
 * The accepted format (a subset of the IntelliJ Platform theme format, as documented on the
 * public "Themes: Customize UI Theme Properties" SDK page cited in ADR 0015, plus FrostIDE
 * extensions):
 *
 *   {
 *     "name": "Glacier Dark",            required string, 1..64 bytes of UTF-8
 *     "dark": true,                      required bool
 *     "highContrast": false,             optional bool (FrostIDE extension), default false
 *     "author": "...", "$schema": "...", ignored
 *     "colors": {"accent": "#5A9BFF"},   named colours: literal "#RRGGBB" or "#RRGGBBAA" only
 *     "ui": {
 *       "*": {"arc": 4},                 wildcard: matches every key whose last segment is "arc"
 *       "Button": {"default.background": "accent", "arc": 6},   nested form
 *       "Island.background": "#23252A"   dotted form
 *     },
 *     "compact": {"Island.arc": 8},      compact-mode values, only for keys with hasCompact
 *     "icons": {"ColorPalette": {"Icon.Default": "#B4B8BF", ...}}   every icon role, required
 *   }
 *
 * Every registry key K resolves to the first of: an explicit value (nested or dotted; two
 * spellings with different values are an error, equal ones a warning); the "*" wildcard
 * entry for K's last segment; K's registry fallback, resolved by the same rules; otherwise the
 * error "unresolved key K". The compiler walks the registry, never the JSON member order, so
 * the output is byte-identical for any member order.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/ui/model/theme.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui {

// Input limits: larger or deeper inputs are refused before any JSON parsing.
inline constexpr std::size_t kMaxThemeJsonBytes = 1024 * 1024;
inline constexpr int kMaxThemeJsonDepth = 8; // The top-level object is depth 1.

/*
 * Diagnostics stay small whatever the input's shape: at most kMaxReportedDuplicates duplicate
 * member names are listed (one more "$" diagnostic counts the rest), a jsonPath longer than
 * kMaxDiagnosticPathBytes keeps its first and last 128 bytes around "...", and a message is
 * elided the same way beyond kMaxDiagnosticMessageBytes.
 */
inline constexpr std::size_t kMaxReportedDuplicates = 32;
inline constexpr std::size_t kMaxDiagnosticPathBytes = 256;
inline constexpr std::size_t kMaxDiagnosticMessageBytes = 1024;

struct Diagnostic {
    enum Severity : std::uint8_t { Warning, Error };
    Severity severity = Error;
    std::string jsonPath; // "ui.Button.arc"; "$" for the document itself.
    std::string message;

    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

struct CompileOptions {
    // Strict (the build): unknown keys and duplicate member names are errors. Lenient (M11's
    // importer): they are warnings, and so are ColorPalette keys that are not icon roles.
    bool strict = true;
};

struct CompileResult {
    std::optional<ThemeData> data;       // Present only when there is no Error diagnostic.
    std::vector<Diagnostic> diagnostics; // Sorted by jsonPath, then message.

    [[nodiscard]] bool ok() const noexcept { return data.has_value(); }
};

/*
 * Compiles `json` (the bytes of a .theme.json file) for theme `themeId` (the file name stem,
 * which must pass isValidThemeId). Error diagnostics, which stop at the first failing stage, in
 * this order: an invalid theme id, input over kMaxThemeJsonBytes, a UTF-8 BOM, invalid UTF-8,
 * nesting deeper than kMaxThemeJsonDepth, malformed JSON, a top level that is not an object.
 * Duplicate member names found by the pre-scan (errors when strict) are kept even when the
 * malformed-JSON stage then fails. Then,
 * collected together: missing or mistyped top-level fields, unknown fields and keys (strict),
 * wildcards on non-leaves, unknown named colours and names that reference names, translucent
 * values for opaque keys, metrics outside their range, compact values for keys without
 * hasCompact or above the normal value (Px keys), missing icon roles, duplicate spellings with
 * different values, unresolved keys, and contrast pairs whose background resolves translucent
 * without an underlay. A "ui" member whose dotted key is already longer than every registry
 * key is reported once as unknown, without visiting what is under it.
 * Pure; thread-safe for distinct inputs (Qt's JSON classes are reentrant). O(n log n) time and
 * O(n) memory in the input size n, for any input shape (no per-member copy of a parent path).
 */
[[nodiscard]] CompileResult compileTheme(std::string_view themeId, std::span<const std::byte> json,
                                         const CompileOptions& options = {});

/*
 * compileTheme, then encodeThemeBlob. Errors: InvalidArgument whose message lists the first
 * three Error diagnostics ("<path>: <message>; ...").
 */
[[nodiscard]] Result<std::vector<std::byte>>
compileThemeToBlob(std::string_view themeId, std::span<const std::byte> json, const CompileOptions& options = {});

// One line per diagnostic: "<path>: <warning|error>: <message>".
[[nodiscard]] std::string formatDiagnostic(const Diagnostic& diagnostic);

} // namespace frost::ui
