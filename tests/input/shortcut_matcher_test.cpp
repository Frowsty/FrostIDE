// SPDX-License-Identifier: Apache-2.0
//
// ShortcutMatcher and ShortcutSequences: the feed algorithm of shortcut_matcher.cpp on
// synthetic keymaps (single strokes, gate order, releases, autorepeat, unknown codes, the
// NumLock remap, Alt-code entry, typedText) and two-stroke sequences, plus the seeded
// random-stream property per layout and platform (ShortcutMatcherProperty).

#include "frost/input/shortcut_matcher.hpp"

#include "input/support/matcher_rig.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <tuple>

namespace frost::input {
namespace {

using test::MatcherRig;
using test::Step;

constexpr std::string_view kSingles = "frost-keymap 1\nid singles\nname Singles\n[default]\n"
                                      "test.high = Ctrl+J\n"
                                      "test.low = Ctrl+J\n"
                                      "test.up = Ctrl+Up\n"
                                      "test.home = Ctrl+Home\n"
                                      "test.numpadEight = Ctrl+Numpad8\n"
                                      "test.delete = Ctrl+Delete\n"
                                      "test.duplicate = Ctrl+D\n"
                                      "test.commentBlock = Ctrl+K, Ctrl+C\n"
                                      "test.commit = Ctrl+K\n";

constexpr std::string_view kSequences = "frost-keymap 1\nid sequences\nname Sequences\n[default]\n"
                                        "test.commentBlock = Ctrl+K, Ctrl+C\n"
                                        "test.bare = Ctrl+K, C\n"
                                        "test.commit = Ctrl+K\n"
                                        "file.saveAll = Ctrl+S\n"
                                        "editor.copy = Ctrl+C\n"
                                        "search.everywhere = DoubleShift\n";

const LayoutTable& enUs() { return simulatedLayout(SimulatedLayoutId::EnUs); }
const LayoutTable& deDe() { return simulatedLayout(SimulatedLayoutId::DeDe); }
const LayoutTable& plPl() { return simulatedLayout(SimulatedLayoutId::PlPl); }

Sequence seq(std::string_view text) {
    auto parsed = parseSequence(text, SyntaxMode::Strict);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(Sequence{});
}

std::u32string typed(const KeyEvent& e, KeyPlatform platform) { return std::u32string(typedText(e, platform)); }

// Every result of the step except the main key's press is PassThrough.
void expectOthersPassThrough(const Step& step, Code key) {
    for (std::size_t i = 0; i < step.events.size(); ++i) {
        if (step.events[i].code == key && step.events[i].isPress()) {
            continue;
        }
        EXPECT_EQ(step.results[i].outcome, MatchOutcome::PassThrough) << i;
    }
}

TEST(ShortcutMatcher, SingleStrokeFires) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    const Step step = rig.stroke(Modifier::Ctrl, Code::KeyJ);
    EXPECT_EQ(step.keyResult.outcome, MatchOutcome::Fire);
    EXPECT_EQ(rig.name(step.keyResult.action), "test.high");
    EXPECT_EQ(step.keyResult.sequence, seq("Ctrl+J"));
    EXPECT_FALSE(step.keyResult.cancelledPending);
    expectOthersPassThrough(step, Code::KeyJ);
    EXPECT_EQ(toString(MatchOutcome::Fire), "Fire");
    EXPECT_EQ(toString(MatchOutcome::Pending), "Pending");
}

TEST(ShortcutMatcher, GateOrderFirstEnabledCandidateWins) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    const ActionId high = rig.idOf("test.high");
    const ActionId low = rig.idOf("test.low");
    rig.enabled = [high](ActionId id) { return id != high; };
    EXPECT_EQ(rig.name(rig.stroke(Modifier::Ctrl, Code::KeyJ).keyResult.action), "test.low");
    rig.enabled = [high, low](ActionId id) { return id != high && id != low; };
    const Step none = rig.stroke(Modifier::Ctrl, Code::KeyJ);
    EXPECT_EQ(none.keyResult.outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(none.keyResult.action, kNoAction);
}

TEST(ShortcutMatcher, ReleasesPassThrough) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    EXPECT_EQ(rig.feed(test::keyRelease(Code::KeyJ, Modifier::Ctrl)).outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(rig.feed(test::keyRelease(Code::ControlLeft)).outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(rig.feed(test::keyRelease(Code::Unknown)).outcome, MatchOutcome::PassThrough);
}

TEST(ShortcutMatcher, AutorepeatFiresRepeatedly) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    static_cast<void>(rig.press(Code::ControlLeft));
    for (int i = 0; i < 4; ++i) {
        const Step step = rig.press(Code::KeyD);
        EXPECT_EQ(step.keyEvent.autoRepeat, i > 0);
        EXPECT_EQ(step.keyResult.outcome, MatchOutcome::Fire) << i;
        EXPECT_EQ(rig.name(step.keyResult.action), "test.duplicate");
    }
    EXPECT_EQ(rig.release(Code::KeyD).keyResult.outcome, MatchOutcome::PassThrough);
}

TEST(ShortcutMatcher, AutorepeatNeverStartsOrCompletesASequence) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    // A repeat of a prefix stroke with nothing pending is consumed, not Pending.
    KeyEvent repeatK = test::keyPress(Code::KeyK, Modifier::Ctrl);
    repeatK.autoRepeat = true;
    const MatchResult first = rig.feed(repeatK);
    EXPECT_EQ(first.outcome, MatchOutcome::Consumed);
    EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());

    static_cast<void>(rig.press(Code::ControlLeft));
    EXPECT_EQ(rig.press(Code::KeyK).keyResult.outcome, MatchOutcome::Pending);
    EXPECT_EQ(rig.press(Code::KeyK).keyResult.outcome, MatchOutcome::Consumed) << "the prefix key repeats";
    EXPECT_EQ(rig.matcher.pendingPrefix(), (Stroke{Modifier::Ctrl, Key::K}));
    KeyEvent repeatC = test::keyPress(Code::KeyC, Modifier::Ctrl);
    repeatC.autoRepeat = true;
    EXPECT_EQ(rig.feed(repeatC).outcome, MatchOutcome::Consumed) << "a repeat never completes a sequence";
    EXPECT_TRUE(rig.matcher.pendingPrefix().has_value());
    static_cast<void>(rig.release(Code::KeyK));
    const Step complete = rig.press(Code::KeyC);
    EXPECT_EQ(complete.keyResult.outcome, MatchOutcome::Fire);
    EXPECT_EQ(rig.name(complete.keyResult.action), "test.commentBlock");
}

TEST(ShortcutMatcher, UnknownCode) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    EXPECT_EQ(rig.feed(test::keyPress(Code::Unknown)).outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(rig.feed(test::keyPress(Code::Unknown, Modifier::Ctrl)).outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(rig.feed(test::keyPress(Code::KeyK, Modifier::Ctrl)).outcome, MatchOutcome::Pending);
    const MatchResult r = rig.feed(test::keyPress(Code::Unknown, Modifier::Ctrl));
    EXPECT_EQ(r.outcome, MatchOutcome::Consumed);
    EXPECT_TRUE(r.cancelledPending);
    EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());
    // Out-of-range codes behave like Unknown.
    EXPECT_EQ(rig.feed(test::keyPress(static_cast<Code>(250), Modifier::Ctrl)).outcome, MatchOutcome::PassThrough);
}

// Code::Unknown is also the "no swallow-repeat code" value: holding an unmapped key (an
// ABNT2 keypad comma, a synthesized event without a scan code) must keep repeating.
TEST(ShortcutMatcher, UnknownCodeAutorepeatPassesThrough) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    KeyEvent press = test::keyPress(Code::Unknown);
    press.text = {U',', 0};
    press.textLength = 1;
    EXPECT_EQ(rig.feed(press).outcome, MatchOutcome::PassThrough);
    press.autoRepeat = true;
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(rig.feed(press).outcome, MatchOutcome::PassThrough) << i;
    }
    // After a sequence cancelled by an unmapped key, its autorepeat still passes through.
    EXPECT_EQ(rig.feed(test::keyPress(Code::KeyK, Modifier::Ctrl)).outcome, MatchOutcome::Pending);
    press.autoRepeat = false;
    EXPECT_EQ(rig.feed(press).outcome, MatchOutcome::Consumed);
    press.autoRepeat = true;
    EXPECT_EQ(rig.feed(press).outcome, MatchOutcome::PassThrough);
}

TEST(ShortcutMatcher, NumpadWithNumLockOffMapsToNavigation) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    EXPECT_EQ(rig.name(rig.stroke(Modifier::Ctrl, Code::Numpad8).fire.action), "test.numpadEight");
    rig.keyboard.setNumLock(false);
    EXPECT_EQ(rig.name(rig.stroke(Modifier::Ctrl, Code::Numpad8).fire.action), "test.up");
    EXPECT_EQ(rig.name(rig.stroke(Modifier::Ctrl, Code::Numpad7).fire.action), "test.home");
    EXPECT_EQ(rig.name(rig.stroke(Modifier::Ctrl, Code::NumpadDecimal).fire.action), "test.delete");
    const Step five = rig.stroke(Modifier::Ctrl, Code::Numpad5);
    EXPECT_EQ(five.keyResult.outcome, MatchOutcome::PassThrough);
    EXPECT_FALSE(strokeFromEvent(five.keyEvent, *rig.table).has_value()) << "Numpad5 without NumLock is nothing";
    const Step plus = rig.stroke(Modifier::Ctrl, Code::NumpadAdd);
    EXPECT_EQ(strokeFromEvent(plus.keyEvent, *rig.table), (Stroke{Modifier::Ctrl, Key::NumpadAdd}));
}

TEST(ShortcutMatcher, WindowsAltNumpadEntryPassesThrough) {
    MatcherRig rig(enUs(), KeyPlatform::Windows);
    static_cast<void>(rig.press(Code::AltLeft));
    const Step one = rig.tap(Code::Numpad1);
    EXPECT_EQ(one.keyResult.outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(one.keyEvent.textLength, 0) << "Alt-code entry composes on Alt's release";
    EXPECT_TRUE(typedText(one.keyEvent, KeyPlatform::Windows).empty());
    static_cast<void>(rig.release(Code::AltLeft));
}

TEST(ShortcutMatcher, TypedTextRules) {
    {
        MatcherRig rig(enUs(), KeyPlatform::Windows);
        const Step altF = rig.stroke(Modifier::Alt, Code::KeyF);
        EXPECT_EQ(std::u32string(altF.keyEvent.textView()), U"f") << "WM_SYSCHAR text";
        EXPECT_EQ(typed(altF.keyEvent, KeyPlatform::Windows), U"") << "Windows Alt+F is not typed";
        EXPECT_EQ(typed(rig.tap(Code::KeyF).keyEvent, KeyPlatform::Windows), U"f");
        EXPECT_EQ(typed(rig.stroke(Modifier::Shift, Code::KeyF).keyEvent, KeyPlatform::Windows), U"F");
    }
    {
        MatcherRig rig(plPl(), KeyPlatform::Windows);
        const Step ctrlAlt = rig.chord({Code::ControlLeft, Code::AltLeft}, Code::KeyL);
        EXPECT_FALSE(ctrlAlt.keyEvent.altGr);
        EXPECT_EQ(typed(ctrlAlt.keyEvent, KeyPlatform::Windows), U"ł") << "LCtrl+LAlt types like AltGr";
        const Step altGr = rig.chord({Code::AltRight}, Code::KeyL);
        EXPECT_TRUE(altGr.keyEvent.altGr);
        EXPECT_EQ(typed(altGr.keyEvent, KeyPlatform::Windows), U"ł");
        EXPECT_EQ(altGr.keyResult.outcome, MatchOutcome::PassThrough);
        const Step withMeta = rig.chord({Code::ControlLeft, Code::AltLeft, Code::MetaLeft}, Code::KeyL);
        EXPECT_EQ(typed(withMeta.keyEvent, KeyPlatform::Windows), U"");
    }
    {
        MatcherRig rig(plPl(), KeyPlatform::X11);
        const Step altGr = rig.chord({Code::AltRight}, Code::KeyL);
        EXPECT_TRUE(altGr.keyEvent.altGr);
        EXPECT_EQ(typed(altGr.keyEvent, KeyPlatform::X11), U"ł");
        const Step ctrlA = rig.stroke(Modifier::Ctrl, Code::KeyA);
        EXPECT_EQ(std::u32string(ctrlA.keyEvent.textView()), U"\u0001");
        EXPECT_EQ(typed(ctrlA.keyEvent, KeyPlatform::X11), U"");
        // Even printable text is not typed under Ctrl on X11, nor under Ctrl+Alt.
        KeyEvent e = test::keyPress(Code::KeyA, Modifier::Ctrl);
        e.text[0] = U'a';
        e.textLength = 1;
        EXPECT_EQ(typed(e, KeyPlatform::X11), U"");
        e.modifiers = Modifier::Ctrl | Modifier::Alt;
        EXPECT_EQ(typed(e, KeyPlatform::X11), U"");
        EXPECT_EQ(typed(e, KeyPlatform::Windows), U"a");
        e.type = KeyEventType::Release;
        EXPECT_EQ(typed(e, KeyPlatform::Windows), U"") << "releases never type";
    }
}

TEST(ShortcutMatcher, ImeOwnsTheKeyboard) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    rig.keyboard.setImeComposing(true);
    const Step step = rig.stroke(Modifier::Ctrl, Code::KeyJ);
    for (const MatchResult& r : step.results) {
        EXPECT_EQ(r.outcome, MatchOutcome::PassThrough);
    }
    EXPECT_TRUE(step.keyEvent.imeComposing);
    EXPECT_EQ(step.keyEvent.textLength, 0);
}

TEST(ShortcutMatcher, PeekDoesNotChangeState) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    const auto gate = [](ActionId) { return true; };
    const KeyEvent ctrlK = test::keyPress(Code::KeyK, Modifier::Ctrl);
    EXPECT_EQ(rig.matcher.peek(ctrlK, rig.index, *rig.table, gate), MatchOutcome::Pending);
    EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());
    EXPECT_EQ(rig.feed(ctrlK).outcome, MatchOutcome::Pending);
    const KeyEvent ctrlC = test::keyPress(Code::KeyC, Modifier::Ctrl);
    EXPECT_EQ(rig.matcher.peek(ctrlC, rig.index, *rig.table, gate), MatchOutcome::Fire);
    EXPECT_TRUE(rig.matcher.pendingPrefix().has_value());
    rig.matcher.reset();
    EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());
}

TEST(ShortcutMatcher, SetOptionsKeepsThePendingStroke) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSingles);
    EXPECT_EQ(rig.feed(test::keyPress(Code::KeyK, Modifier::Ctrl)).outcome, MatchOutcome::Pending);
    rig.matcher.setOptions(MatcherOptions{.doubleTapInterval = std::chrono::milliseconds(250)});
    EXPECT_EQ(rig.matcher.options().doubleTapInterval, std::chrono::milliseconds(250));
    EXPECT_TRUE(rig.matcher.pendingPrefix().has_value());
}

// ShortcutSequences (synthetic keymap: Ctrl+K, Ctrl+C; Ctrl+K, C; Ctrl+K alone; Ctrl+S).

TEST(ShortcutSequences, PendingThenComplete) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    static_cast<void>(rig.press(Code::ControlLeft));
    const Step k = rig.press(Code::KeyK);
    EXPECT_EQ(k.keyResult.outcome, MatchOutcome::Pending);
    EXPECT_EQ(rig.matcher.pendingPrefix(), (Stroke{Modifier::Ctrl, Key::K}));
    EXPECT_EQ(rig.release(Code::KeyK).keyResult.outcome, MatchOutcome::PassThrough);
    EXPECT_TRUE(rig.matcher.pendingPrefix().has_value()) << "releases keep the pending stroke";
    const Step c = rig.press(Code::KeyC);
    EXPECT_EQ(c.keyResult.outcome, MatchOutcome::Fire);
    EXPECT_EQ(rig.name(c.keyResult.action), "test.commentBlock");
    EXPECT_EQ(c.keyResult.sequence, seq("Ctrl+K, Ctrl+C"));
    EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());
}

TEST(ShortcutSequences, BareSecondStroke) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    EXPECT_EQ(rig.stroke(Modifier::Ctrl, Code::KeyK).keyResult.outcome, MatchOutcome::Pending);
    const Step c = rig.tap(Code::KeyC);
    EXPECT_EQ(rig.name(c.keyResult.action), "test.bare");
    EXPECT_EQ(c.keyResult.sequence, seq("Ctrl+K, C"));
}

TEST(ShortcutSequences, WrongSecondStrokeIsConsumed) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    static_cast<void>(rig.stroke(Modifier::Ctrl, Code::KeyK));
    const Step x = rig.stroke(Modifier::Ctrl, Code::KeyX);
    EXPECT_EQ(x.keyResult.outcome, MatchOutcome::Consumed);
    EXPECT_TRUE(x.keyResult.cancelledPending);
    EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());
    EXPECT_EQ(rig.stroke(Modifier::Ctrl, Code::KeyS).fired, "file.saveAll");
}

TEST(ShortcutSequences, EscapeCancels) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    static_cast<void>(rig.stroke(Modifier::Ctrl, Code::KeyK));
    const Step esc = rig.tap(Code::Escape);
    EXPECT_EQ(esc.keyResult.outcome, MatchOutcome::Consumed);
    EXPECT_TRUE(esc.keyResult.cancelledPending);
    EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());
}

TEST(ShortcutSequences, ModifierActivityKeepsThePendingStroke) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    static_cast<void>(rig.stroke(Modifier::Ctrl, Code::KeyK));
    for (const Code modifier : {Code::ShiftLeft, Code::AltLeft, Code::ControlRight, Code::MetaLeft}) {
        const Step step = rig.tap(modifier);
        for (const MatchResult& r : step.results) {
            EXPECT_EQ(r.outcome, MatchOutcome::PassThrough);
        }
        EXPECT_TRUE(rig.matcher.pendingPrefix().has_value()) << codeName(modifier);
    }
    EXPECT_EQ(rig.stroke(Modifier::Ctrl, Code::KeyC).fired, "test.commentBlock");
}

TEST(ShortcutSequences, DeadKeyOrAltGrTextAsSecondStrokeCancels) {
    for (const KeyPlatform platform : {KeyPlatform::Windows, KeyPlatform::X11}) {
        MatcherRig rig(deDe(), platform, kSequences);
        static_cast<void>(rig.stroke(Modifier::Ctrl, Code::KeyK));
        const Step dead = rig.tap(Code::Backquote);
        EXPECT_EQ(dead.keyEvent.deadKey, U'^');
        EXPECT_EQ(dead.keyResult.outcome, MatchOutcome::Consumed);
        EXPECT_TRUE(dead.keyResult.cancelledPending);
        EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());
        // The OS's dead-key state survives: the next letter still composes.
        EXPECT_EQ(std::u32string(rig.tap(Code::KeyE).keyEvent.textView()), U"ê");

        static_cast<void>(rig.stroke(Modifier::Ctrl, Code::KeyK));
        const Step at = rig.chord({Code::AltRight}, Code::KeyQ);
        EXPECT_EQ(std::u32string(at.keyEvent.textView()), U"@");
        EXPECT_EQ(at.keyResult.outcome, MatchOutcome::Consumed);
        EXPECT_TRUE(at.keyResult.cancelledPending);
        EXPECT_TRUE(at.fired.empty());
    }
}

TEST(ShortcutSequences, ImeCancels) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    static_cast<void>(rig.stroke(Modifier::Ctrl, Code::KeyK));
    rig.keyboard.setImeComposing(true);
    const Step a = rig.tap(Code::KeyA);
    EXPECT_EQ(a.keyResult.outcome, MatchOutcome::PassThrough);
    EXPECT_TRUE(a.keyResult.cancelledPending);
    EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());
}

TEST(ShortcutSequences, PrefixBeatsTheSingleBindingAndIsReported) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    const Step k = rig.stroke(Modifier::Ctrl, Code::KeyK);
    EXPECT_EQ(k.keyResult.outcome, MatchOutcome::Pending);
    EXPECT_TRUE(k.fired.empty()) << "test.commit (Ctrl+K alone) does not fire";
    const std::vector<Conflict> conflicts = rig.index.conflicts();
    const auto it = std::ranges::find_if(conflicts, [](const Conflict& c) { return c.prefixConflict; });
    ASSERT_NE(it, conflicts.end());
    EXPECT_EQ(it->sequence, seq("Ctrl+K"));
    std::vector<std::string> names;
    for (const ActionId id : it->actions) {
        names.push_back(rig.name(id));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"test.commit", "test.bare", "test.commentBlock"}));
}

TEST(ShortcutSequences, GateDisabledTwoStrokeActionsDoNotMakeAPrefix) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    const ActionId block = rig.idOf("test.commentBlock");
    const ActionId bare = rig.idOf("test.bare");
    rig.enabled = [block, bare](ActionId id) { return id != block && id != bare; };
    const Step k = rig.stroke(Modifier::Ctrl, Code::KeyK);
    EXPECT_EQ(k.keyResult.outcome, MatchOutcome::Fire);
    EXPECT_EQ(rig.name(k.keyResult.action), "test.commit");
}

TEST(ShortcutSequences, CompletingKeyAutorepeatIsConsumedUntilRelease) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    static_cast<void>(rig.press(Code::ControlLeft));
    static_cast<void>(rig.tap(Code::KeyK));
    EXPECT_EQ(rig.press(Code::KeyC).fired, "test.commentBlock");
    for (int i = 0; i < 3; ++i) {
        const Step repeat = rig.press(Code::KeyC);
        EXPECT_TRUE(repeat.keyEvent.autoRepeat);
        EXPECT_EQ(repeat.keyResult.outcome, MatchOutcome::Consumed) << "never editor.copy";
    }
    static_cast<void>(rig.release(Code::KeyC));
    EXPECT_EQ(rig.press(Code::KeyC).fired, "editor.copy") << "after the release Ctrl+C is itself again";
    // The release cleared the swallowed key, so the new press's autorepeat repeats it.
    const Step held = rig.press(Code::KeyC);
    EXPECT_TRUE(held.keyEvent.autoRepeat);
    EXPECT_EQ(held.fired, "editor.copy");
}

// The release of the swallowed key ends the swallowing for good: typing that key again
// autorepeats normally, with or without the modifiers.
TEST(ShortcutSequences, SwallowedKeyRepeatsAgainAfterItsRelease) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    static_cast<void>(rig.press(Code::ControlLeft));
    static_cast<void>(rig.tap(Code::KeyK));
    EXPECT_EQ(rig.press(Code::KeyC).fired, "test.commentBlock");
    static_cast<void>(rig.release(Code::KeyC));
    static_cast<void>(rig.release(Code::ControlLeft));
    const Step first = rig.press(Code::KeyC);
    EXPECT_FALSE(first.keyEvent.autoRepeat);
    EXPECT_EQ(first.keyResult.outcome, MatchOutcome::PassThrough);
    for (int i = 0; i < 3; ++i) {
        const Step repeat = rig.press(Code::KeyC);
        EXPECT_TRUE(repeat.keyEvent.autoRepeat);
        EXPECT_EQ(repeat.keyResult.outcome, MatchOutcome::PassThrough) << "'c' keeps repeating, " << i;
        EXPECT_EQ(typed(repeat.keyEvent, KeyPlatform::Windows), U"c");
    }
}

TEST(ShortcutSequences, CancellingKeyAutorepeatIsConsumedUntilRelease) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    static_cast<void>(rig.press(Code::ControlLeft));
    static_cast<void>(rig.tap(Code::KeyK));
    const Step s = rig.press(Code::KeyS);
    EXPECT_EQ(s.keyResult.outcome, MatchOutcome::Consumed);
    EXPECT_TRUE(s.keyResult.cancelledPending);
    for (int i = 0; i < 3; ++i) {
        const Step repeat = rig.press(Code::KeyS);
        EXPECT_EQ(repeat.keyResult.outcome, MatchOutcome::Consumed) << "never file.saveAll";
    }
    static_cast<void>(rig.release(Code::KeyS));
    EXPECT_EQ(rig.press(Code::KeyS).fired, "file.saveAll");
}

TEST(ShortcutSequences, DoubleShiftWhilePendingClearsIt) {
    MatcherRig rig(enUs(), KeyPlatform::Windows, kSequences);
    static_cast<void>(rig.stroke(Modifier::Ctrl, Code::KeyK));
    static_cast<void>(rig.tap(Code::ShiftLeft));
    EXPECT_TRUE(rig.matcher.pendingPrefix().has_value());
    const Step second = rig.tap(Code::ShiftLeft);
    EXPECT_EQ(second.fired, "search.everywhere");
    EXPECT_TRUE(second.fire.cancelledPending);
    EXPECT_EQ(second.fire.sequence, Sequence::of(Gesture::DoubleShift));
    EXPECT_FALSE(rig.matcher.pendingPrefix().has_value());
}

// The random-stream property, 2,000 streams of 32 events per layout and platform
// (input_stress_tests runs 50,000).
class ShortcutMatcherProperty : public ::testing::TestWithParam<std::tuple<SimulatedLayoutId, KeyPlatform>> {};

TEST_P(ShortcutMatcherProperty, RandomStreams) {
    const auto [layout, platform] = GetParam();
    test::PropertyStats stats;
    test::runMatcherProperty(simulatedLayout(layout), platform, 0x5EED0000U + static_cast<unsigned>(layout), 2000, 32,
                             stats);
    EXPECT_GE(stats.events, 2000U * 32U);
    EXPECT_GT(stats.fires, 0U);
    EXPECT_GT(stats.pendings, 0U);
    EXPECT_GT(stats.sequenceFires, 0U);
    EXPECT_GT(stats.gestureFires, 0U);
    EXPECT_GT(stats.consumed, 0U);
    EXPECT_GT(stats.imeEvents, 0U);
}

INSTANTIATE_TEST_SUITE_P(Layouts, ShortcutMatcherProperty,
                         ::testing::Combine(::testing::Values(SimulatedLayoutId::EnUs, SimulatedLayoutId::DeDe,
                                                              SimulatedLayoutId::PlPl, SimulatedLayoutId::FrFr),
                                            ::testing::Values(KeyPlatform::Windows, KeyPlatform::X11)),
                         [](const auto& info) {
                             std::string name(simulatedLayoutName(std::get<0>(info.param)));
                             std::erase(name, '-');
                             return name + (std::get<1>(info.param) == KeyPlatform::Windows ? "_Windows" : "_X11");
                         });

} // namespace
} // namespace frost::input
