// SPDX-License-Identifier: Apache-2.0
//
// The outline seam of the editor model (ADR 0025): IOutlineProvider lists the member ranges
// (functions, methods, types, ...) of a snapshot for the next/previous member commands. m1/syntax's
// tree-sitter outline will implement it; tests use a fixed-list double.
#pragma once

#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"

#include <vector>

namespace frost::text {

/*
 * Lists the members of a snapshot. Contract: the ranges lie inside `text` (begin <= end <= size,
 * both char boundaries), in any order; nested members may overlap. The commands go to a member's
 * begin. Lifetime: borrowed by the call. Thread safety: implementation-defined; the commands call
 * it on the caller's thread. Cost: the implementation's (the syntax host answers from its tree).
 */
class IOutlineProvider {
public:
    virtual ~IOutlineProvider() = default;

    // The member ranges of `text`.
    [[nodiscard]] virtual std::vector<ByteRange> members(const Snapshot& text) const noexcept = 0;

protected:
    IOutlineProvider() = default;
    IOutlineProvider(const IOutlineProvider&) = default;
    IOutlineProvider& operator=(const IOutlineProvider&) = default;
};

} // namespace frost::text
