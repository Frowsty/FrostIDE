// SPDX-License-Identifier: Apache-2.0
//
// Tests for the contract macros: the default "enforce" semantic aborts on a
// violation after calling the handler; "observe" calls the handler and
// continues; "ignore" never evaluates the condition; "quick_enforce" traps.

#include "frost/core/assert.hpp"

#include "assert_semantics.hpp"
#include "support/crash_dialogs.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <thread>

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

// A checked function whose body is harmless after a failed check, so a death can
// only come from the contract machinery (not from, say, a division by zero).
int enforcedIdentity(int value) {
    FROST_PRE(value > 0);
    return value;
}

TEST(Contracts, PassingChecksHaveNoEffect) {
    EXPECT_EQ(enforcedDivide(10, 2), 5);
    FROST_ASSERT(1 + 1 == 2);
    FROST_POST(true);
}

TEST(ContractsDeathTest, EnforceAbortsWithDiagnostic) {
    // std::abort: SIGABRT on POSIX, exit code 3 on Windows. Under "observe" the
    // process would survive and the death assertion would fail.
#if defined(_WIN32)
    const auto aborted = testing::ExitedWithCode(3);
#else
    const auto aborted = testing::KilledBySignal(SIGABRT);
#endif
    EXPECT_EXIT(
        {
            frost::test::disableCrashDialogs();
            (void)enforcedIdentity(-1);
            std::exit(0);
        },
        aborted, "precondition violated: value > 0");
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

TEST(ContractsDeathTest, QuickEnforceTrapsWithoutHandler) {
    frost::test::checkQuickEnforced(true);
    // __builtin_trap: SIGILL on x86-64 Linux, STATUS_ILLEGAL_INSTRUCTION on Windows.
    // The handler is never called, so "violated" must not appear in the output.
#if defined(_WIN32)
    const auto trapped = testing::ExitedWithCode(static_cast<int>(0xC000001D));
#else
    const auto trapped = testing::KilledBySignal(SIGILL);
#endif
    // The trap runs on its own thread: on Windows, gtest wraps the test body in an
    // SEH handler that would turn a trap on this thread into a normal exit(1).
    EXPECT_EXIT(
        {
            frost::test::disableCrashDialogs();
            std::thread([] { frost::test::checkQuickEnforced(false); }).join();
            std::exit(0);
        },
        trapped, testing::Not(testing::HasSubstr("violated")));
}

TEST(Contracts, SetHandlerReturnsPrevious) {
    const auto original = frost::contracts::setViolationHandler(&recordingHandler);
    EXPECT_EQ(frost::contracts::setViolationHandler(original), &recordingHandler);
}

} // namespace
