// SPDX-License-Identifier: Apache-2.0
//
// MatcherRig: the shared harness of the matcher suites (ShortcutMatcher,
// ShortcutSequences, ShortcutLayouts, DoubleShift, the stress property). It wires a
// ManualClock, a SimulatedKeyboard on a truth layout, the table the product would read on
// that platform, a keymap with stub actions, its BindingIndex and a ShortcutMatcher, and
// feeds the simulated events through the matcher. Also the random-stream matcher property
// shared by input_tests and input_stress_tests.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/input/action.hpp"
#include "frost/input/key_event.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/layout.hpp"
#include "frost/input/shortcut_matcher.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include "input/support/test_actions.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::input::test {

// The events of one rig call and the matcher's results for them.
struct Step {
    std::vector<KeyEvent> events;
    std::vector<MatchResult> results;
    // The press of the call's main key and its result (chord, press and tap).
    KeyEvent keyEvent{};
    MatchResult keyResult{};
    // The id of the first action fired anywhere in the step ("" for none), and its result.
    std::string fired;
    MatchResult fire{};
};

// The keymap platform a key platform resolves keymaps for.
[[nodiscard]] KeymapPlatform keymapPlatformFor(KeyPlatform platform) noexcept;
// The table the product's matcher sees on `platform`: `truth` on Windows, linuxM1Layout() elsewhere.
[[nodiscard]] const LayoutTable& productTable(const LayoutTable& truth, KeyPlatform platform) noexcept;
// The keyCharHint SimulatedKeyboard reports for `code` without AltGr.
[[nodiscard]] char32_t simulatedHint(const LayoutTable& truth, KeyPlatform platform, Code code) noexcept;
// The first code (in Code order, modifiers and locks excluded) whose press the product
// table maps to `key`, with the simulator's hint; nullopt when the key is unreachable.
[[nodiscard]] std::optional<Code> physicalCodeFor(Key key, const LayoutTable& truth, KeyPlatform platform);
// The left modifier keys of a modifier set, in the order Ctrl, Alt, Shift, Meta.
[[nodiscard]] std::vector<Code> modifierCodes(Modifiers mods);

class MatcherRig {
public:
    // `keymap` is the built-in IntelliJ keymap when empty. Every action it names is
    // registered (test_actions.hpp), plus `extraActions`. Overrides apply to the index.
    explicit MatcherRig(const LayoutTable& truth = simulatedLayout(SimulatedLayoutId::EnUs),
                        KeyPlatform platform = KeyPlatform::Windows, std::string_view keymapText = {},
                        std::vector<ActionOverride> overrides = {}, std::vector<std::string> extraActions = {});

    MatcherRig(const MatcherRig&) = delete ("the rig hands out pointers to its members");
    MatcherRig& operator=(const MatcherRig&) = delete ("the rig hands out pointers to its members");

    // Feeds one event (or a batch) through the matcher with the current gate.
    MatchResult feed(const KeyEvent& event);
    Step feedAll(std::vector<KeyEvent> events);

    // Simulated keyboard calls, fed through the matcher. keyEvent/keyResult are the press of `key`.
    Step press(Code key);
    Step release(Code key);
    Step tap(Code key);
    Step chord(std::initializer_list<Code> held, Code key);
    // Presses the left modifier keys of `mods`, taps `key`, releases them.
    Step stroke(Modifiers mods, Code key);

    // A fresh keyboard (no keys down, no pending dead key) and a reset matcher.
    void resetKeyboard();

    // The string id of `id` ("" for kNoAction).
    [[nodiscard]] std::string name(ActionId id) const;
    // The registered id for `id`; fails the test when it is missing.
    [[nodiscard]] ActionId idOf(std::string_view id) const;

    ManualClock clock;
    ActionRegistry registry;
    InvocationLog log;
    Keymap keymap;
    KeyPlatform platform;
    const LayoutTable* truth;
    const LayoutTable* table;
    BindingIndex index;
    SimulatedKeyboard keyboard;
    ShortcutMatcher matcher;
    // The gate; every action passes by default.
    std::function<bool(ActionId)> enabled = [](ActionId) { return true; };
};

// Counters of one property run, so callers can assert the streams were not vacuous.
struct PropertyStats {
    std::size_t events = 0;
    std::size_t fires = 0;
    std::size_t sequenceFires = 0;
    std::size_t gestureFires = 0;
    std::size_t pendings = 0;
    std::size_t consumed = 0;
    std::size_t imeEvents = 0;
};

/*
 * The matcher property over `streams` seeded random streams of `length` events on
 * (truth, platform), mixing simulated key activity, raw random events, IME toggles,
 * reset() calls and time jumps, with a random gate per stream. Checks, per event: peek()
 * equals feed().outcome and changes no state; Fire names an action bound to the fired
 * sequence, and for single strokes strokeFromEvent agrees; IME events PassThrough; a
 * Pending result is followed by Fire or Consumed on the next non-modifier, non-IME press;
 * reset() clears the pending stroke. Failures are reported with gtest assertions (the
 * run stops at the first failing stream).
 */
void runMatcherProperty(const LayoutTable& truth, KeyPlatform platform, std::uint64_t seed, std::size_t streams,
                        std::size_t length, PropertyStats& stats);

} // namespace frost::input::test
