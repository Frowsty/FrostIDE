// SPDX-License-Identifier: Apache-2.0
//
// ISessionState tests (PLAN.md section 6.3): the precheck policy in
// evaluateSession() for each of the three checks, unknown states, and
// precheck() through fake ISessionState implementations.

#include "frost/perf/session_state.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace {

using namespace frost::perf;

SessionFacts goodFacts() {
    SessionFacts facts;
    facts.inputDesktopOpened = true;
    facts.inputDesktopName = "Default";
    facts.lock = LockState::Unlocked;
    facts.processSession = 1;
    facts.activeConsoleSession = 1;
    facts.consoleDisplay = DisplayPower::On;
    facts.outputs.push_back(OutputFacts{"\\\\.\\DISPLAY1", true, 0, 0, 5120, 1440, 144.0});
    return facts;
}

bool mentions(const SessionVerdict& verdict, std::string_view text) {
    return std::ranges::any_of(verdict.problems,
                               [&](const std::string& problem) { return problem.find(text) != std::string::npos; });
}

TEST(SessionState, InteractiveUnlockedSessionIsValid) {
    const auto verdict = evaluateSession(goodFacts());
    EXPECT_TRUE(verdict.valid);
    EXPECT_TRUE(verdict.problems.empty());
}

TEST(SessionState, LockedWorkstationIsInvalid) {
    auto facts = goodFacts();
    facts.inputDesktopOpened = false; // OpenInputDesktop fails while Winlogon's desktop has input.
    facts.inputDesktopName.clear();
    facts.lock = LockState::Locked;
    const auto verdict = evaluateSession(facts);
    EXPECT_FALSE(verdict.valid);
    EXPECT_TRUE(mentions(verdict, "input desktop cannot be opened"));
    EXPECT_TRUE(mentions(verdict, "locked"));
}

TEST(SessionState, SecureDesktopWithInputIsInvalid) {
    auto facts = goodFacts();
    facts.inputDesktopName = "Winlogon";
    const auto verdict = evaluateSession(facts);
    EXPECT_FALSE(verdict.valid);
    EXPECT_TRUE(mentions(verdict, "'Winlogon'"));
}

TEST(SessionState, DesktopNameComparisonIgnoresCase) {
    auto facts = goodFacts();
    facts.inputDesktopName = "default";
    EXPECT_TRUE(evaluateSession(facts).valid);
}

TEST(SessionState, UnknownLockStateIsInvalid) {
    auto facts = goodFacts();
    facts.lock = LockState::Unknown;
    const auto verdict = evaluateSession(facts);
    EXPECT_FALSE(verdict.valid);
    EXPECT_TRUE(mentions(verdict, "lock state"));
}

TEST(SessionState, RemoteSessionIsInvalid) {
    auto facts = goodFacts();
    facts.processSession = 3; // An RDP session while session 1 owns the console.
    const auto verdict = evaluateSession(facts);
    EXPECT_FALSE(verdict.valid);
    EXPECT_TRUE(mentions(verdict, "not the active console session 1"));
}

TEST(SessionState, ServiceSessionIsInvalid) {
    auto facts = goodFacts();
    facts.processSession = 0;
    const auto verdict = evaluateSession(facts);
    EXPECT_FALSE(verdict.valid);
    EXPECT_TRUE(mentions(verdict, "session 0"));
}

TEST(SessionState, NoConsoleSessionIsInvalid) {
    auto facts = goodFacts();
    facts.activeConsoleSession = 0xFFFFFFFFU;
    const auto verdict = evaluateSession(facts);
    EXPECT_FALSE(verdict.valid);
    EXPECT_TRUE(mentions(verdict, "no active console session"));
}

TEST(SessionState, MonitorOffIsInvalid) {
    auto facts = goodFacts();
    facts.consoleDisplay = DisplayPower::Off;
    const auto verdict = evaluateSession(facts);
    EXPECT_FALSE(verdict.valid);
    EXPECT_TRUE(mentions(verdict, "display is off"));
}

TEST(SessionState, UnknownDisplayPowerIsInvalid) {
    auto facts = goodFacts();
    facts.consoleDisplay = DisplayPower::Unknown;
    EXPECT_FALSE(evaluateSession(facts).valid);
}

TEST(SessionState, DimmedDisplayStillComposesAndIsValid) {
    auto facts = goodFacts();
    facts.consoleDisplay = DisplayPower::Dimmed;
    EXPECT_TRUE(evaluateSession(facts).valid);
}

TEST(SessionState, DetachedOutputsAreInvalid) {
    auto facts = goodFacts();
    facts.outputs.front().attachedToDesktop = false;
    const auto verdict = evaluateSession(facts);
    EXPECT_FALSE(verdict.valid);
    EXPECT_TRUE(mentions(verdict, "no output attached"));

    facts.outputs.clear();
    EXPECT_FALSE(evaluateSession(facts).valid);
}

TEST(SessionState, OneAttachedOutputOfSeveralIsEnough) {
    auto facts = goodFacts();
    facts.outputs.push_back(OutputFacts{"\\\\.\\DISPLAY2", false, 5120, 0, 1920, 1080, 60.0});
    EXPECT_TRUE(evaluateSession(facts).valid);
}

TEST(SessionState, EveryFailedCheckIsReported) {
    SessionFacts facts; // Everything unknown or false.
    facts.activeConsoleSession = 0xFFFFFFFFU;
    const auto verdict = evaluateSession(facts);
    EXPECT_FALSE(verdict.valid);
    EXPECT_GE(verdict.problems.size(), 5u);
}

class FakeSessionState final : public ISessionState {
public:
    explicit FakeSessionState(frost::Result<SessionFacts> facts) : m_facts(std::move(facts)) {}
    [[nodiscard]] frost::Result<SessionFacts> facts() const override { return m_facts; }

private:
    frost::Result<SessionFacts> m_facts;
};

TEST(SessionState, PrecheckEvaluatesTheFacts) {
    const FakeSessionState good(goodFacts());
    EXPECT_TRUE(precheck(good).valid);

    auto locked = goodFacts();
    locked.lock = LockState::Locked;
    const FakeSessionState bad(locked);
    EXPECT_FALSE(precheck(bad).valid);
}

TEST(SessionState, UnreadableFactsMakeTheRunInvalid) {
    const FakeSessionState broken(frost::fail(frost::ErrorCode::IoError, "DXGI is unavailable"));
    const auto verdict = precheck(broken);
    EXPECT_FALSE(verdict.valid);
    EXPECT_TRUE(mentions(verdict, "DXGI is unavailable"));
}

TEST(SessionState, StateNames) {
    EXPECT_STREQ(toString(LockState::Locked), "locked");
    EXPECT_STREQ(toString(LockState::Unknown), "unknown");
    EXPECT_STREQ(toString(DisplayPower::Dimmed), "dimmed");
    EXPECT_STREQ(toString(DisplayPower::On), "on");
}

} // namespace
