// SPDX-License-Identifier: Apache-2.0
// Exact unmerged index stage identities from actual git ls-files --unmerged -z output.
#pragma once
#include "frost/core/error.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
namespace frost::vcs {
struct ConflictStage {
    std::string object, mode;
};
// Pure/thread-safe, bounded 3 records/1MiB. Requires exact selected path, regular file modes,
// unique stages1..3 and full SHA1/SHA256 object IDs. Paths may contain spaces, tabs and newlines.
// Missing slots represent real absent stages; no base is inferred from conflict marker text.
[[nodiscard]] Result<std::array<std::optional<ConflictStage>, 3>> parseConflictStages(std::string_view bytes,
                                                                                      std::string_view relativePath);
} // namespace frost::vcs
