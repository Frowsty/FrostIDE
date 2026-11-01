// SPDX-License-Identifier: Apache-2.0
//
// DoubleShift: the double-tap detector of shortcut_matcher.cpp in virtual time (a
// ManualClock and SimulatedKeyboard OS ticks): the inclusive interval boundary on OS and
// steady time, interruptions, the sliding window, the 32-bit tick wrap, stall
// compression, DoubleCtrl and the Windows AltGr fake Ctrl.

#include "frost/input/shortcut_matcher.hpp"

#include "input/support/matcher_rig.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace frost::input {
namespace {

using namespace std::chrono_literals;
using test::MatcherRig;
using test::Step;

constexpr IClock::Duration kT = 400ms; // MatcherOptions' default interval.

std::vector<std::string> firedIn(const MatcherRig& rig, const std::vector<Step>& steps) {
    std::vector<std::string> out;
    for (const Step& step : steps) {
        for (const MatchResult& r : step.results) {
            if (r.outcome == MatchOutcome::Fire) {
                out.push_back(rig.name(r.action));
            }
        }
    }
    return out;
}

// Press 1, release 1, press 2, release 2 of `key`, with the given gaps before events 2-4.
std::vector<Step> doubleTap(MatcherRig& rig, Code key, std::array<IClock::Duration, 3> gaps) {
    std::vector<Step> steps;
    steps.push_back(rig.press(key));
    rig.clock.advance(gaps[0]);
    steps.push_back(rig.release(key));
    rig.clock.advance(gaps[1]);
    steps.push_back(rig.press(key));
    rig.clock.advance(gaps[2]);
    steps.push_back(rig.release(key));
    return steps;
}

TEST(DoubleShift, FiresAtTheSecondRelease) {
    MatcherRig rig;
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());
    EXPECT_TRUE(rig.press(Code::ShiftLeft).fired.empty());
    const Step release = rig.release(Code::ShiftLeft);
    EXPECT_EQ(release.fired, "search.everywhere");
    EXPECT_EQ(release.fire.sequence, Sequence::of(Gesture::DoubleShift));
    EXPECT_FALSE(release.fire.cancelledPending);
}

TEST(DoubleShift, OsTimeBoundaryIsInclusive) {
    for (std::size_t gap = 0; gap < 3; ++gap) {
        MatcherRig rig;
        std::array<IClock::Duration, 3> gaps = {kT, kT, kT};
        EXPECT_EQ(firedIn(rig, doubleTap(rig, Code::ShiftLeft, gaps)), std::vector<std::string>{"search.everywhere"})
            << "every gap exactly T";
        rig.resetKeyboard();
        rig.clock.advance(10s);
        gaps[gap] = kT + 1ms;
        EXPECT_TRUE(firedIn(rig, doubleTap(rig, Code::ShiftLeft, gaps)).empty()) << "gap " << gap << " is T+1 ms";
    }
}

TEST(DoubleShift, SteadyTimeBoundaryWithoutOsTimestamps) {
    for (std::size_t gap = 0; gap < 3; ++gap) {
        MatcherRig rig;
        rig.keyboard.setOsTimestamps(false);
        std::array<IClock::Duration, 3> gaps = {kT, kT, kT};
        EXPECT_EQ(firedIn(rig, doubleTap(rig, Code::ShiftLeft, gaps)).size(), 1U);
        rig.clock.advance(10s);
        gaps[gap] = kT + 1ns;
        EXPECT_TRUE(firedIn(rig, doubleTap(rig, Code::ShiftLeft, gaps)).empty()) << "gap " << gap << " is T+1 ns";
    }
}

TEST(DoubleShift, MixedTimestampsFallBackToSteadyTime) {
    MatcherRig rig;
    static_cast<void>(rig.tap(Code::ShiftLeft));
    rig.keyboard.setOsTimestamps(false); // The second tap has no OS time.
    rig.keyboard.advance(5s);            // A wild OS-tick jump is ignored on the steady basis.
    EXPECT_EQ(rig.tap(Code::ShiftLeft).fired, "search.everywhere");

    // Only the final release lacks an OS time: the steady clock still decides.
    rig.resetKeyboard();
    rig.keyboard.setOsTimestamps(true);
    static_cast<void>(rig.tap(Code::ShiftLeft));
    static_cast<void>(rig.press(Code::ShiftLeft));
    rig.keyboard.setOsTimestamps(false);
    EXPECT_EQ(rig.release(Code::ShiftLeft).fired, "search.everywhere");
}

TEST(DoubleShift, HeldFirstPressDoesNotFireButSlides) {
    MatcherRig rig;
    const std::vector<Step> slow = doubleTap(rig, Code::ShiftLeft, {kT + 1ms, 10ms, 10ms});
    EXPECT_TRUE(firedIn(rig, slow).empty());
    // The second tap became a first tap: one more quick tap completes a pair.
    rig.clock.advance(10ms);
    EXPECT_EQ(rig.tap(Code::ShiftLeft).fired, "search.everywhere");
}

TEST(DoubleShift, KeyBetweenTapsDoesNotFire) {
    MatcherRig rig;
    static_cast<void>(rig.tap(Code::ShiftLeft));
    static_cast<void>(rig.tap(Code::KeyA));
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());

    rig.resetKeyboard();
    static_cast<void>(rig.press(Code::KeyA));
    static_cast<void>(rig.tap(Code::ShiftLeft));
    static_cast<void>(rig.release(Code::KeyA)); // A release of an earlier key between the taps.
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());

    rig.resetKeyboard();
    static_cast<void>(rig.tap(Code::ShiftLeft));
    static_cast<void>(rig.tap(Code::AltLeft)); // Another modifier between the taps.
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());

    // A key pressed (and still held) between the taps: its press alone interrupts.
    rig.resetKeyboard();
    static_cast<void>(rig.tap(Code::ShiftLeft));
    static_cast<void>(rig.press(Code::KeyX));
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty()) << "X is still down";
    static_cast<void>(rig.release(Code::KeyX));
}

TEST(DoubleShift, CtrlHeldDuringTheTapsDoesNotFire) {
    MatcherRig rig;
    static_cast<void>(rig.press(Code::ControlLeft));
    static_cast<void>(rig.tap(Code::ShiftLeft));
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());
    static_cast<void>(rig.release(Code::ControlLeft));
}

TEST(DoubleShift, LeftThenRightShiftFires) {
    MatcherRig rig;
    static_cast<void>(rig.tap(Code::ShiftLeft));
    EXPECT_EQ(rig.tap(Code::ShiftRight).fired, "search.everywhere");
}

TEST(DoubleShift, OverlappingShiftKeysDoNotFire) {
    MatcherRig rig;
    static_cast<void>(rig.press(Code::ShiftLeft));
    static_cast<void>(rig.press(Code::ShiftRight));
    static_cast<void>(rig.release(Code::ShiftRight));
    EXPECT_TRUE(rig.release(Code::ShiftLeft).fired.empty()) << "the release is not of the key just pressed";
    // That ShiftLeft release came after the ShiftRight tap, so it interrupts: one more
    // tap does not complete a pair with the ShiftRight tap.
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());
    EXPECT_EQ(rig.tap(Code::ShiftLeft).fired, "search.everywhere");
}

TEST(DoubleShift, TripleTapFiresOnce) {
    MatcherRig rig;
    std::vector<Step> steps;
    for (int i = 0; i < 3; ++i) {
        steps.push_back(rig.tap(Code::ShiftLeft));
    }
    EXPECT_EQ(firedIn(rig, steps).size(), 1U);
    EXPECT_EQ(rig.tap(Code::ShiftLeft).fired, "search.everywhere") << "the third tap started a new pair";
}

TEST(DoubleShift, AutorepeatOfShiftIsIgnored) {
    MatcherRig rig;
    static_cast<void>(rig.press(Code::ShiftLeft));
    for (int i = 0; i < 3; ++i) {
        const Step repeat = rig.press(Code::ShiftLeft);
        EXPECT_TRUE(repeat.keyEvent.autoRepeat);
    }
    static_cast<void>(rig.release(Code::ShiftLeft));
    static_cast<void>(rig.press(Code::ShiftLeft));
    static_cast<void>(rig.press(Code::ShiftLeft));
    EXPECT_EQ(rig.release(Code::ShiftLeft).fired, "search.everywhere");
}

TEST(DoubleShift, ImeCompositionBetweenTheTapsResets) {
    MatcherRig rig;
    static_cast<void>(rig.tap(Code::ShiftLeft));
    rig.keyboard.setImeComposing(true);
    static_cast<void>(rig.tap(Code::KeyA));
    rig.keyboard.setImeComposing(false);
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());
}

TEST(DoubleShift, TapWhileComposingThenAfterCommitDoesNotFire) {
    MatcherRig rig;
    rig.keyboard.setImeComposing(true);
    static_cast<void>(rig.tap(Code::ShiftLeft));
    rig.keyboard.setImeComposing(false);
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());
}

TEST(DoubleShift, ZeroIntervalDisables) {
    MatcherRig rig;
    rig.matcher.setOptions(MatcherOptions{.doubleTapInterval = IClock::Duration::zero()});
    static_cast<void>(rig.tap(Code::ShiftLeft));
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());
    rig.matcher.setOptions(MatcherOptions{});
    static_cast<void>(rig.tap(Code::ShiftLeft));
    EXPECT_EQ(rig.tap(Code::ShiftLeft).fired, "search.everywhere");
}

TEST(DoubleShift, TapsAcrossTheOsTickWrapFire) {
    MatcherRig rig;
    rig.keyboard.setOsTick(std::numeric_limits<std::uint32_t>::max() - 100);
    const std::vector<Step> steps = doubleTap(rig, Code::ShiftLeft, {60ms, 60ms, 60ms});
    EXPECT_LT(steps[3].keyEvent.osTimeMs, steps[0].keyEvent.osTimeMs) << "the tick wrapped";
    EXPECT_EQ(firedIn(rig, steps), std::vector<std::string>{"search.everywhere"});
    // And a slow pair across the wrap still does not fire.
    rig.clock.advance(5s);
    rig.keyboard.setOsTick(std::numeric_limits<std::uint32_t>::max() - 10);
    EXPECT_TRUE(firedIn(rig, doubleTap(rig, Code::ShiftLeft, {10ms, kT + 1ms, 10ms})).empty());
}

TEST(DoubleShift, StallCompressionDoesNotFakeADoubleTap) {
    MatcherRig rig;
    static_cast<void>(rig.tap(Code::ShiftLeft));
    rig.keyboard.advance(2s); // The OS saw the taps 2 s apart; the stalled UI thread sees them 1 ms apart.
    rig.clock.advance(1ms);
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty());
}

TEST(DoubleShift, DoubleCtrlIsIndependent) {
    MatcherRig rig;
    static_cast<void>(rig.tap(Code::ControlLeft));
    EXPECT_EQ(rig.tap(Code::ControlRight).fired, "tools.runAnything");
    rig.resetKeyboard();
    static_cast<void>(rig.tap(Code::ShiftLeft));
    EXPECT_TRUE(rig.tap(Code::ControlLeft).fired.empty()) << "a Shift tap then a Ctrl tap";
    EXPECT_TRUE(rig.tap(Code::ShiftLeft).fired.empty()) << "a Ctrl tap then a Shift tap";
}

TEST(DoubleShift, WindowsAltGrTwiceDoesNotFireDoubleCtrl) {
    MatcherRig rig(simulatedLayout(SimulatedLayoutId::DeDe), KeyPlatform::Windows);
    const Step first = rig.tap(Code::AltRight);
    ASSERT_EQ(first.events.size(), 4U) << "fake LCtrl press, AltRight press, fake LCtrl release, AltRight release";
    EXPECT_EQ(first.events[0].code, Code::ControlLeft);
    EXPECT_TRUE(first.fired.empty());
    EXPECT_TRUE(rig.tap(Code::AltRight).fired.empty());
}

} // namespace
} // namespace frost::input
