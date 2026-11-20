// SPDX-License-Identifier: Apache-2.0
// Actual engine document/content validator replies; no locally invented semantic findings.
#pragma once
#include "frost/core/error.hpp"
#include "frost/rpc/json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace frost::engine {
struct DocumentIssue {
    std::string severity, code, message, asset, location;
    std::uint64_t line = 0, column = 0, byte = 0; // Zero is unknown; byte is actual 1-based captured-image byte.
};
struct DocumentValidation {
    bool valid = false;
    std::vector<DocumentIssue> issues;
};
// Pure, thread-safe. expectedAsset nonempty requires document.validate protocol/version/path.
// Empty parses actual content.validate's issue envelope. Bounded 10000 rows/4MiB issue text;
// only Assets/ relative paths, no traversal/absolute endpoints. Parse errors are not diagnostics.
[[nodiscard]] Result<DocumentValidation> parseDocumentValidation(const rpc::Json&, std::string_view expectedAsset = {});
} // namespace frost::engine
