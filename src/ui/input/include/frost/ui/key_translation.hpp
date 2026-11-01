// SPDX-License-Identifier: Apache-2.0
//
// The Qt adapter of frost::input (PLAN.md section 2.2 "Keyboard and IME", ADR 0016):
// translates a QKeyEvent into the Qt-free frost::input::KeyEvent the shortcut matcher
// consumes, and names the platform whose key semantics apply.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/input/key_event.hpp"
#include "frost/input/layout.hpp"
#include "frost/ui/ime_tracker.hpp"

class QKeyEvent;
class QString;

namespace frost::ui {

// The key semantics of the platform plugin named `name`: "windows" is Windows, "xcb"
// X11, any name starting with "wayland" Wayland; "offscreen", "minimal" and others give
// the native default (Windows on Windows, X11 elsewhere). O(1), thread-safe.
[[nodiscard]] input::KeyPlatform keyPlatformFromName(const QString& name) noexcept;
// keyPlatformFromName(QGuiApplication::platformName()). Requires a QGuiApplication.
[[nodiscard]] input::KeyPlatform currentKeyPlatform() noexcept;

// What translateKeyEvent needs besides the event. All references must outlive the call.
struct KeyTranslationContext {
    input::KeyPlatform platform;
    const input::LayoutTable& layout;
    const input::IKeyStateProbe& keyState;
    const ImeTracker& ime;
    const IClock& clock;
};

/*
 * Translates one key event. Precondition: the type is KeyPress, KeyRelease or
 * ShortcutOverride (translated as a press).
 *   - code: from the native scan code (Windows: the 9-bit scan code; X11 and Wayland: the
 *     xkb keycode minus 8, an evdev code). Synthesized events without one (QTest, the
 *     automation endpoint, some remote tools) fall back to the Qt::Key: a table for named
 *     and modifier keys (KeypadModifier selects numpad codes), layout.codeFor() for
 *     letters, digits and the 11 punctuation keys, and for other printable characters the
 *     writing key that produces them at base or Shift level (Key_Question is US Slash);
 *     codeFromFallback is then set.
 *   - modifiers: Qt's, without KeypadModifier; a modifier key's own bit is set on its press
 *     and cleared on its release (the AltGr refinement below then applies, so a Windows
 *     AltGr press reports altGr without Alt).
 *   - altGr (Windows): Qt::GroupSwitchModifier (the windows:altgr platform option), or Ctrl
 *     and Alt both reported while keyState says RightAlt is down on a layout with AltGr;
 *     in that second case (also with GroupSwitchModifier) Alt becomes LeftAlt's state and
 *     Ctrl RightCtrl's (LeftCtrl is AltGr's fake Ctrl).
 *     X11 and Wayland: GroupSwitchModifier, or on X11 Mod5 in the native modifiers (the
 *     usual ISO_Level3_Shift mapping); best effort.
 *   - imeComposing: ime.composing(), or on Windows the VK_PROCESSKEY virtual key.
 *   - numLock: off when a numpad code reports a navigation key, on otherwise.
 *   - deadKey: Windows reads the layout table first (no text and the dead bit of the level
 *     Shift and AltGr or Ctrl+Alt select), then Qt::Key_Dead_*; X11 and Wayland use
 *     Qt::Key_Dead_* only. A dead press carries no text.
 *   - text: QKeyEvent::text(), at most two code points; keyCharHint: the a-z letter of
 *     Key_A..Key_Z on X11 and Wayland, 0 on Windows.
 *   - time: clock.steadyNow(); osTimeMs: the Qt timestamp (32-bit OS milliseconds).
 * noexcept, no allocation. Budget: B5.
 */
[[nodiscard]] input::KeyEvent translateKeyEvent(const QKeyEvent& event, const KeyTranslationContext& context) noexcept;

} // namespace frost::ui
