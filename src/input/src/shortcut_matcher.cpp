// SPDX-License-Identifier: Apache-2.0
//
// ShortcutMatcher (shortcut_matcher.hpp; ADR 0016 decisions 4-7): the feed algorithm and
// the double-tap detector.
/*
 * feed(e), in order:
 *   1. e.imeComposing: reset everything (cancelledPending if a stroke was pending);
 *      PassThrough. The IME owns the keyboard while it composes.
 *   2. Release: if e.code is the swallow-repeat code, clear it. Feed the tap detector; if
 *      it completes a double tap and a gate-passing action is bound to the gesture, Fire
 *      it (a pending stroke is dropped, cancelledPending). Otherwise PassThrough. Releases
 *      never touch the pending stroke otherwise.
 *   3. Press of a modifier key: feed the tap detector; PassThrough. A pending stroke
 *      survives modifier presses (Ctrl+K, then Ctrl is pressed again for Ctrl+C).
 *   4. Any other press resets the tap detector; a non-autorepeat press of a code other
 *      than the swallow-repeat code clears that code.
 *   5. Autorepeat: of the swallow-repeat code, or while a stroke is pending: Consumed
 *      (the pending stroke stays). Otherwise it matches like a press, so a held Ctrl+D
 *      keeps duplicating; but autorepeat never starts a sequence (a prefix stroke's
 *      repeat is Consumed).
 *   6-8. strokeFromEvent (layout.cpp): dead keys without Ctrl/Alt/Meta, keys mapping to
 *      Key::None and AltGr text are not strokes. For those: a pending stroke is cancelled
 *      and the event Consumed (the swallow-repeat code becomes e.code); otherwise
 *      PassThrough. Consuming a dead key does not clear the OS's own dead-key state, so
 *      the next character may still compose.
 *   9. With s = the stroke: if p is pending, the first gate-passing action of
 *      second(p, s) Fires, else the event is Consumed with cancelledPending (Escape
 *      included); either way the pending stroke is cleared and the swallow-repeat code
 *      becomes e.code, so the completing key's autorepeat does not type. Else, if a
 *      gate-passing action has a two-stroke binding starting with s, the result is
 *      Pending (the prefix beats a single-stroke binding of s; BindingIndex::conflicts
 *      reports such pairs). Else the first gate-passing action of single(s) Fires. Else
 *      PassThrough (typedText decides whether the widget inserts the event's text).
 * There is no sequence timeout; reset() cancels.
 *
 * Double taps (DoubleShift, DoubleCtrl), on the timeline
 *     t0 press 1   t1 release 1   t2 press 2   t3 release 2
 * fire at t3 when: both taps are of the same class (ShiftLeft and ShiftRight are both
 * Shift; Ctrl likewise), each release is of the key just pressed, the presses at t0 and t2
 * carry exactly that modifier, no other key event happens in [t0, t3] (autorepeat of the
 * held modifier is ignored), and t1-t0, t2-t1 and t3-t2 are each at most T (inclusive).
 * Intervals use osTimeMs as unsigned 32-bit differences (wrap-safe), so a UI-thread
 * stall that delivers queued events late cannot fake or break a double tap; when any of
 * the four events has osTimeMs == 0 all four use the steady time instead. When the check
 * fails at t3, the second tap becomes a first tap, so a slow tap followed by a quick pair
 * still fires. After firing the detector is idle, so a third tap starts a new pair. On
 * Windows the fake LCtrl press that precedes AltGr is followed by the AltRight press,
 * another key, so AltGr taps never complete DoubleCtrl. T = 0 disables gestures.
 */

#include "frost/input/shortcut_matcher.hpp"

#include <chrono>

namespace frost::input {

std::string_view toString(MatchOutcome outcome) noexcept {
    switch (outcome) {
    case MatchOutcome::PassThrough: return "PassThrough";
    case MatchOutcome::Consumed: return "Consumed";
    case MatchOutcome::Pending: return "Pending";
    case MatchOutcome::Fire: return "Fire";
    }
    return "MatchOutcome(?)";
}

namespace {

// The gesture class of a modifier key: Shift or Ctrl keys, or None (Alt, Meta, others).
Gesture tapClass(Code code) noexcept {
    switch (code) {
    case Code::ShiftLeft:
    case Code::ShiftRight: return Gesture::DoubleShift;
    case Code::ControlLeft:
    case Code::ControlRight: return Gesture::DoubleCtrl;
    default: return Gesture::None;
    }
}

Modifier classModifier(Gesture gesture) noexcept {
    return gesture == Gesture::DoubleShift ? Modifier::Shift : Modifier::Ctrl;
}

} // namespace

Gesture ShortcutMatcher::tapOnEvent(const KeyEvent& event, TapState& tap) const noexcept {
    using Phase = TapState::Phase;
    const IClock::Duration limit = m_options.doubleTapInterval;
    const Gesture cls = isModifierKey(event.code) ? tapClass(event.code) : Gesture::None;
    if (limit <= IClock::Duration::zero() || cls == Gesture::None) {
        tap = TapState{};
        return Gesture::None;
    }
    const auto record = [&](std::size_t slot) {
        tap.os[slot] = event.osTimeMs;
        tap.steady[slot] = event.time;
    };
    if (event.isPress()) {
        if (event.autoRepeat) {
            // Holding the tapped modifier repeats its press; that is not another key.
            if ((tap.phase == Phase::Down1 || tap.phase == Phase::Down2) && event.code == tap.code) {
                return Gesture::None;
            }
            tap = TapState{};
            return Gesture::None;
        }
        if (event.modifiers != Modifiers(classModifier(cls))) {
            tap = TapState{};
            return Gesture::None;
        }
        if (tap.phase == Phase::Up1 && tap.gesture == cls) {
            tap.phase = Phase::Down2;
            tap.code = event.code;
            record(2);
            return Gesture::None;
        }
        tap = TapState{};
        tap.phase = Phase::Down1;
        tap.gesture = cls;
        tap.code = event.code;
        record(0);
        return Gesture::None;
    }

    // A release.
    if (event.code != tap.code) {
        tap = TapState{};
        return Gesture::None;
    }
    if (tap.phase == Phase::Down1) {
        tap.phase = Phase::Up1;
        record(1);
        return Gesture::None;
    }
    if (tap.phase != Phase::Down2) {
        tap = TapState{};
        return Gesture::None;
    }
    const bool steadyBasis = tap.os[0] == 0 || tap.os[1] == 0 || tap.os[2] == 0 || event.osTimeMs == 0;
    bool within = true;
    if (steadyBasis) {
        const std::array<IClock::SteadyTime, 4> t = {tap.steady[0], tap.steady[1], tap.steady[2], event.time};
        for (std::size_t i = 1; i < t.size(); ++i) {
            within = within && t[i] - t[i - 1] <= limit;
        }
    } else {
        const std::array<std::uint32_t, 4> t = {tap.os[0], tap.os[1], tap.os[2], event.osTimeMs};
        for (std::size_t i = 1; i < t.size(); ++i) {
            const auto delta = static_cast<std::uint32_t>(t[i] - t[i - 1]); // Wraps safely.
            within = within && std::chrono::milliseconds(delta) <= limit;
        }
    }
    if (within) {
        const Gesture fired = tap.gesture;
        tap = TapState{};
        return fired;
    }
    // Too slow: the second tap becomes the first of a new pair.
    tap.phase = Phase::Up1;
    tap.os[0] = tap.os[2];
    tap.steady[0] = tap.steady[2];
    record(1);
    tap.os[2] = 0;
    tap.steady[2] = {};
    return Gesture::None;
}

ShortcutMatcher::Decision ShortcutMatcher::decide(const KeyEvent& event, const BindingIndex& bindings,
                                                  const LayoutTable& layout, Gate gate) const noexcept {
    Decision d{.result = {}, .pending = m_pending, .swallowCode = m_swallowCode, .tap = m_tap};
    MatchResult& r = d.result;

    // Step 1: the IME owns the keyboard.
    if (event.imeComposing) {
        r.cancelledPending = d.pending.has_value();
        d.pending.reset();
        d.swallowCode = Code::Unknown;
        d.tap = TapState{};
        return d;
    }

    // Step 2: releases.
    if (event.isRelease()) {
        if (event.code == d.swallowCode) {
            d.swallowCode = Code::Unknown;
        }
        const Gesture gesture = tapOnEvent(event, d.tap);
        if (gesture != Gesture::None) {
            for (const ActionId id : bindings.gesture(gesture)) {
                if (gate(id)) {
                    r.outcome = MatchOutcome::Fire;
                    r.action = id;
                    r.sequence = Sequence::of(gesture);
                    r.cancelledPending = d.pending.has_value();
                    d.pending.reset();
                    break;
                }
            }
        }
        return d;
    }

    // Step 3: modifier presses.
    if (isModifierKey(event.code)) {
        static_cast<void>(tapOnEvent(event, d.tap));
        return d;
    }

    // Step 4.
    d.tap = TapState{};
    if (!event.autoRepeat && event.code != d.swallowCode) {
        d.swallowCode = Code::Unknown;
    }

    // Step 5. Code::Unknown doubles as "no swallow-repeat code", so an unmapped key's
    // autorepeat is never swallowed by it.
    const bool swallowed = d.swallowCode != Code::Unknown && event.code == d.swallowCode;
    if (event.autoRepeat && (swallowed || d.pending.has_value())) {
        r.outcome = MatchOutcome::Consumed;
        return d;
    }

    const auto cancelPending = [&] {
        r.outcome = MatchOutcome::Consumed;
        r.cancelledPending = true;
        d.pending.reset();
        d.swallowCode = event.code;
    };

    // Steps 6-8.
    const std::optional<Stroke> stroke = strokeFromEvent(event, layout);
    if (!stroke) {
        if (d.pending) {
            cancelPending();
        }
        return d;
    }

    // Step 9.
    if (d.pending) {
        const Stroke first = *d.pending;
        for (const ActionId id : bindings.second(first, *stroke)) {
            if (gate(id)) {
                r.outcome = MatchOutcome::Fire;
                r.action = id;
                r.sequence = Sequence::of(first, *stroke);
                d.pending.reset();
                d.swallowCode = event.code;
                return d;
            }
        }
        cancelPending();
        return d;
    }
    for (const ActionId id : bindings.prefixed(*stroke)) {
        if (gate(id)) {
            if (event.autoRepeat) {
                r.outcome = MatchOutcome::Consumed; // Autorepeat never starts a sequence.
            } else {
                r.outcome = MatchOutcome::Pending;
                d.pending = *stroke;
            }
            return d;
        }
    }
    for (const ActionId id : bindings.single(*stroke)) {
        if (gate(id)) {
            r.outcome = MatchOutcome::Fire;
            r.action = id;
            r.sequence = Sequence::of(*stroke);
            return d;
        }
    }
    return d;
}

void ShortcutMatcher::apply(const Decision& decision) noexcept {
    m_pending = decision.pending;
    m_swallowCode = decision.swallowCode;
    m_tap = decision.tap;
}

MatchResult ShortcutMatcher::feed(const KeyEvent& event, const BindingIndex& bindings, const LayoutTable& layout,
                                  Gate gate) noexcept {
    const Decision decision = decide(event, bindings, layout, gate);
    apply(decision);
    return decision.result;
}

MatchOutcome ShortcutMatcher::peek(const KeyEvent& event, const BindingIndex& bindings, const LayoutTable& layout,
                                   Gate gate) const noexcept {
    return decide(event, bindings, layout, gate).result.outcome;
}

void ShortcutMatcher::reset() noexcept {
    m_pending.reset();
    m_swallowCode = Code::Unknown;
    m_tap = TapState{};
}

void ShortcutMatcher::setOptions(MatcherOptions options) noexcept {
    m_options = options;
    m_tap = TapState{};
}

} // namespace frost::input
