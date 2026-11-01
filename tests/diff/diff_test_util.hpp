// SPDX-License-Identifier: Apache-2.0
//
// Helpers shared by the frost::diff unit, stress and perf tests: sequence construction, gtest
// wrappers of the support library's property checks (ADR 0019, D16) and build-type scaling of
// case counts.
#pragma once

#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"

#include "diff/support/checks.hpp"
#include "diff/support/goldens.hpp"
#include "diff/support/oracles.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace frost::diff::test {

// `optimized` cases in optimised unsanitised builds, `debug` in Debug, `sanitized` under a sanitizer.
constexpr std::uint32_t scaled(std::uint32_t optimized, std::uint32_t debug, std::uint32_t sanitized) noexcept {
    if (frost::test::kSanitized) {
        return sanitized;
    }
    return frost::test::kOptimized ? optimized : debug;
}

inline LineSequence makeSequence(std::string bytes, LineSplit split = LineSplit::Lsp) {
    auto sequence = LineSequence::fromBytes(std::move(bytes), split);
    EXPECT_TRUE(sequence.has_value()) << sequence.error().describe();
    return sequence ? std::move(*sequence) : LineSequence();
}

// checkApplies (support/checks.hpp) as a gtest assertion.
inline ::testing::AssertionResult appliesCleanly(const LineSequence& a, const LineSequence& b, const LineDiff& diff,
                                                 const DiffOptions& options = {}) {
    if (auto checked = checkApplies(a, b, diff, options); !checked) {
        return ::testing::AssertionFailure() << checked.error().describe();
    }
    return ::testing::AssertionSuccess();
}

// checkRefinement (support/checks.hpp) as a gtest assertion.
inline ::testing::AssertionResult refinedApplies(const LineSequence& a, const LineSequence& b, const Hunk& hunk,
                                                 const Refinement& refinement) {
    if (auto checked = checkRefinement(a, b, hunk, refinement); !checked) {
        return ::testing::AssertionFailure() << checked.error().describe();
    }
    return ::testing::AssertionSuccess();
}

} // namespace frost::diff::test
