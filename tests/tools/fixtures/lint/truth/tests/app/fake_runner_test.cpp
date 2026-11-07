// SPDX-License-Identifier: Apache-2.0
// Test doubles may build results and imitate tool output: they feed the parsers.
#include "fake_runner.hpp"

TEST(Parser, ReadsGtestOutput) {
    TestResult expected{.name = "Suite.Case", .passed = true};
    const auto parsed = parse("[ RUN      ] Suite.Case\n[       OK ] Suite.Case (3 ms)\n");
    EXPECT_EQ(parsed.front().name, expected.name);
    EXPECT_EQ(stateOf(parsed.front()), UnitTestRunState::Passed);
}
