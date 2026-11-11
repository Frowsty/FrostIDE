// SPDX-License-Identifier: Apache-2.0
//
// MatcherRig and the random-stream matcher property (matcher_rig.hpp).

#include "input/support/matcher_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <random>
#include <utility>

namespace frost::input::test {

KeymapPlatform keymapPlatformFor(KeyPlatform platform) noexcept {
    return platform == KeyPlatform::Windows ? KeymapPlatform::Windows : KeymapPlatform::Linux;
}

const LayoutTable& productTable(const LayoutTable& truth, KeyPlatform platform) noexcept {
    // NOLINTNEXTLINE(bugprone-return-const-ref-from-parameter): The oracle outlives MatcherRig.
    return platform == KeyPlatform::Windows ? truth : linuxM1Layout();
}

char32_t simulatedHint(const LayoutTable& truth, KeyPlatform platform, Code code, bool shift) noexcept {
    return platform == KeyPlatform::Windows ? 0 : simulatedKeyCharHint(truth, code, shift);
}

std::optional<Code> physicalCodeFor(Key key, const LayoutTable& truth, KeyPlatform platform, bool shift) {
    const LayoutTable& table = productTable(truth, platform);
    for (std::size_t i = 1; i < kCodeCount; ++i) {
        const auto code = static_cast<Code>(i);
        if (isModifierKey(code) || code == Code::CapsLock || code == Code::NumLock || code == Code::ScrollLock) {
            continue;
        }
        if (table.logicalKey(code, simulatedHint(truth, platform, code, shift)) == key) {
            return code;
        }
    }
    return std::nullopt;
}

std::vector<Code> modifierCodes(Modifiers mods) {
    std::vector<Code> codes;
    if (mods.has(Modifier::Ctrl)) {
        codes.push_back(Code::ControlLeft);
    }
    if (mods.has(Modifier::Alt)) {
        codes.push_back(Code::AltLeft);
    }
    if (mods.has(Modifier::Shift)) {
        codes.push_back(Code::ShiftLeft);
    }
    if (mods.has(Modifier::Meta)) {
        codes.push_back(Code::MetaLeft);
    }
    return codes;
}

namespace {

Keymap keymapFrom(std::string_view text) {
    if (text.empty()) {
        return intellij();
    }
    auto keymap = parseKeymap(text);
    EXPECT_TRUE(keymap.has_value()) << keymap.error().message();
    return keymap ? std::move(*keymap) : Keymap{};
}

} // namespace

MatcherRig::MatcherRig(const LayoutTable& truthTable, KeyPlatform keyPlatform, std::string_view keymapText,
                       std::vector<ActionOverride> overrides, const std::vector<std::string>& extraActions)
    : keymap(keymapFrom(keymapText)), platform(keyPlatform), truth(&truthTable),
      table(&productTable(truthTable, keyPlatform)), keyboard(truthTable, keyPlatform, clock) {
    static_cast<void>(registerKeymapActions(registry, keymap, &log));
    for (const std::string& id : extraActions) {
        ActionSpec spec;
        spec.id = id;
        spec.name = id;
        spec.context = contextFor(id);
        spec.handler = [this, id](const ActionContext&, const InvokeInfo& info) { log.calls.push_back({id, info}); };
        EXPECT_TRUE(registry.add(std::move(spec)).has_value()) << id;
    }
    index = BindingIndex::build(keymap, keymapPlatformFor(platform), overrides, registry);
}

MatchResult MatcherRig::feed(const KeyEvent& event) {
    const auto gate = [this](ActionId id) { return enabled(id); };
    return matcher.feed(event, index, *table, gate);
}

Step MatcherRig::feedAll(std::vector<KeyEvent> events) {
    Step step;
    step.events = std::move(events);
    for (const KeyEvent& event : step.events) {
        const MatchResult result = feed(event);
        step.results.push_back(result);
        if (result.outcome == MatchOutcome::Fire && step.fired.empty()) {
            step.fired = name(result.action);
            step.fire = result;
        }
    }
    if (!step.events.empty()) {
        step.keyEvent = step.events.back();
        step.keyResult = step.results.back();
    }
    return step;
}

namespace {

// Points keyEvent/keyResult at the first press of `key` in the step.
void selectKey(Step& step, Code key) {
    for (std::size_t i = 0; i < step.events.size(); ++i) {
        if (step.events[i].code == key && step.events[i].isPress()) {
            step.keyEvent = step.events[i];
            step.keyResult = step.results[i];
            return;
        }
    }
}

} // namespace

Step MatcherRig::press(Code key) {
    Step step = feedAll(keyboard.press(key));
    selectKey(step, key);
    return step;
}

Step MatcherRig::release(Code key) { return feedAll(keyboard.release(key)); }

Step MatcherRig::tap(Code key) {
    std::vector<KeyEvent> events = keyboard.press(key);
    const std::vector<KeyEvent> up = keyboard.release(key);
    events.insert(events.end(), up.begin(), up.end());
    Step step = feedAll(std::move(events));
    selectKey(step, key);
    return step;
}

Step MatcherRig::chord(std::initializer_list<Code> held, Code key) {
    Step step = feedAll(keyboard.chord(held, key));
    selectKey(step, key);
    return step;
}

Step MatcherRig::stroke(Modifiers mods, Code key) {
    const std::vector<Code> held = modifierCodes(mods);
    std::vector<KeyEvent> events;
    for (const Code c : held) {
        const std::vector<KeyEvent> down = keyboard.press(c);
        events.insert(events.end(), down.begin(), down.end());
    }
    const std::vector<KeyEvent> tapped = keyboard.tap(key);
    events.insert(events.end(), tapped.begin(), tapped.end());
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        const std::vector<KeyEvent> up = keyboard.release(*it);
        events.insert(events.end(), up.begin(), up.end());
    }
    Step step = feedAll(std::move(events));
    selectKey(step, key);
    return step;
}

void MatcherRig::resetKeyboard() {
    keyboard = SimulatedKeyboard(*truth, platform, clock);
    matcher.reset();
}

std::string MatcherRig::name(ActionId id) const { return actionName(registry, id); }

ActionId MatcherRig::idOf(std::string_view id) const {
    const auto found = registry.find(id);
    EXPECT_TRUE(found.has_value()) << id;
    return found.value_or(kNoAction);
}

namespace {

// The codes the random streams press.
constexpr std::array kPool = {
    Code::ShiftLeft,   Code::ShiftRight, Code::ControlLeft,  Code::ControlRight, Code::AltLeft,   Code::AltRight,
    Code::MetaLeft,    Code::KeyA,       Code::KeyC,         Code::KeyK,         Code::KeyL,      Code::KeyS,
    Code::KeyY,        Code::KeyZ,       Code::KeyQ,         Code::KeyM,         Code::KeyE,      Code::KeyN,
    Code::Digit1,      Code::Digit2,     Code::Digit7,       Code::Digit0,       Code::Minus,     Code::Equal,
    Code::BracketLeft, Code::Backquote,  Code::Semicolon,    Code::Comma,        Code::Slash,     Code::IntlBackslash,
    Code::Space,       Code::Enter,      Code::Escape,       Code::Tab,          Code::Backspace, Code::F2,
    Code::F7,          Code::F12,        Code::ArrowUp,      Code::Home,         Code::Insert,    Code::Delete,
    Code::Numpad1,     Code::Numpad8,    Code::NumpadDivide, Code::NumpadEnter,  Code::KeyD,      Code::KeyW,
};

bool contains(std::span<const ActionId> ids, ActionId id) { return std::ranges::find(ids, id) != ids.end(); }

KeyEvent randomRawEvent(std::mt19937_64& rng, const IClock& clock) {
    KeyEvent e;
    e.time = clock.steadyNow();
    e.type = rng() % 3 == 0 ? KeyEventType::Release : KeyEventType::Press;
    e.code = static_cast<Code>(rng() % (kCodeCount + 3));
    e.modifiers = Modifiers::fromBits(static_cast<std::uint8_t>(rng() % 16));
    e.altGr = rng() % 5 == 0;
    e.autoRepeat = rng() % 6 == 0;
    e.numLock = rng() % 4 != 0;
    e.imeComposing = rng() % 10 == 0;
    e.osTimeMs = rng() % 4 == 0 ? 0 : static_cast<std::uint32_t>(rng());
    constexpr std::array<char32_t, 5> dead = {0, 0, 0, U'^', U'~'};
    e.deadKey = dead[rng() % dead.size()];
    constexpr std::array<char32_t, 6> text = {0, U'a', U'ł', 0x01, U'@', 0x1B};
    const char32_t t = text[rng() % text.size()];
    if (t != 0) {
        e.text[0] = t;
        e.textLength = 1;
    }
    e.keyCharHint = rng() % 4 == 0 ? static_cast<char32_t>(U'a' + rng() % 26) : 0;
    return e;
}

} // namespace

void runMatcherProperty(const LayoutTable& truth, KeyPlatform platform, std::uint64_t seed, std::size_t streams,
                        std::size_t length, PropertyStats& stats) {
    const std::vector<ActionOverride> overrides = {
        {"test.chordCopy", {*parseSequence("Ctrl+K, Ctrl+C", SyntaxMode::Strict)}},
        {"test.chordBare", {*parseSequence("Ctrl+K, C", SyntaxMode::Strict)}},
        {"test.chordAlt", {*parseSequence("Ctrl+Alt+K, L", SyntaxMode::Strict)}},
    };
    MatcherRig rig(truth, platform, {}, overrides, {"test.chordCopy", "test.chordBare", "test.chordAlt"});
    std::vector<bool> disabled(rig.registry.size() + 1, false);
    const auto gate = [&disabled](ActionId id) { return id.value() < disabled.size() && !disabled[id.value()]; };

    for (std::size_t s = 0; s < streams; ++s) {
        std::mt19937_64 rng(seed * 1'000'003U + s);
        SCOPED_TRACE(std::format("{} {} stream {} (seed {})", truth.id(),
                                 platform == KeyPlatform::Windows ? "Windows" : "X11", s, seed));
        rig.resetKeyboard();
        for (auto&& actionDisabled : disabled) {
            actionDisabled = rng() % 8 == 0;
        }
        bool composing = false;
        std::size_t produced = 0;
        while (produced < length) {
            std::vector<KeyEvent> events;
            const unsigned pick = static_cast<unsigned>(rng() % 100);
            if (pick < 30) {
                events = rig.keyboard.press(kPool[rng() % kPool.size()]);
            } else if (pick < 55) {
                std::vector<Code> held;
                for (const Code c : kPool) {
                    if (rig.keyboard.isDown(c)) {
                        held.push_back(c);
                    }
                }
                events = held.empty() ? rig.keyboard.press(kPool[rng() % kPool.size()])
                                      : rig.keyboard.release(held[rng() % held.size()]);
            } else if (pick < 70) {
                events = rig.keyboard.tap(kPool[7 + rng() % (kPool.size() - 7)]);
            } else if (pick < 75) {
                composing = !composing;
                rig.keyboard.setImeComposing(composing);
                continue;
            } else if (pick < 78) {
                rig.matcher.reset();
                EXPECT_FALSE(rig.matcher.pendingPrefix().has_value()) << "reset() clears the pending stroke";
                continue;
            } else if (pick < 85) {
                rig.clock.advance(std::chrono::milliseconds(rng() % 800));
                continue;
            } else if (pick < 90) {
                events.push_back(randomRawEvent(rng, rig.clock));
            } else if (pick < 92) {
                rig.keyboard.setNumLock(rng() % 2 == 0);
                continue;
            } else if (pick < 96) {
                // A directed attempt at one of the overridden sequences (random keys rarely
                // complete one), on the keys that carry those labels on the truth layout.
                const Code k = physicalCodeFor(Key::K, truth, platform).value_or(Code::KeyK);
                const Code c = physicalCodeFor(Key::C, truth, platform).value_or(Code::KeyC);
                const Code l = physicalCodeFor(Key::L, truth, platform).value_or(Code::KeyL);
                const bool alt = rng() % 3 == 0;
                events = alt ? rig.keyboard.chord({Code::ControlLeft, Code::AltLeft}, k)
                             : rig.keyboard.chord({Code::ControlLeft}, k);
                const std::vector<KeyEvent> second = alt              ? rig.keyboard.tap(l)
                                                     : rng() % 2 == 0 ? rig.keyboard.chord({Code::ControlLeft}, c)
                                                                      : rig.keyboard.tap(c);
                events.insert(events.end(), second.begin(), second.end());
            } else {
                const Code tapped = rng() % 2 == 0 ? Code::ShiftLeft : Code::ControlLeft;
                events = rig.keyboard.tap(tapped);
                rig.clock.advance(std::chrono::milliseconds(rng() % 300));
                const std::vector<KeyEvent> again = rig.keyboard.tap(rng() % 4 == 0 ? Code::ShiftRight : tapped);
                events.insert(events.end(), again.begin(), again.end());
            }

            for (const KeyEvent& e : events) {
                ++produced;
                ++stats.events;
                const ShortcutMatcher before = rig.matcher;
                const MatchOutcome peeked = rig.matcher.peek(e, rig.index, *rig.table, gate);
                ShortcutMatcher twin = before;
                const MatchResult expected = twin.feed(e, rig.index, *rig.table, gate);
                const MatchResult r = rig.matcher.feed(e, rig.index, *rig.table, gate);
                // peek() changed nothing: feeding after it gives what feeding a copy taken before it gives.
                ASSERT_EQ(r.outcome, expected.outcome);
                ASSERT_EQ(r.action, expected.action);
                ASSERT_EQ(r.sequence, expected.sequence);
                ASSERT_EQ(r.cancelledPending, expected.cancelledPending);
                ASSERT_EQ(rig.matcher.pendingPrefix(), twin.pendingPrefix());
                ASSERT_EQ(peeked, r.outcome) << toString(peeked) << " vs " << toString(r.outcome);

                if (e.imeComposing) {
                    ++stats.imeEvents;
                    ASSERT_EQ(r.outcome, MatchOutcome::PassThrough);
                    ASSERT_EQ(r.cancelledPending, before.pendingPrefix().has_value());
                    ASSERT_FALSE(rig.matcher.pendingPrefix().has_value());
                }
                const bool decisivePress = e.isPress() && !e.imeComposing && !isModifierKey(e.code);
                if (before.pendingPrefix() && decisivePress) {
                    ASSERT_TRUE(r.outcome == MatchOutcome::Fire || r.outcome == MatchOutcome::Consumed)
                        << "a pending stroke resolves on the next press, got " << toString(r.outcome);
                }
                switch (r.outcome) {
                case MatchOutcome::Fire: {
                    ++stats.fires;
                    ASSERT_NE(r.action, kNoAction);
                    ASSERT_TRUE(gate(r.action));
                    const Sequence& seq = r.sequence;
                    if (seq.isGesture()) {
                        ++stats.gestureFires;
                        ASSERT_TRUE(e.isRelease());
                        ASSERT_TRUE(contains(rig.index.gesture(seq.gesture()), r.action));
                    } else if (seq.length() == 1) {
                        ASSERT_TRUE(contains(rig.index.single(seq.first()), r.action)) << formatSequence(seq);
                        ASSERT_EQ(strokeFromEvent(e, *rig.table), seq.first()) << formatSequence(seq);
                    } else {
                        ++stats.sequenceFires;
                        ASSERT_TRUE(contains(rig.index.second(seq.first(), seq.second()), r.action))
                            << formatSequence(seq);
                        ASSERT_EQ(before.pendingPrefix(), seq.first());
                        ASSERT_EQ(strokeFromEvent(e, *rig.table), seq.second()) << formatSequence(seq);
                    }
                    ASSERT_FALSE(rig.matcher.pendingPrefix().has_value());
                    break;
                }
                case MatchOutcome::Pending:
                    ++stats.pendings;
                    ASSERT_TRUE(e.isPress());
                    ASSERT_FALSE(before.pendingPrefix().has_value());
                    ASSERT_EQ(rig.matcher.pendingPrefix(), strokeFromEvent(e, *rig.table));
                    break;
                case MatchOutcome::Consumed: ++stats.consumed; break;
                case MatchOutcome::PassThrough: break;
                }
            }
        }
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

} // namespace frost::input::test
