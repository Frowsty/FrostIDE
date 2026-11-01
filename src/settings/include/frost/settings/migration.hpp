// SPDX-License-Identifier: Apache-2.0
//
// Scope-file migrations (PLAN.md section 3.6, ADR 0021): each step is a pure function that edits a
// SettingsDocument from version N to N + 1 with SettingsDocument's byte-exact edits, so a migrated
// file differs from the original only where the step changed it. Golden tests pin each step.
/*
 * A file without schemaVersion is version 0. MigrationSet::apply runs every step from the file's
 * version up to currentVersion(), then stamps schemaVersion. A file at a newer version than this
 * build knows is left untouched (MigrationReport::newer), and FrostIDE never rewrites its version.
 * The store migrates in memory on load and writes the migrated text only when it next saves that
 * file itself, so opening a project never dirties committed files.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/settings/document.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace frost::settings {

// The scope-file format version this build writes.
inline constexpr std::uint32_t kCurrentSchemaVersion = 1;

// One step: edits `document` from version `from` to `from + 1`. Must be deterministic and touch
// nothing but the document. The step does not stamp schemaVersion; apply() does.
struct Migration {
    std::uint32_t from = 0;
    std::string description; // English, for the log.
    std::function<Result<void>(SettingsDocument&)> apply;
};

struct MigrationReport {
    std::uint32_t fromVersion = 0; // The file's version (0 when absent).
    std::uint32_t toVersion = 0;   // The version after apply (== fromVersion when nothing ran).
    bool changed = false;          // The bytes changed.
    bool newer = false;            // The file is newer than currentVersion(); nothing was done.
};

class MigrationSet {
public:
    // Precondition (FROST_PRE): `steps` holds exactly one step for each version in [0, currentVersion).
    MigrationSet(std::uint32_t currentVersion, std::vector<Migration> steps);

    [[nodiscard]] std::uint32_t currentVersion() const noexcept { return m_current; }
    [[nodiscard]] std::span<const Migration> steps() const noexcept { return m_steps; }

    /*
     * Brings `document` to currentVersion() (see the file comment). An empty document is left
     * empty (nothing to migrate). Errors: a step's error, with context naming the step; the
     * document is then unchanged. O(steps * file size).
     */
    [[nodiscard]] Result<MigrationReport> apply(SettingsDocument& document) const;

private:
    std::uint32_t m_current;
    std::vector<Migration> m_steps; // Sorted by `from`.
};

// FrostIDE's migrations up to kCurrentSchemaVersion. Version 0 -> 1 only adds schemaVersion.
[[nodiscard]] const MigrationSet& builtinMigrations();

} // namespace frost::settings
