// SPDX-License-Identifier: Apache-2.0
// The snapshot validator worker accepts exactly one versioned request on standard input.
#pragma once

#include "frost/lang/snapshot_validator.hpp"

namespace frost::lang {

inline constexpr std::size_t kSnapshotValidationMaxJsonBytes = 2 * 1024 * 1024;
// JSON string escaping can expand each source or path byte to six bytes.
inline constexpr std::size_t kSnapshotValidationMaxPathBytes = 32 * 1024;
inline constexpr std::size_t kSnapshotValidationMaxInputBytes =
    6 * (kSnapshotValidationMaxJsonBytes + kSnapshotValidationMaxPathBytes) + kSnapshotValidationMaxJsonBytes + 128;

[[nodiscard]] Result<std::string> validateSnapshotRequest(std::string_view);

} // namespace frost::lang
