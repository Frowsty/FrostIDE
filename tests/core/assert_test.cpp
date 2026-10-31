// SPDX-License-Identifier: Apache-2.0
//
// Tests for the contract macros: the default "enforce" semantic aborts on a
// violation after calling the handler; "observe" calls the handler and
// continues; "ignore" never evaluates the condition; "quick_enforce" traps.

#include "frost/core/assert.hpp"

#include "assert_semantics.hpp"
#include "support/crash_dialogs.hpp"

#include <gtest/gtest.h>

#include <atomic>

namespace {

// Records violations seen by the test handler.
std::atomic<int> g_violations{0};
std::atomic<frost::contracts::Kind> g_lastKind{frost::contracts::Kind::Assertion};

void recordingHandler(const frost::contracts::Violation& violation) noexcept {
    g_lastKind.store(violation.kind);
    g_violations.fetch_add(1);
}

int enforcedDivide(int a, int b) {
    FROST_PRE(b != 0);
    return a / b;
}

TEST(Contracts, PassingChecksHaveNoEffect) {
    EXPECT_EQ(enforcedDivide(10, 2), 5);
    FROST_ASSERT(1 + 1 == 2);
    FROST_POST(true);
}

TEST(ContractsDeathTest, EnforceAbortsWithDiagnostic) {
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            (void)enforcedDivide(1, 0);
        },
        "precondition violated: b != 0");
}

TEST(Contracts, ObserveCallsHandlerAndContinues) {
    g_violations = 0;
    const auto previous = frost::contracts::setViolationHandler(&recordingHandler);
    frost::test::checkObserved(false);
    frost::test::checkObserved(true);
    frost::contracts::setViolationHandler(previous);
    EXPECT_EQ(g_violations.load(), 1);
    EXPECT_EQ(g_lastKind.load(), frost::contracts::Kind::Precondition);
}

TEST(Contracts, IgnoreDoesNotEvaluateCondition) {
    EXPECT_EQ(frost::test::checkIgnored(false), 0);
    EXPECT_EQ(frost::test::checkIgnored(true), 0);
}

TEST(ContractsDeathTest, QuickEnforceTraps) {
    frost::test::checkQuickEnforced(true);
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            frost::test::checkQuickEnforced(false);
        },
        "");
}

TEST(Contracts, SetHandlerReturnsPrevious) {
    const auto original = frost::contracts::setViolationHandler(&recordingHandler);
    EXPECT_EQ(frost::contracts::setViolationHandler(original), &recordingHandler);
}

} // namespace
