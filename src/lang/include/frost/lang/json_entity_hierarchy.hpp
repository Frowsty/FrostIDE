// SPDX-License-Identifier: Apache-2.0
// Entity structure built only from captured engine JSON IDs and Relationship.Parent values.
#pragma once
#include "frost/lang/lsp_types.hpp"

#include <cstdint>
#include <span>
#include <string>
namespace frost::lang {
struct JsonEntityStructureNode {
    std::uint64_t id = 0, parent = 0;
    std::string name;
    LspRange range, selectionRange; // Actual entity object and ID token ranges in the captured image.
};
// Pure/thread-safe, O(n log n), at most 10000 entities/64 parent levels. Refuses missing or
// duplicate IDs, unresolved parents and cycles; it never invents a parent or drops an entity.
// Result is LSP DocumentSymbol[]; hierarchy ranges encompass actual children and selections
// retain the exact captured ID token. Errors: Corrupt for invalid topology, OutOfRange for bounds.
[[nodiscard]] Result<Json> jsonEntityHierarchy(std::span<const JsonEntityStructureNode>);
} // namespace frost::lang
