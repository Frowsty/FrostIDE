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

#include <cstdint>

class QKeyEvent;
class QString;

namespace frost::ui {

/*
 * The per-side modifier bits of QKeyEvent::nativeModifiers() under Qt's "windows" platform
 * plugin. Qt reads them from the thread's key state while it processes the key message,
 * so they describe that message even when Qt delivers the QKeyEvent later, after the
 * dispatcher has already retrieved later key messages (GetKeyState at delivery time can
 * already show a later release). Qt does not document the values; they were observed with
 * Qt 6.12 (ADR 0016 decision 4) and NativeKeyDelivery pins them on a real, never-shown
 * window. Synthesized events (QTest, the automation endpoint) carry 0.
 */
namespace windows_native {
inline constexpr std::uint32_t kShiftLeft = 0x01;
inline constexpr std::uint32_t kControlLeft = 0x02;
inline constexpr std::uint32_t kAltLeft = 0x04;
inline constexpr std::uint32_t kMetaLeft = 0x08;
inline constexpr std::uint32_t kShiftRight = 0x10;
inline constexpr std::uint32_t kControlRight = 0x20;
inline constexpr std::uint32_t kAltRight = 0x40;
inline constexpr std::uint32_t kMetaRight = 0x80;
// AltGr as Windows reports it: right Alt plus the fake left Ctrl it generates.
inline constexpr std::uint32_t kAltGr = kControlLeft | kAltRight;
} // namespace windows_native

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
 *     letters and digits; for the 11 US punctuation keys the first writing key (in Code
 *     order) that types the character at base, as the hardware key would report (de
 *     Key_Minus is the '-' key, Code::Slash; fr Key_Apostrophe is Digit4; ADR 0030), else
 *     layout.codeFor() (de Key_Slash is Code::Slash); and for other printable characters
 *     the writing key that produces them at base or Shift level (Key_Question is US
 *     Slash); codeFromFallback is then set. O(writing keys), no allocation.
 *   - modifiers: Qt's, without KeypadModifier; a modifier key's own bit is set on its press
 *     and cleared on its release (the AltGr refinement below then applies, so a Windows
 *     AltGr press reports altGr without Alt).
 *   - altGr (Windows): Qt::GroupSwitchModifier (the windows:altgr platform option), or Ctrl
 *     and Alt both reported while the event's nativeModifiers() hold windows_native::
 *     kAltRight and a Ctrl bit on a layout with AltGr; in that second case (also with
 *     GroupSwitchModifier) Alt becomes kAltLeft's state and Ctrl kControlRight's
 *     (kControlLeft is AltGr's fake Ctrl). The live key state (GetKeyState) is never read:
 *     it may already belong to a later message. An event without per-side bits (QTest,
 *     the automation endpoint) keeps Ctrl+Alt as reported.
 *     X11 and Wayland: GroupSwitchModifier, or on X11 Mod5 in the native modifiers (the
 *     usual ISO_Level3_Shift mapping); best effort.
 *   - imeComposing: ime.composing(), or on Windows the VK_PROCESSKEY virtual key.
 *   - numLock: off when a numpad code reports a navigation key, on otherwise.
 *   - deadKey: Windows reads the layout table first (no text and the dead bit of the level
 *     Shift and AltGr or Ctrl+Alt select), then Qt::Key_Dead_*; X11 and Wayland use
 *     Qt::Key_Dead_* only. A dead press carries no text.
 *   - text: QKeyEvent::text(), at most two code points; keyCharHint: on X11 and Wayland the
 *     printable ASCII character of the Qt::Key (0x21..0x7E), Key_A..Key_Z as a-z; 0 for
 *     other keys and on Windows.
 *   - time: clock.steadyNow(); osTimeMs: the Qt timestamp (32-bit OS milliseconds).
 * noexcept, no allocation. Budget: B5.
 */
[[nodiscard]] input::KeyEvent translateKeyEvent(const QKeyEvent& event, const KeyTranslationContext& context) noexcept;

} // namespace frost::ui
