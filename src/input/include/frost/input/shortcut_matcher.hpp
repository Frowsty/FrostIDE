// SPDX-License-Identifier: Apache-2.0
//
// ShortcutMatcher: turns KeyEvents into shortcut outcomes (PLAN.md section 2.2 "Keyboard
// and IME", ADR 0016 decisions 4-7): single strokes, two-stroke sequences with autorepeat
// suppression, the AltGr policy, dead keys, IME suppression and modifier double taps
// timed by OS timestamps. Qt-free; the ui adapter translates QKeyEvents into KeyEvents.
#pragma once

#include "frost/compat/function_ref.hpp"
#include "frost/core/clock.hpp"
#include "frost/input/action.hpp"
#include "frost/input/key.hpp"
#include "frost/input/key_event.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/layout.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace frost::input {

enum class MatchOutcome : std::uint8_t {
    PassThrough, // Deliver the event to the focused widget as usual.
    Consumed,    // Swallow it; no action.
    Pending,     // The first stroke of a sequence; the UI may show the prefix.
    Fire,        // Run MatchResult::action.
};

[[nodiscard]] std::string_view toString(MatchOutcome outcome) noexcept;

struct MatchResult {
    MatchOutcome outcome = MatchOutcome::PassThrough;
    // Valid only for Fire.
    ActionId action = kNoAction;
    Sequence sequence;
    // A pending stroke was dropped during this call, for any reason.
    bool cancelledPending = false;
};

struct MatcherOptions {
    // The longest gap between the four events of a double tap (inclusive). 0 disables
    // gestures. KeymapService fills it from preferences or the platform interval.
    IClock::Duration doubleTapInterval = std::chrono::milliseconds(400);
};

/*
 * The matcher. It holds three pieces of state: the pending first stroke of a sequence,
 * the swallow-repeat code (the key that completed or cancelled a sequence, whose
 * autorepeat is consumed until it is released), and the tap detector. It never allocates.
 * Not thread-safe; owned by the UI thread.
 *
 * The gate decides whether an action may fire in the current context (KeymapService
 * passes ActionRegistry::isEnabled). It must not mutate the registry or re-enter the
 * matcher. Candidates are tried in BindingIndex order (priority, then keymap order); the
 * first that passes the gate wins.
 *
 * There is no sequence timeout: reset() cancels a pending stroke. Call it on focus
 * changes, mouse presses, IME start, Qt shortcut activation and layout changes.
 */
class ShortcutMatcher {
public:
    using Gate = function_ref<bool(ActionId)>;

    ShortcutMatcher() = default;
    explicit ShortcutMatcher(MatcherOptions options) noexcept : m_options(options) {}

    // Feeds one event (the algorithm is documented in shortcut_matcher.cpp). O(log B) plus
    // O(c) gate calls for the c actions sharing the stroke or prefix. Budget: B5.
    [[nodiscard]] MatchResult feed(const KeyEvent& event, const BindingIndex& bindings, const LayoutTable& layout,
                                   Gate gate) noexcept;
    // feed()'s outcome for `event`, without changing any state. Budget: B5.
    [[nodiscard]] MatchOutcome peek(const KeyEvent& event, const BindingIndex& bindings, const LayoutTable& layout,
                                    Gate gate) const noexcept;

    // Cancels the pending stroke, the swallow-repeat code and the tap state.
    void reset() noexcept;
    [[nodiscard]] std::optional<Stroke> pendingPrefix() const noexcept { return m_pending; }

    void setOptions(MatcherOptions options) noexcept;
    [[nodiscard]] const MatcherOptions& options() const noexcept { return m_options; }

private:
    // The double-tap detector's state (timeline in shortcut_matcher.cpp).
    struct TapState {
        enum class Phase : std::uint8_t { Idle, Down1, Up1, Down2 } phase = Phase::Idle;
        Gesture gesture = Gesture::None;
        // The tapped modifier key (both taps may use either key of the class).
        Code code = Code::Unknown;
        // OS and steady times of press 1, release 1 and press 2.
        std::array<std::uint32_t, 3> os{};
        std::array<IClock::SteadyTime, 3> steady{};
    };
    // What feed() would do: the result, and the state to adopt.
    struct Decision {
        MatchResult result;
        std::optional<Stroke> pending;
        Code swallowCode = Code::Unknown;
        TapState tap;
    };

    [[nodiscard]] Decision decide(const KeyEvent& event, const BindingIndex& bindings, const LayoutTable& layout,
                                  Gate gate) const noexcept;
    [[nodiscard]] Gesture tapOnEvent(const KeyEvent& event, TapState& tap) const noexcept;
    void apply(const Decision& decision) noexcept;

    MatcherOptions m_options;
    std::optional<Stroke> m_pending;
    Code m_swallowCode = Code::Unknown;
    TapState m_tap;
};

} // namespace frost::input
