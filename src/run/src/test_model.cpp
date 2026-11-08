// SPDX-License-Identifier: Apache-2.0
//
// The test model's names and the duration format (test_model.hpp). Builds no result: the parsers
// in ctest_parsers.cpp and gtest_parsers.cpp do.

#include "frost/run/test_model.hpp"

#include <format>

namespace frost::run {

std::string_view toString(TestOutcome outcome) noexcept {
    switch (outcome) {
    case TestOutcome::NotRun: return "Not run";
    case TestOutcome::Passed: return "Passed";
    case TestOutcome::Failed: return "Failed";
    case TestOutcome::Skipped: return "Skipped";
    case TestOutcome::Disabled: return "Disabled";
    }
    return "Not run";
}

bool countsAsFailure(TestOutcome outcome) noexcept {
    return outcome == TestOutcome::Failed || outcome == TestOutcome::NotRun;
}

std::string formatTestDuration(std::optional<std::chrono::microseconds> duration) {
    if (!duration)
        return {};
    const auto us = duration->count();
    if (us < 1000)
        return "< 1 ms";
    if (us < 1'000'000)
        return std::format("{} ms", us / 1000);
    if (us < 60'000'000)
        return std::format("{:.2f} s", static_cast<double>(us) / 1e6);
    const auto seconds = us / 1'000'000;
    return std::format("{} min {:02} s", seconds / 60, seconds % 60);
}

} // namespace frost::run
