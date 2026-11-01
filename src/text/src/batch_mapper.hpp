// SPDX-License-Identifier: Apache-2.0
//
// BatchMapper (private header; ADR 0017, decision 1): maps a non-decreasing sequence of offsets
// through one raw batch in O(m) setup plus amortised O(1) per offset, for the sweep paths of
// AnchorSet::transformed and SelectionSet::transformed. Equal to mapOffset(x, bias, batch).
#pragma once

#include "frost/text/anchor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace frost::text::detail {

/*
 * Precomputes, for every non-no-op step i of the batch, where its range starts and ends in the
 * text after the batch, the Left target of an offset inside it (the start of its touching run,
 * see mapOffset in anchor.hpp) and the Right target of its end offset b_i (which may also lie in
 * inserts and a range starting at b_i). map() then walks a cursor to the first step with b >= x.
 *
 * Not thread-safe as an object (the cursor moves). Allocation failure terminates.
 */
class BatchMapper {
public:
    // Copies the batch's non-no-op steps. Precondition: sorted like applyEdits. O(m).
    explicit BatchMapper(std::span<const RawEdit> batch) noexcept;

    // The image of `x` under the batch with `bias`. Precondition: x is not smaller than the
    // previous call's x (any bias). Amortised O(1).
    [[nodiscard]] std::uint64_t map(std::uint64_t x, Bias bias) noexcept;

    // The batch's size change, Σ (newBytes - |range|). O(1).
    [[nodiscard]] std::int64_t totalDelta() const noexcept { return m_totalDelta; }
    // True when the batch has no step that changes anything.
    [[nodiscard]] bool empty() const noexcept { return m_steps.empty(); }

private:
    struct Step {
        std::uint64_t begin = 0;      // a, before the batch.
        std::uint64_t end = 0;        // b, before the batch.
        std::uint64_t startFinal = 0; // a + D_i: where the replacement starts after the batch.
        std::uint64_t endFinal = 0;   // a + n + D_i: where it ends.
        std::uint64_t leftTarget = 0; // Left image of any a <= x <= b: startFinal of the run start.
        std::uint64_t rightAtEnd = 0; // Right image of x == b.
    };

    std::vector<Step> m_steps;
    std::size_t m_next = 0; // The first step whose end is >= the last mapped offset.
    std::uint64_t m_last = 0;
    std::int64_t m_totalDelta = 0;
};

} // namespace frost::text::detail
