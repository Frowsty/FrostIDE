// SPDX-License-Identifier: Apache-2.0
//
// Scope-file validation against the registry, diagnostic text and notifications (validation.hpp).
// The store builds its layers with checkDocument(), so loading and validation on save agree.

#include "frost/settings/validation.hpp"

#include "validation_internal.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <set>
#include <system_error>
#include <utility>

namespace frost::settings {

std::string_view toString(Severity severity) noexcept {
    switch (severity) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Info: return "info";
    }
    return "Severity(?)";
}

std::string formatDiagnostic(const Diagnostic& diagnostic) {
    std::string text = diagnostic.path;
    if (diagnostic.line != 0) {
        text += std::format(":{}", diagnostic.line);
        if (diagnostic.column != 0) {
            text += std::format(":{}", diagnostic.column);
        }
    }
    if (!text.empty()) {
        text += ": ";
    }
    text += toString(diagnostic.severity);
    text += ": ";
    if (!diagnostic.key.empty()) {
        text += diagnostic.key + ": ";
    }
    text += diagnostic.message;
    return text;
}

namespace {

struct Positioned {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string reason;
};

// Splits the parser's "line L, column C: reason" into its parts; other text is all reason.
[[nodiscard]] Positioned splitPosition(std::string_view message) {
    Positioned result{.line = 0, .column = 0, .reason = std::string(message)};
    constexpr std::string_view kLine = "line ";
    constexpr std::string_view kColumn = ", column ";
    if (!message.starts_with(kLine)) {
        return result;
    }
    const char* cursor = message.data() + kLine.size();
    const char* end = message.data() + message.size();
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    auto parsedLine = std::from_chars(cursor, end, line);
    if (parsedLine.ec != std::errc() || !std::string_view(parsedLine.ptr, end).starts_with(kColumn)) {
        return result;
    }
    auto parsedColumn = std::from_chars(parsedLine.ptr + kColumn.size(), end, column);
    if (parsedColumn.ec != std::errc() || !std::string_view(parsedColumn.ptr, end).starts_with(": ")) {
        return result;
    }
    return Positioned{.line = line, .column = column, .reason = std::string(parsedColumn.ptr + 2, end)};
}

// The last path component, for either separator.
[[nodiscard]] std::string_view baseName(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

namespace detail {

CheckedDocument checkDocument(const Registry& registry, Scope scope, std::string_view path, std::string_view bytes,
                              const MigrationSet& migrations) {
    CheckedDocument checked;
    checked.values.resize(registry.size());
    const auto report = [&](Severity severity, std::string key, std::uint32_t line, std::uint32_t column,
                            std::string message) {
        checked.diagnostics.push_back(Diagnostic{.severity = severity,
                                                 .scope = scope,
                                                 .path = std::string(path),
                                                 .key = std::move(key),
                                                 .line = line,
                                                 .column = column,
                                                 .message = std::move(message)});
    };

    auto parsed = SettingsDocument::parse(std::string(bytes));
    if (!parsed) {
        // "line L, column C: reason" from the parser; keep the position as numbers too.
        auto [line, column, reason] = splitPosition(parsed.error().message());
        report(Severity::Error, {}, line, column, std::move(reason));
        return checked;
    }
    checked.parsed = true;
    // Positions are reported against the file as it is on disk, not the migrated text.
    const SettingsDocument original = *parsed;
    const auto positionOf = [&](std::string_view key) -> std::pair<std::uint32_t, std::uint32_t> {
        const SettingsDocument::Member* member = original.find(key);
        return member != nullptr ? std::pair{member->line, member->column} : std::pair{0U, 0U};
    };
    SettingsDocument document = std::move(*parsed);
    auto migrated = migrations.apply(document);
    if (!migrated) {
        report(Severity::Error, {}, 0, 0, migrated.error().describe());
    } else if (migrated->newer) {
        report(Severity::Info, std::string(kSchemaVersionKey), 0, 0,
               std::format("written by a newer FrostIDE (version {}; this build knows {}): unknown keys are kept, "
                           "and FrostIDE will not change this file",
                           migrated->fromVersion, migrations.currentVersion()));
        checked.newer = true;
    } else if (migrated->changed) {
        report(Severity::Info, std::string(kSchemaVersionKey), 0, 0,
               std::format("migrated from version {} to {} in memory; saved on the next change", migrated->fromVersion,
                           migrated->toVersion));
    }

    std::set<std::string_view> seen;
    for (const SettingsDocument::Member& member : original.members()) {
        if (!seen.insert(member.key).second) {
            report(Severity::Warning, member.key, member.line, member.column,
                   "is set more than once; the last one is used");
        }
    }
    for (std::size_t i = 0; i < document.members().size(); ++i) {
        const SettingsDocument::Member& member = document.members()[i];
        if (member.key == kSchemaVersionKey) {
            if (!document.schemaVersion()) {
                const auto [line, column] = positionOf(member.key);
                report(Severity::Error, member.key, line, column, "must be a non-negative integer");
            }
            continue;
        }
        if (document.find(member.key) != &member) {
            continue; // An earlier duplicate: only the last occurrence counts.
        }
        const auto [line, column] = positionOf(member.key);
        const auto index = registry.indexOf(member.key);
        if (!index) {
            if (registry.knowsNamespace(member.key)) {
                report(Severity::Warning, member.key, line, column, "is not a known setting");
            }
            continue;
        }
        const KeySpec& spec = registry.at(*index);
        if (!spec.scopes.has(scope)) {
            report(Severity::Error, member.key, line, column,
                   std::format("cannot be set in {} settings", toString(scope)));
            continue;
        }
        auto value = document.value(member.key, spec.schema);
        if (!value) {
            report(Severity::Error, member.key, line, column, value.error().message());
            continue;
        }
        checked.values[*index] = std::move(*value);
        checked.positions.emplace_back(*index, std::pair{line, column});
    }
    checked.document = std::move(document);
    return checked;
}

} // namespace detail

std::vector<Diagnostic> validateSettingsText(const Registry& registry, Scope scope, std::string_view path,
                                             std::string_view bytes, const MigrationSet& migrations) {
    return detail::checkDocument(registry, scope, path, bytes, migrations).diagnostics;
}

std::vector<SettingsNotification> toNotifications(std::span<const Diagnostic> diagnostics, std::size_t limit) {
    std::vector<SettingsNotification> notifications;
    std::size_t shown = 0;
    std::size_t hidden = 0;
    std::string lastPath;
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == Severity::Info) {
            continue;
        }
        if (shown == limit) {
            ++hidden;
            lastPath = diagnostic.path;
            continue;
        }
        const std::string_view name =
            diagnostic.path.empty() ? std::string_view("settings") : baseName(diagnostic.path);
        notifications.push_back(SettingsNotification{
            .severity = diagnostic.severity,
            .title = std::format("Settings file has {}: {}",
                                 diagnostic.severity == Severity::Error ? "errors" : "warnings", name),
            .message = formatDiagnostic(diagnostic),
            .path = diagnostic.path,
            .line = diagnostic.line,
            .column = diagnostic.column});
        ++shown;
    }
    if (hidden > 0) {
        notifications.push_back(SettingsNotification{.severity = Severity::Warning,
                                                     .title = "More settings problems",
                                                     .message = std::format("{} more problems are not shown", hidden),
                                                     .path = lastPath,
                                                     .line = 0,
                                                     .column = 0});
    }
    return notifications;
}

} // namespace frost::settings
