// SPDX-License-Identifier: Apache-2.0
//
// ShortcutLayouts: the M1 exit suite for layout-aware shortcuts and the shortcut half of
// PLAN.md row I6. Parameterised over {de-DE, pl-PL, fr-FR} x {Windows, X11} (plus an en-US
// instance): SimulatedKeyboard is driven by the simulated truth table, the matcher sees
// the table the product reads on that platform (the same full table on Windows,
// linuxM1Layout() on X11), with the built-in IntelliJ keymap and every action enabled.
// No keyboard layout is switched and no real input is injected: the layouts are data.

#include "frost/input/keymap.hpp"
#include "frost/input/shortcut_matcher.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include "input/support/matcher_rig.hpp"
#include "input/support/synthetic_layouts.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace frost::input {
namespace {

using test::MatcherRig;
using test::Step;

using LayoutParam = std::tuple<SimulatedLayoutId, KeyPlatform>;

std::string paramName(const ::testing::TestParamInfo<LayoutParam>& info) {
    std::string name(simulatedLayoutName(std::get<0>(info.param)));
    std::erase(name, '-');
    return name + (std::get<1>(info.param) == KeyPlatform::Windows ? "_Windows" : "_X11");
}

// The physical keys that select `level` (SimulatedKeyboard's AltGr is AltRight).
std::vector<Code> levelKeys(Level level) {
    switch (level) {
    case Level::Base: return {};
    case Level::Shift: return {Code::ShiftLeft};
    case Level::AltGr: return {Code::AltRight};
    case Level::AltGrShift: return {Code::AltRight, Code::ShiftLeft};
    }
    return {};
}

bool printable(char32_t c) { return c >= 0x20 && c != 0x7F; }

std::u32string typed(const Step& step, KeyPlatform platform) {
    return std::u32string(typedText(step.keyEvent, platform));
}

// Presses `held`, taps `key`, releases `held`, through the rig.
Step chordOf(MatcherRig& rig, const std::vector<Code>& held, Code key) {
    std::vector<KeyEvent> events;
    for (const Code c : held) {
        const auto down = rig.keyboard.press(c);
        events.insert(events.end(), down.begin(), down.end());
    }
    const auto tapped = rig.keyboard.tap(key);
    events.insert(events.end(), tapped.begin(), tapped.end());
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        const auto up = rig.keyboard.release(*it);
        events.insert(events.end(), up.begin(), up.end());
    }
    Step step = rig.feedAll(std::move(events));
    for (std::size_t i = 0; i < step.events.size(); ++i) {
        if (step.events[i].code == key && step.events[i].isPress()) {
            step.keyEvent = step.events[i];
            step.keyResult = step.results[i];
            break;
        }
    }
    return step;
}

void expectAllPassThrough(const Step& step, std::string_view what) {
    for (std::size_t i = 0; i < step.results.size(); ++i) {
        EXPECT_EQ(step.results[i].outcome, MatchOutcome::PassThrough)
            << what << ", event " << i << " (" << codeName(step.events[i].code) << ")";
    }
}

bool contains(std::span<const ActionId> ids, ActionId id) { return std::ranges::find(ids, id) != ids.end(); }

// Where a character is typed on `truth`: (code, level), preferring lower levels.
std::optional<std::pair<Code, Level>> sourceOf(const LayoutTable& truth, char32_t c, bool dead) {
    if (c == U' ' && !dead) {
        return std::pair{Code::Space, Level::Base};
    }
    for (unsigned level = 0; level < 4; ++level) {
        for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
            const KeyLevels l = truth.levels(static_cast<Code>(i));
            if (l.at(static_cast<Level>(level)) == c && l.dead(static_cast<Level>(level)) == dead) {
                return std::pair{static_cast<Code>(i), static_cast<Level>(level)};
            }
        }
    }
    return std::nullopt;
}

class ShortcutLayouts : public ::testing::TestWithParam<LayoutParam> {
protected:
    [[nodiscard]] SimulatedLayoutId layoutId() const { return std::get<0>(GetParam()); }
    [[nodiscard]] KeyPlatform platform() const { return std::get<1>(GetParam()); }
    [[nodiscard]] const LayoutTable& truth() const { return simulatedLayout(layoutId()); }
    [[nodiscard]] bool windows() const { return platform() == KeyPlatform::Windows; }

    // The action a fresh keyboard fires for `mods` + `code` ("" for none).
    std::string fire(MatcherRig& rig, Modifiers mods, Code code) {
        rig.resetKeyboard();
        return rig.stroke(mods, code).fired;
    }
};

// (a) Every character and dead key of every writing key, and every composition, types
// text and never fires a shortcut.
TEST_P(ShortcutLayouts, TypingNeverFiresShortcuts) {
    MatcherRig rig(truth(), platform());
    std::size_t characters = 0;
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const auto code = static_cast<Code>(i);
        const KeyLevels levels = truth().levels(code);
        for (unsigned l = 0; l < 4; ++l) {
            const auto level = static_cast<Level>(l);
            const char32_t c = levels.at(level);
            if (c == 0) {
                continue;
            }
            rig.resetKeyboard();
            const Step step = chordOf(rig, levelKeys(level), code);
            const std::string what = std::format("{} level {}", codeName(code), l);
            expectAllPassThrough(step, what);
            if (levels.dead(level)) {
                EXPECT_EQ(step.keyEvent.deadKey, c) << what;
                EXPECT_TRUE(typed(step, platform()).empty()) << what;
            } else if (printable(c)) {
                EXPECT_EQ(typed(step, platform()), std::u32string(1, c)) << what;
            }
            ++characters;
        }
    }
    EXPECT_GT(characters, 95U);

    std::size_t compositions = 0;
    for (const Composition& comp : truth().compositions()) {
        const auto dead = sourceOf(truth(), comp.dead, true);
        const auto base = sourceOf(truth(), comp.base, false);
        ASSERT_TRUE(dead.has_value()) << "no dead key for U+" << std::hex << static_cast<unsigned>(comp.dead);
        ASSERT_TRUE(base.has_value()) << "no key for U+" << std::hex << static_cast<unsigned>(comp.base);
        rig.resetKeyboard();
        const Step first = chordOf(rig, levelKeys(dead->second), dead->first);
        const Step second = chordOf(rig, levelKeys(base->second), base->first);
        const std::string what = std::format("composition U+{:04X} U+{:04X}", static_cast<unsigned>(comp.dead),
                                             static_cast<unsigned>(comp.base));
        expectAllPassThrough(first, what);
        expectAllPassThrough(second, what);
        EXPECT_EQ(typed(second, platform()), std::u32string(1, comp.composed)) << what;
        ++compositions;
    }
    EXPECT_EQ(compositions, truth().compositions().size());
}

// (b) AltGr characters type even where their Ctrl+Alt reading is bound; the golden list
// proves the collisions exist, so the check is not vacuous.
TEST_P(ShortcutLayouts, AltGrTypesTextOverCtrlAltBindings) {
    MatcherRig rig(truth(), platform());
    std::vector<std::string> collisions;
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const auto code = static_cast<Code>(i);
        for (const Level level : {Level::AltGr, Level::AltGrShift}) {
            if (truth().levels(code).at(level) == 0) {
                continue;
            }
            rig.resetKeyboard();
            const Step step = chordOf(rig, levelKeys(level), code);
            expectAllPassThrough(step, std::format("AltGr {}", codeName(code)));
            EXPECT_TRUE(step.keyEvent.altGr);
            const Key key = rig.table->logicalKey(code);
            if (key == Key::None) {
                continue;
            }
            Modifiers mods = Modifier::Ctrl | Modifier::Alt;
            if (level == Level::AltGrShift) {
                mods = mods.with(Modifier::Shift);
            }
            const Stroke reading{mods, key};
            for (const ActionId id : rig.index.single(reading)) {
                collisions.push_back(formatStroke(reading) + " " + rig.name(id));
            }
        }
    }
    std::ranges::sort(collisions);
    std::vector<std::string> expected;
    switch (layoutId()) {
    case SimulatedLayoutId::PlPl:
        expected = {"Ctrl+Alt+C refactor.extractConstant",
                    "Ctrl+Alt+L code.reformat",
                    "Ctrl+Alt+N refactor.inline",
                    "Ctrl+Alt+O code.optimizeImports",
                    "Ctrl+Alt+S file.settings",
                    "Ctrl+Alt+Shift+L code.reformatFile",
                    "Ctrl+Alt+Shift+N navigate.gotoSymbol",
                    "Ctrl+Alt+Shift+S file.projectStructure",
                    "Ctrl+Alt+Z vcs.rollback"};
        break;
    case SimulatedLayoutId::DeDe: expected = {"Ctrl+Alt+M refactor.extractMethod"}; break;
    case SimulatedLayoutId::FrFr: // None in the IntelliJ keymap; see ShortcutLayoutsSynthetic.
    case SimulatedLayoutId::EnUs: break;
    }
    EXPECT_EQ(collisions, expected);
}

// (c) Physical Ctrl+Alt(+Shift) bindings fire through the left keys, including where the
// Windows simulator also produced AltGr text (pl LCtrl+LAlt+L types 'ł').
TEST_P(ShortcutLayouts, CtrlAltBindingsFireWithLeftKeys) {
    MatcherRig rig(truth(), platform());
    std::size_t fired = 0;
    for (const Binding& binding : rig.keymap.bindings(test::keymapPlatformFor(platform()))) {
        for (const Sequence& s : binding.sequences) {
            const Stroke stroke = s.first();
            if (s.length() != 1 || !stroke.mods.has(Modifier::Ctrl) || !stroke.mods.has(Modifier::Alt) ||
                stroke.mods.has(Modifier::Meta)) {
                continue;
            }
            const auto code = test::physicalCodeFor(stroke.key, truth(), platform(), stroke.mods.has(Modifier::Shift));
            if (!code) {
                continue; // Unreachable bindings are EveryBindingReachableOrListed's subject.
            }
            rig.resetKeyboard();
            const Step step = rig.stroke(stroke.mods, *code);
            ASSERT_EQ(step.keyResult.outcome, MatchOutcome::Fire) << formatSequence(s) << " on " << codeName(*code);
            EXPECT_TRUE(contains(rig.index.single(stroke), step.keyResult.action)) << formatSequence(s);
            ++fired;
        }
    }
    EXPECT_GT(fired, 20U);
    EXPECT_EQ(fire(rig, Modifier::Ctrl | Modifier::Alt, Code::KeyL), "code.reformat");
    EXPECT_EQ(fire(rig, Modifier::Ctrl | Modifier::Alt | Modifier::Shift, Code::KeyL), "code.reformatFile");
}

// (d) AltGr on a key whose AltGr level is empty acts as Ctrl+Alt. A fact about the
// simulated model: real X11 German types 'ł' on AltGr+L.
TEST_P(ShortcutLayouts, AltGrWithoutCharacterActsAsCtrlAlt) {
    MatcherRig rig(truth(), platform());
    if (!truth().hasAltGr()) {
        // Right Alt is plain Alt on layouts without AltGr.
        rig.resetKeyboard();
        const Step step = chordOf(rig, {Code::AltRight}, Code::F7);
        EXPECT_FALSE(step.keyEvent.altGr);
        EXPECT_EQ(strokeFromEvent(step.keyEvent, *rig.table), (Stroke{Modifier::Alt, Key::F7}));
        return;
    }
    std::size_t fired = 0;
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const auto code = static_cast<Code>(i);
        if (truth().levels(code).altGr != 0 || rig.table->levels(code).altGr != 0) {
            continue;
        }
        const Key key = rig.table->logicalKey(code);
        const Stroke reading{Modifier::Ctrl | Modifier::Alt, key};
        if (key == Key::None || rig.index.single(reading).empty()) {
            continue;
        }
        rig.resetKeyboard();
        const Step step = chordOf(rig, {Code::AltRight}, code);
        EXPECT_EQ(step.keyResult.outcome, MatchOutcome::Fire) << codeName(code);
        EXPECT_TRUE(contains(rig.index.single(reading), step.keyResult.action)) << codeName(code);
        ++fired;
    }
    EXPECT_GT(fired, 5U);
    rig.resetKeyboard();
    const Step f7 = chordOf(rig, {Code::AltRight}, Code::F7);
    EXPECT_EQ(f7.fired, "navigate.showUsages");
    if (layoutId() == SimulatedLayoutId::DeDe) {
        rig.resetKeyboard();
        EXPECT_EQ(chordOf(rig, {Code::AltRight}, Code::KeyL).fired, "code.reformat");
    }

    // AltGr+Shift with an empty AltGr+Shift level is Ctrl+Alt+Shift (no hint under AltGr).
    std::size_t firedShift = 0;
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const auto code = static_cast<Code>(i);
        if (truth().levels(code).altGrShift != 0 || rig.table->levels(code).altGrShift != 0) {
            continue;
        }
        const Key key = rig.table->logicalKey(code);
        const Stroke reading{Modifier::Ctrl | Modifier::Alt | Modifier::Shift, key};
        if (key == Key::None || rig.index.single(reading).empty()) {
            continue;
        }
        rig.resetKeyboard();
        const Step step = chordOf(rig, {Code::AltRight, Code::ShiftLeft}, code);
        EXPECT_EQ(step.keyResult.outcome, MatchOutcome::Fire) << "AltGr+Shift+" << codeName(code);
        EXPECT_TRUE(contains(rig.index.single(reading), step.keyResult.action)) << codeName(code);
        ++firedShift;
    }
    EXPECT_GT(firedShift, 0U);
    if (layoutId() == SimulatedLayoutId::DeDe) {
        ASSERT_EQ(truth().levels(Code::KeyL).altGrShift, 0U);
        rig.resetKeyboard();
        EXPECT_EQ(chordOf(rig, {Code::AltRight, Code::ShiftLeft}, Code::KeyL).fired, "code.reformatFile")
            << "not code.reformat: Shift stays";
    }
    // AltGr+Meta keeps Meta: Ctrl+Alt+Meta.
    rig.resetKeyboard();
    const Step meta = chordOf(rig, {Code::AltRight, Code::MetaLeft}, Code::F7);
    EXPECT_TRUE(meta.keyEvent.altGr);
    EXPECT_EQ(strokeFromEvent(meta.keyEvent, *rig.table),
              (Stroke{Modifier::Ctrl | Modifier::Alt | Modifier::Meta, Key::F7}));
}

// Every single-stroke binding of `rig`'s keymap fires from the physical key the product maps
// to its logical key; returns the bindings that have no such key (sorted, unique).
std::vector<std::string> unreachableSingleStrokes(MatcherRig& rig) {
    std::vector<std::string> unreachable;
    std::size_t checked = 0;
    for (const Binding& binding : rig.keymap.bindings(test::keymapPlatformFor(rig.platform))) {
        for (const Sequence& s : binding.sequences) {
            if (s.length() != 1) {
                continue;
            }
            const Stroke stroke = s.first();
            const auto code =
                test::physicalCodeFor(stroke.key, *rig.truth, rig.platform, stroke.mods.has(Modifier::Shift));
            if (!code) {
                unreachable.push_back(formatSequence(s));
                continue;
            }
            rig.resetKeyboard();
            const Step step = rig.stroke(stroke.mods, *code);
            if (!rig.index.prefixed(stroke).empty()) {
                EXPECT_EQ(step.keyResult.outcome, MatchOutcome::Pending) << formatSequence(s);
            } else {
                EXPECT_EQ(step.keyResult.outcome, MatchOutcome::Fire)
                    << formatSequence(s) << " (" << binding.actionId << ") on " << codeName(*code);
                EXPECT_TRUE(contains(rig.index.single(stroke), step.keyResult.action)) << formatSequence(s);
            }
            ++checked;
        }
    }
    EXPECT_GT(checked, 200U);
    std::ranges::sort(unreachable);
    const auto [first, last] = std::ranges::unique(unreachable);
    unreachable.erase(first, last);
    return unreachable;
}

// The single-stroke bindings KeymapService reports as unreachable on the rig's table (sorted).
std::vector<std::string> reportedUnreachable(const MatcherRig& rig) {
    std::vector<std::string> reported;
    for (const Sequence& s : rig.index.unreachable(*rig.table)) {
        if (s.length() == 1) {
            reported.push_back(formatSequence(s));
        }
    }
    std::ranges::sort(reported);
    return reported;
}

// (e) Every single-stroke binding fires from the physical key the product maps to its
// logical key: since rule 3.6 (ADR 0030) none is unreachable on any simulated layout and
// platform (de-DE Ctrl+Slash is the '-' key). ShortcutLayoutsSynthetic keeps the listed case.
TEST_P(ShortcutLayouts, EveryBindingReachableOrListed) {
    MatcherRig rig(truth(), platform());
    EXPECT_EQ(unreachableSingleStrokes(rig), std::vector<std::string>{});
    if (windows()) {
        // On Windows the table is the whole truth: KeymapService reports the same (empty) list.
        EXPECT_EQ(reportedUnreachable(rig), std::vector<std::string>{});
    }
}

// (f) The named examples of the slice spec.
TEST_P(ShortcutLayouts, NamedExamples) {
    MatcherRig rig(truth(), platform());
    const Modifiers ctrl = Modifier::Ctrl;
    const Modifiers ctrlShift = Modifier::Ctrl | Modifier::Shift;
    switch (layoutId()) {
    case SimulatedLayoutId::DeDe: {
        EXPECT_EQ(fire(rig, ctrl, Code::KeyY), "editor.undo");
        EXPECT_EQ(fire(rig, ctrl, Code::KeyZ), "editor.deleteLine");
        EXPECT_EQ(fire(rig, ctrl, Code::BracketLeft), "editor.codeBlockStart");
        EXPECT_EQ(fire(rig, ctrl, Code::NumpadDivide), "editor.commentLine");
        // The key labelled '-' (US '/' position) is Slash, as JetBrains documents for its
        // IDEs' Linux mapping ("to execute Alt + / on a German keyboard, press Alt + -",
        // Rider Help, Keymap, "Use national layout for shortcuts"); 'ß' (US '-') is Minus.
        // Rule 3.6, ADR 0030; the same on Windows and Linux.
        EXPECT_EQ(fire(rig, ctrl, Code::Slash), "editor.commentLine");
        EXPECT_EQ(fire(rig, ctrlShift, Code::Slash), "editor.commentBlock");
        EXPECT_EQ(fire(rig, ctrl, Code::Minus), "editor.foldCollapse");
        EXPECT_EQ(fire(rig, ctrlShift, Code::Minus), "editor.foldCollapseAll");
        EXPECT_EQ(fire(rig, ctrl, Code::NumpadSubtract), "editor.foldCollapse");
        if (windows()) {
            const auto shown = [&](std::string_view text) {
                return toDisplayText(*parseSequence(text, SyntaxMode::Strict), *rig.table, KeymapPlatform::Windows);
            };
            EXPECT_EQ(shown("Ctrl+Slash"), "Ctrl+-");
            EXPECT_EQ(shown("Ctrl+Shift+Slash"), "Ctrl+Shift+-");
            EXPECT_EQ(shown("Ctrl+Minus"), "Ctrl+ß");
        }
        rig.resetKeyboard();
        const Step caret = rig.stroke(ctrl, Code::Backquote);
        EXPECT_EQ(caret.fired, "view.quickSwitchScheme");
        EXPECT_EQ(caret.keyEvent.deadKey, U'^') << "reported, not composed";
        EXPECT_EQ(rig.keyboard.pendingDeadKey(), 0U);
        EXPECT_EQ(std::u32string(rig.tap(Code::KeyE).keyEvent.textView()), U"e");
        break;
    }
    case SimulatedLayoutId::FrFr: {
        EXPECT_EQ(fire(rig, ctrlShift, Code::KeyQ), "search.findAction");
        EXPECT_EQ(fire(rig, Modifier::Alt, Code::Digit1), "window.project");
        EXPECT_EQ(fire(rig, ctrlShift, Code::Digit1), "bookmarks.toggle1");
        EXPECT_EQ(fire(rig, ctrl, Code::Slash), "editor.commentLine");
        EXPECT_EQ(fire(rig, ctrl, Code::KeyZ), "editor.extendSelection");
        rig.resetKeyboard();
        const Step m = rig.stroke(ctrl, Code::KeyM);
        rig.resetKeyboard();
        const Step comma = rig.stroke(ctrl, Code::Comma);
        // KeyM types ',' ('?' with Shift): it is Comma on both platforms (on Linux through the
        // ',' hint), and never the letter M, which is the key typing 'm' (Semicolon).
        EXPECT_EQ(strokeFromEvent(m.keyEvent, *rig.table), (Stroke{ctrl, Key::Comma}));
        EXPECT_EQ(strokeFromEvent(comma.keyEvent, *rig.table), (Stroke{ctrl, windows() ? Key::Semicolon : Key::Comma}));
        const Modifiers ctrlAlt = Modifier::Ctrl | Modifier::Alt;
        EXPECT_EQ(fire(rig, ctrlAlt, Code::KeyM), "") << "Ctrl+Alt+',' is not Ctrl+Alt+M";
        EXPECT_EQ(fire(rig, ctrlShift, Code::KeyM), "") << "Ctrl+Shift+'?' is not Ctrl+Shift+M";
        EXPECT_EQ(fire(rig, ctrlAlt, Code::Semicolon), "refactor.extractMethod");
        EXPECT_EQ(fire(rig, ctrlShift, Code::Semicolon), "navigate.matchingBrace");
        rig.resetKeyboard();
        const Step question = rig.stroke(ctrlShift, Code::KeyM);
        EXPECT_EQ(strokeFromEvent(question.keyEvent, *rig.table),
                  windows() ? std::optional<Stroke>(Stroke{ctrlShift, Key::Comma}) : std::nullopt)
            << "Linux sees only the Shift level's '?', which names no key";
        break;
    }
    case SimulatedLayoutId::PlPl: {
        rig.resetKeyboard();
        const Step altGr = chordOf(rig, {Code::AltRight}, Code::KeyS);
        expectAllPassThrough(altGr, "AltGr+S");
        EXPECT_EQ(typed(altGr, platform()), U"ś");
        EXPECT_EQ(fire(rig, Modifier::Ctrl | Modifier::Alt, Code::KeyS), "file.settings");
        break;
    }
    case SimulatedLayoutId::EnUs: {
        // Right Alt is plain Alt on en-US, so RAlt+Ctrl+L is Ctrl+Alt+L.
        rig.resetKeyboard();
        EXPECT_EQ(chordOf(rig, {Code::ControlLeft, Code::AltRight}, Code::KeyL).fired, "code.reformat");
        break;
    }
    }
}

// (g) Dead keys compose, and a shortcut after a dead key fires and drops the composition.
TEST_P(ShortcutLayouts, DeadKeys) {
    MatcherRig rig(truth(), platform());
    const auto typeSequence = [&](std::vector<std::pair<std::vector<Code>, Code>> keys) {
        rig.resetKeyboard();
        std::u32string text;
        for (const auto& [held, code] : keys) {
            const Step step = chordOf(rig, held, code);
            expectAllPassThrough(step, codeName(code));
            text += typed(step, platform());
        }
        return text;
    };
    const std::vector<Code> none;
    const std::vector<Code> shift = {Code::ShiftLeft};
    const std::vector<Code> altGr = {Code::AltRight};
    std::optional<std::pair<std::vector<Code>, Code>> deadKey;
    switch (layoutId()) {
    case SimulatedLayoutId::DeDe:
        EXPECT_EQ(typeSequence({{none, Code::Backquote}, {none, Code::KeyE}}), U"ê");
        EXPECT_EQ(typeSequence({{none, Code::Backquote}, {none, Code::Space}}), U"^");
        EXPECT_EQ(typeSequence({{none, Code::Backquote}, {none, Code::Backquote}}), U"^^");
        EXPECT_EQ(typeSequence({{none, Code::Backquote}, {none, Code::KeyX}}), U"^x");
        deadKey = std::pair{none, Code::Backquote};
        break;
    case SimulatedLayoutId::FrFr:
        EXPECT_EQ(typeSequence({{none, Code::BracketLeft}, {none, Code::KeyE}}), U"ê");
        EXPECT_EQ(typeSequence({{altGr, Code::Digit2}, {none, Code::KeyN}}), U"ñ");
        deadKey = std::pair{none, Code::BracketLeft};
        break;
    case SimulatedLayoutId::PlPl:
        EXPECT_EQ(typeSequence({{shift, Code::Backquote}, {none, Code::KeyA}}), U"ą");
        deadKey = std::pair{shift, Code::Backquote};
        break;
    case SimulatedLayoutId::EnUs: EXPECT_TRUE(truth().compositions().empty()); return;
    }
    ASSERT_TRUE(deadKey.has_value());
    rig.resetKeyboard();
    static_cast<void>(chordOf(rig, deadKey->first, deadKey->second));
    EXPECT_NE(rig.keyboard.pendingDeadKey(), 0U);
    EXPECT_EQ(rig.stroke(Modifier::Ctrl, Code::KeyS).fired, "file.saveAll");
    EXPECT_EQ(rig.keyboard.pendingDeadKey(), 0U) << "the shortcut drops the composition";
    EXPECT_EQ(std::u32string(rig.tap(Code::KeyE).keyEvent.textView()), U"e");
}

// (h) DoubleShift fires search.everywhere on each layout, but not across a dead key or IME.
TEST_P(ShortcutLayouts, DoubleShift) {
    MatcherRig rig(truth(), platform());
    static_cast<void>(rig.tap(Code::ShiftLeft));
    EXPECT_EQ(rig.tap(Code::ShiftLeft).fired, "search.everywhere");

    const auto dead = sourceOf(truth(), U'^', true).or_else([&] { return sourceOf(truth(), U'~', true); });
    rig.resetKeyboard();
    static_cast<void>(rig.tap(Code::ShiftLeft));
    if (dead) {
        static_cast<void>(chordOf(rig, levelKeys(dead->second), dead->first));
    } else {
        static_cast<void>(rig.tap(Code::KeyE)); // en-US has no dead key: any key between the taps.
    }
    EXPECT_EQ(rig.tap(Code::ShiftLeft).fired, "") << "a key between the taps";

    rig.resetKeyboard();
    rig.keyboard.setImeComposing(true);
    static_cast<void>(rig.tap(Code::ShiftLeft));
    EXPECT_EQ(rig.tap(Code::ShiftLeft).fired, "") << "while composing";
}

INSTANTIATE_TEST_SUITE_P(Layouts, ShortcutLayouts,
                         ::testing::Combine(::testing::Values(SimulatedLayoutId::DeDe, SimulatedLayoutId::PlPl,
                                                              SimulatedLayoutId::FrFr),
                                            ::testing::Values(KeyPlatform::Windows, KeyPlatform::X11)),
                         paramName);
INSTANTIATE_TEST_SUITE_P(EnUs, ShortcutLayouts,
                         ::testing::Combine(::testing::Values(SimulatedLayoutId::EnUs),
                                            ::testing::Values(KeyPlatform::Windows, KeyPlatform::X11)),
                         paramName);

// (e, listed) On a layout with no '/' (the old de-DE gap, synthetic since rule 3.6) both
// comment shortcuts are unreachable on Windows, KeymapService lists the same two, and the
// numpad alternatives still fire.
TEST(ShortcutLayoutsSynthetic, UnreachableListedOnNoSlash) {
    const LayoutTable noSlash = test::synthetic::noSlash();
    MatcherRig rig(noSlash, KeyPlatform::Windows);
    const std::vector<std::string> expected = {"Ctrl+Shift+Slash", "Ctrl+Slash"};
    EXPECT_EQ(unreachableSingleStrokes(rig), expected);
    EXPECT_EQ(reportedUnreachable(rig), expected);
    rig.resetKeyboard();
    EXPECT_EQ(rig.stroke(Modifier::Ctrl, Code::NumpadDivide).fired, "editor.commentLine");
    rig.resetKeyboard();
    EXPECT_EQ(rig.stroke(Modifier::Ctrl, Code::Slash).fired, "editor.foldCollapse") << "'-' is Minus here";
}

// (b, fr) French AltGr characters against a synthetic keymap that binds their Ctrl+Alt reading.
TEST(ShortcutLayoutsSynthetic, FrenchAltGrTypesTextOverCtrlAltBindings) {
    constexpr std::string_view kKeymap = "frost-keymap 1\nid synthetic\nname Synthetic\n[default]\n"
                                         "test.at = Ctrl+Alt+Digit0\n"
                                         "test.hash = Ctrl+Alt+Digit3\n"
                                         "test.euro = Ctrl+Alt+E\n";
    for (const KeyPlatform platform : {KeyPlatform::Windows, KeyPlatform::X11}) {
        MatcherRig rig(simulatedLayout(SimulatedLayoutId::FrFr), platform, kKeymap);
        const std::array<std::pair<Code, std::u32string_view>, 3> cases = {
            {{Code::Digit0, U"@"}, {Code::Digit3, U"#"}, {Code::KeyE, U"€"}}};
        for (const auto& [code, text] : cases) {
            rig.resetKeyboard();
            const Step step = chordOf(rig, {Code::AltRight}, code);
            expectAllPassThrough(step, codeName(code));
            EXPECT_EQ(typed(step, platform), text);
            EXPECT_FALSE(rig.index.single(Stroke{Modifier::Ctrl | Modifier::Alt, rig.table->logicalKey(code)}).empty())
                << "the collision exists";
            // The same key with the physical left keys is the shortcut.
            rig.resetKeyboard();
            EXPECT_FALSE(rig.stroke(Modifier::Ctrl | Modifier::Alt, code).fired.empty()) << codeName(code);
        }
    }
}

} // namespace
} // namespace frost::input
