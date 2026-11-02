// SPDX-License-Identifier: Apache-2.0
//
// frost::settings internals: one scope file checked against the registry, shared by the store's
// layer loading and validateSettingsText() so both report the same diagnostics.
#pragma once

#include "frost/settings/document.hpp"
#include "frost/settings/migration.hpp"
#include "frost/settings/registry.hpp"
#include "frost/settings/validation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::settings::detail {

struct CheckedDocument {
    bool parsed = false;                      // The text parsed (diagnostics may still hold errors).
    bool newer = false;                       // schemaVersion is newer than the migrations know.
    SettingsDocument document;                // After in-memory migration.
    std::vector<std::optional<Value>> values; // By registry index: the valid values this file sets.
    // (registry index, (line, column)) of each value in `values`, for later diagnostics.
    std::vector<std::pair<std::size_t, std::pair<std::uint32_t, std::uint32_t>>> positions;
    std::vector<Diagnostic> diagnostics;
};

// Parses, migrates and checks `bytes` as `scope`'s file (validation.hpp lists the checks). O(n).
[[nodiscard]] CheckedDocument checkDocument(const Registry& registry, Scope scope, std::string_view path,
                                            std::string_view bytes, const MigrationSet& migrations);

} // namespace frost::settings::detail
