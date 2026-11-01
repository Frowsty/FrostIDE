// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the nearest-rank statistics (see frost/perf/stats.hpp).

#include "frost/perf/stats.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace frost::perf {

double percentile(std::span<const double> sorted, double p) noexcept {
    FROST_PRE(!sorted.empty());
    FROST_PRE(p >= 0.0 && p <= 100.0);
    const auto n = static_cast<double>(sorted.size());
    // Rank is 1-based; p = 0 maps to the minimum.
    const double rank = std::ceil(p / 100.0 * n);
    const auto index = rank < 1.0 ? std::size_t{0} : static_cast<std::size_t>(rank) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

double mean(std::span<const double> samples) noexcept {
    if (samples.empty()) {
        return 0.0;
    }
    return std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
}

Summary summarize(std::span<const double> samples) {
    Summary summary;
    if (samples.empty()) {
        return summary;
    }
    std::vector<double> sorted(samples.begin(), samples.end());
    std::ranges::sort(sorted);
    summary.count = sorted.size();
    summary.min = sorted.front();
    summary.max = sorted.back();
    summary.mean = mean(sorted);
    summary.p50 = percentile(sorted, 50.0);
    summary.p90 = percentile(sorted, 90.0);
    summary.p99 = percentile(sorted, 99.0);
    return summary;
}

} // namespace frost::perf
