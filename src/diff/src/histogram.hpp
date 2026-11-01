// SPDX-License-Identifier: Apache-2.0
//
// The histogram diff over id sequences (ADR 0019, D3): the algorithm of JGit's HistogramDiff and
// HistogramDiffIndex, run the way git runs it (no common prefix/suffix trimming), with Myers as
// the fallback for regions where every common line repeats more than 64 times. See histogram.cpp
// for the source citation and the rules.
#pragma once

#include "myers.hpp"
#include "work_budget.hpp"

#include <cstdint>
#include <span>

namespace frost::diff::detail {

struct HistogramResult {
    std::uint32_t fallbacks = 0; // Regions handed to Myers.
    bool exhausted = false;      // The budget ran out; the unfinished regions are whole replaces.
};

// Whether the histogram reuses work across regions (index narrowing, the small-region check and
// the run cache; histogram.cpp). Both settings give identical flags when the budget suffices; Off
// is the plain rules, kept as the oracle of Histogram.ReuseIsExact.
enum class HistogramReuse : std::uint8_t { On, Off };

/*
 * Marks the histogram diff of a against b in the change flags (all 0 on entry; ids < idCount).
 * Precondition: both sides non-empty (the caller handles the empty cases). When the budget runs
 * out, the current region and every queued region become whole replaces, which is valid because
 * the regions are disjoint and the common sequences between them are true matches.
 * Typically O((N + M) * D); with reuse, edits at evenly spaced lines cost about O(N + D^2) instead.
 * Bounded by the budget. State is allocated once per call: about 16 bytes per distinct line, 4 per
 * line of A and (with reuse) 12 per line of B.
 */
[[nodiscard]] HistogramResult histogramDiff(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b,
                                            std::uint32_t idCount, std::span<std::uint8_t> changedA,
                                            std::span<std::uint8_t> changedB, WorkBudget& budget,
                                            HistogramReuse reuse = HistogramReuse::On) noexcept;

} // namespace frost::diff::detail
