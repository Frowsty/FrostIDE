// SPDX-License-Identifier: Apache-2.0
// Immutable source input and raw byte ranges for the local snapshot validator.
#pragma once

#include "frost/lang/json.hpp"
#include "frost/text/snapshot.hpp"

namespace frost::lang {

inline constexpr int kSnapshotValidationVersion = 1;
inline constexpr std::size_t kSnapshotValidationMaxOutputBytes = 1024 * 1024;
inline constexpr std::size_t kSnapshotValidationMaxIssues = 256;

struct SnapshotValidationIssue {
    std::string_view source;
    std::string_view code;
    std::string message;
    text::ByteRange range;
};

// An engine shape may only be supplied from the selected schema for this document.
[[nodiscard]] Result<std::string> makeSnapshotValidationRequest(std::string_view path, const text::Snapshot& source,
                                                                const std::optional<Json>& engineSchema = {});

} // namespace frost::lang
