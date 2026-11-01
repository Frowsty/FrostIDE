// SPDX-License-Identifier: Apache-2.0
//
// Tests for the nearest-rank statistics used by every perf report (frost/perf/stats.hpp).

#include "frost/perf/stats.hpp"

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

namespace {

using frost::perf::percentile;
using frost::perf::summarize;

std::vector<double> oneToHundred() {
    std::vector<double> values(100);
    std::iota(values.begin(), values.end(), 1.0);
    return values;
}

TEST(PerfStats, NearestRankPercentilesOfOneToHundred) {
    const auto values = oneToHundred();
    EXPECT_EQ(percentile(values, 0.0), 1.0);
    EXPECT_EQ(percentile(values, 1.0), 1.0);
    EXPECT_EQ(percentile(values, 50.0), 50.0);
    EXPECT_EQ(percentile(values, 90.0), 90.0);
    EXPECT_EQ(percentile(values, 99.0), 99.0);
    EXPECT_EQ(percentile(values, 99.5), 100.0);
    EXPECT_EQ(percentile(values, 100.0), 100.0);
}

TEST(PerfStats, PercentileNeverInterpolates) {
    // With 10 samples, p99 is the 10th (ceil(9.9)), never a value between samples.
    const std::vector<double> values{1, 2, 3, 4, 5, 6, 7, 8, 9, 1000};
    EXPECT_EQ(percentile(values, 99.0), 1000.0);
    EXPECT_EQ(percentile(values, 90.0), 9.0);
    EXPECT_EQ(percentile(values, 50.0), 5.0);
}

TEST(PerfStats, SingleSampleIsEveryPercentile) {
    const std::vector<double> values{4.25};
    EXPECT_EQ(percentile(values, 0.0), 4.25);
    EXPECT_EQ(percentile(values, 50.0), 4.25);
    EXPECT_EQ(percentile(values, 100.0), 4.25);
}

TEST(PerfStats, SummarizeSortsACopy) {
    const std::vector<double> values{5, 1, 4, 2, 3};
    const auto summary = summarize(values);
    EXPECT_EQ(summary.count, 5u);
    EXPECT_EQ(summary.min, 1.0);
    EXPECT_EQ(summary.max, 5.0);
    EXPECT_EQ(summary.mean, 3.0);
    EXPECT_EQ(summary.p50, 3.0);
    EXPECT_EQ(summary.p99, 5.0);
    EXPECT_EQ(values.front(), 5.0); // The input is untouched.
}

TEST(PerfStats, EmptySummaryIsZero) {
    const auto summary = summarize(std::vector<double>{});
    EXPECT_EQ(summary.count, 0u);
    EXPECT_EQ(summary.max, 0.0);
    EXPECT_EQ(summary.p99, 0.0);
    EXPECT_EQ(frost::perf::mean(std::vector<double>{}), 0.0);
}

} // namespace
