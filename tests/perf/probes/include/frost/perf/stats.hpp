// SPDX-License-Identifier: Apache-2.0
//
// Sample statistics for the perf harnesses: nearest-rank percentiles and a
// fixed summary (count, min, mean, p50, p90, p99, max). Portable and pure, so
// the budget arithmetic is unit-tested on both platforms.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace frost::perf {

// The nearest-rank percentile of `sorted` (ascending): the smallest sample such
// that at least p percent of the samples are less than or equal to it, i.e.
// sorted[ceil(p / 100 * n) - 1], clamped to the first sample for p = 0.
// Nearest rank never interpolates, so a reported p99 is always a measured value,
// which is the conservative reading for a budget. Preconditions: `sorted` is
// non-empty and ascending, 0 <= p <= 100. O(1).
[[nodiscard]] double percentile(std::span<const double> sorted, double p) noexcept;

// Summary of one metric. All fields are zero when count is zero.
struct Summary {
    std::size_t count = 0;
    double min = 0;
    double mean = 0;
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
    double max = 0;
};

// Summarises `samples` (any order; copied and sorted). O(n log n).
[[nodiscard]] Summary summarize(std::span<const double> samples);

// The arithmetic mean of `samples`, or 0 for an empty span. O(n).
[[nodiscard]] double mean(std::span<const double> samples) noexcept;

} // namespace frost::perf
