// SPDX-License-Identifier: Apache-2.0
//
// Checking a scope file against the registry (ADR 0021): the diagnostics the store keeps for each
// loaded file and that validation on save reports, and their notification text. Pure functions.
/*
 * What is checked, per file:
 *   Error    invalid JSON or not an object; a key not allowed in this scope; a value that fails its
 *            schema (that value is ignored and the next lower scope's value applies).
 *   Warning  an unknown key in a namespace the registry knows ("editor.fontSzie"); a duplicate key
 *            (the last one wins); an `executes` key in a project file of an untrusted project
 *            (reported by the store, which knows the trust state).
 *   Info     schemaVersion newer than this build (read as far as understood, never rewritten);
 *            a migration that ran in memory.
 * Unknown keys elsewhere ("layout", "breakpoints" in workspace.json) are not reported: other
 * features keep their own members in the same files.
 */
#pragma once

#include "frost/settings/migration.hpp"
#include "frost/settings/registry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::settings {

enum class Severity : std::uint8_t { Error, Warning, Info };

// "error", "warning", "info".
[[nodiscard]] std::string_view toString(Severity severity) noexcept;

struct Diagnostic {
    Severity severity = Severity::Error;
    Scope scope = Scope::Application;
    std::string path;         // The file.
    std::string key;          // The member, or "" for the whole file.
    std::uint32_t line = 0;   // 1-based; 0 when unknown.
    std::uint32_t column = 0; // 1-based, in code points; 0 when unknown.
    std::string message;      // English.
    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

// "<path>:<line>:<column>: <severity>: <key>: <message>" (parts that are empty or 0 left out).
[[nodiscard]] std::string formatDiagnostic(const Diagnostic& diagnostic);

/*
 * Every diagnostic for `bytes` read as `scope`'s file at `path` (Error, Warning and Info above,
 * without the trust warning). Never fails; `bytes` is not changed. O(file size).
 */
[[nodiscard]] std::vector<Diagnostic> validateSettingsText(const Registry& registry, Scope scope, std::string_view path,
                                                           std::string_view bytes,
                                                           const MigrationSet& migrations = builtinMigrations());

// What the shell shows for a file's diagnostics: one notification per Error or Warning (Info is
// for the log), at most `limit`, then one that counts the rest.
struct SettingsNotification {
    Severity severity = Severity::Error;
    std::string title;   // "Settings file has errors: settings.json".
    std::string message; // formatDiagnostic's text.
    std::string path;    // Where the editor should go.
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

[[nodiscard]] std::vector<SettingsNotification> toNotifications(std::span<const Diagnostic> diagnostics,
                                                                std::size_t limit = 10);

} // namespace frost::settings
