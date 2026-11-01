// SPDX-License-Identifier: Apache-2.0
//
// MigrationSet (migration.hpp): running migration steps over a SettingsDocument, and FrostIDE's own
// steps.

#include "frost/settings/migration.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::settings {

MigrationSet::MigrationSet(std::uint32_t currentVersion, std::vector<Migration> steps)
    : m_current(currentVersion), m_steps(std::move(steps)) {
    std::ranges::sort(m_steps, {}, &Migration::from);
    FROST_PRE(m_steps.size() == m_current);
    for (std::uint32_t i = 0; i < m_steps.size(); ++i) {
        FROST_PRE(m_steps[i].from == i && m_steps[i].apply);
    }
}

Result<MigrationReport> MigrationSet::apply(SettingsDocument& document) const {
    MigrationReport report;
    if (document.isEmpty()) {
        report.fromVersion = report.toVersion = m_current;
        return report;
    }
    const std::uint32_t version = document.schemaVersion().value_or(0);
    report.fromVersion = report.toVersion = version;
    if (version > m_current) {
        report.newer = true;
        return report;
    }
    if (version == m_current && document.schemaVersion().has_value()) {
        return report;
    }
    SettingsDocument working = document;
    for (std::uint32_t from = version; from < m_current; ++from) {
        const Migration& step = m_steps[from];
        if (auto ok = step.apply(working); !ok) {
            Error error = ok.error();
            return std::unexpected(std::move(error).withContext(
                std::format("while migrating settings from version {} ({})", from, step.description)));
        }
    }
    if (auto ok = working.setSchemaVersion(m_current); !ok) {
        return std::unexpected(ok.error());
    }
    report.toVersion = m_current;
    report.changed = working.bytes() != document.bytes();
    document = std::move(working);
    return report;
}

const MigrationSet& builtinMigrations() {
    // Built on first use and never destroyed (no exit-time destructor; startup hygiene, PLAN.md 2.1).
    static const MigrationSet* const migrations = new MigrationSet(
        kCurrentSchemaVersion, {Migration{.from = 0,
                                          .description = "stamp schemaVersion on files written before it existed",
                                          .apply = [](SettingsDocument&) -> Result<void> { return {}; }}});
    return *migrations;
}

} // namespace frost::settings
