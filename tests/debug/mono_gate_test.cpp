// SPDX-License-Identifier: Apache-2.0
// Test Mono protocol operation gating and request ownership.
#include "frost/debug/mono_gate.hpp"

#include <gtest/gtest.h>

namespace frost::debug::mono {
namespace {
TEST(MonoGate, NegotiatesBeforeOperationsAndClearsStepBeforeDispose) {
    SessionGate gate;
    EXPECT_FALSE(gate.begin(Operation::Breakpoint));
    ASSERT_TRUE(gate.handshakeComplete());
    auto version = gate.begin(Operation::Version);
    ASSERT_TRUE(version);
    EXPECT_FALSE(gate.begin(Operation::Negotiate));
    ASSERT_TRUE(gate.complete(*version, true, false, 2, 58));
    auto negotiate = gate.begin(Operation::Negotiate);
    ASSERT_TRUE(negotiate);
    ASSERT_TRUE(gate.complete(*negotiate, true, true));
    auto breakpoint = gate.begin(Operation::Breakpoint);
    ASSERT_TRUE(breakpoint);
    ASSERT_TRUE(gate.complete(*breakpoint, true, false, 0, 0, 9));
    EXPECT_FALSE(gate.begin(Operation::ClearBreakpoint, 8));
    auto clearBreakpoint = gate.begin(Operation::ClearBreakpoint, 9);
    ASSERT_TRUE(clearBreakpoint);
    ASSERT_TRUE(gate.complete(*clearBreakpoint, true));
    auto step = gate.begin(Operation::Step);
    ASSERT_TRUE(step);
    ASSERT_TRUE(gate.complete(*step, true, false, 0, 0, 7));
    EXPECT_EQ(gate.activeStep(), 7u);
    EXPECT_FALSE(gate.begin(Operation::Dispose));
    auto clear = gate.begin(Operation::ClearStep);
    ASSERT_TRUE(clear);
    ASSERT_TRUE(gate.complete(*clear, true));
    auto dispose = gate.begin(Operation::Dispose);
    ASSERT_TRUE(dispose);
    ASSERT_TRUE(gate.complete(*dispose, true));
    EXPECT_EQ(gate.state().phase(), Phase::Ended);
    gate.close();
    EXPECT_FALSE(gate.uncleanClose());
}
TEST(MonoGate, ReloadRejectsStaleCompletionsAndKeepsStepOwnership) {
    SessionGate gate;
    ASSERT_TRUE(gate.handshakeComplete());
    auto version = gate.begin(Operation::Version);
    ASSERT_TRUE(version);
    ASSERT_TRUE(gate.complete(*version, true, false, 2, 58));
    auto negotiate = gate.begin(Operation::Negotiate);
    ASSERT_TRUE(negotiate);
    ASSERT_TRUE(gate.complete(*negotiate, true, true));
    auto step = gate.begin(Operation::Step);
    ASSERT_TRUE(step);
    ASSERT_TRUE(gate.complete(*step, true, false, 0, 0, 4));
    auto pending = gate.begin(Operation::Frames);
    ASSERT_TRUE(pending);
    gate.reload();
    EXPECT_FALSE(gate.complete(*pending, true));
    EXPECT_EQ(gate.activeStep(), 4u);
    EXPECT_FALSE(gate.begin(Operation::Dispose));
    auto clear = gate.begin(Operation::ClearStep);
    ASSERT_TRUE(clear);
    ASSERT_TRUE(gate.complete(*clear, true));
    ASSERT_TRUE(gate.reloaded(true));
    EXPECT_TRUE(gate.begin(Operation::Dispose));
}
TEST(MonoGate, ReloadDuringStepCannotAssumeItWasNotInstalled) {
    SessionGate gate;
    ASSERT_TRUE(gate.handshakeComplete());
    auto version = gate.begin(Operation::Version);
    ASSERT_TRUE(version);
    ASSERT_TRUE(gate.complete(*version, true, false, 2, 58));
    auto negotiate = gate.begin(Operation::Negotiate);
    ASSERT_TRUE(negotiate);
    ASSERT_TRUE(gate.complete(*negotiate, true, true));
    auto step = gate.begin(Operation::Step);
    ASSERT_TRUE(step);
    gate.reload();
    EXPECT_FALSE(gate.complete(*step, true, false, 0, 0, 4));
    ASSERT_TRUE(gate.reloaded(true));
    EXPECT_FALSE(gate.begin(Operation::Dispose));
    EXPECT_FALSE(gate.begin(Operation::Resume));
    gate.close();
    EXPECT_TRUE(gate.uncleanClose());
}
} // namespace
} // namespace frost::debug::mono
