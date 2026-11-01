// SPDX-License-Identifier: Apache-2.0
//
// ImeTracker: whether an input method is composing, derived from the Qt events the
// shell's event filter observes (PLAN.md section 2.2 "Keyboard and IME", ADR 0016). While
// it composes, translated key events carry imeComposing and the matcher passes them
// through untouched.
#pragma once

class QEvent;

namespace frost::ui {

/*
 * Composition state from observed events:
 *   - QEvent::InputMethod with a non-empty preedit string: composing (a start is
 *     remembered for takeCompositionStarted());
 *   - QEvent::InputMethod with an empty preedit (commit or cancel): not composing;
 *   - FocusIn, FocusOut, WindowDeactivate, and ApplicationStateChange to a non-active
 *     state: not composing.
 * Other events are ignored. Not thread-safe; UI thread only. O(1), no allocation.
 */
class ImeTracker {
public:
    void observe(const QEvent& event) noexcept;
    [[nodiscard]] bool composing() const noexcept { return m_composing; }
    // True once after composition started (an edge), so the filter can reset the matcher.
    [[nodiscard]] bool takeCompositionStarted() noexcept;
    void reset() noexcept;

private:
    bool m_composing = false;
    bool m_started = false;
};

} // namespace frost::ui
