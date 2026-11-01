// SPDX-License-Identifier: Apache-2.0
//
// KeyEvent: the Qt-free key event the shortcut matcher consumes, and KeyPlatform, the one
// enum for platform key semantics (PLAN.md section 2.2, ADR 0016). The ui adapter
// (frost/ui/key_translation.hpp) fills a KeyEvent from a QKeyEvent; the SimulatedKeyboard
// double (simulated_keyboard.hpp) produces the same events for tests. Kept lightweight so
// the adapter and m1/editor-view include only this header.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/input/key.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace frost::input {

// The platform whose key semantics an event follows. Wayland behaves like X11 except
// where a comment says otherwise. (macOS has no value: unsupported, D24.)
enum class KeyPlatform : std::uint8_t { Windows, X11, Wayland };

enum class KeyEventType : std::uint8_t { Press, Release };

/*
 * One key press or release, as the OS and Qt report it.
 *
 * modifiers holds Ctrl, Alt, Shift and Meta as held after the event. The adapter
 * normalises it for modifier keys (a modifier's press includes its own bit, its release
 * excludes it) and excludes the synthetic Ctrl+Alt that Windows reports for AltGr; altGr
 * says whether AltGr (Windows right Alt on an AltGr layout, X11 ISO_Level3_Shift) is held.
 * On a Windows AltGr layout the right Alt key is AltGr, not Alt: its own press sets altGr
 * and carries no Alt bit (SimulatedKeyboard models the same).
 *
 * text holds up to two code points the OS produced for a press, possibly control
 * characters (Ctrl+A gives U+0001 on both platforms; Windows Alt+letter gives the letter
 * through WM_SYSCHAR). Whether a widget should insert it is typedText()'s decision.
 * deadKey is non-zero when this press is a dead key (the spacing form: U+005E '^', U+00B4,
 * U+0060, U+007E, U+00A8, ...); the press then carries no text. keyCharHint is the
 * lowercase Latin letter Qt reports as the key on X11 and Wayland (0 on Windows), used by
 * the LettersByLabel mapping mode.
 *
 * osTimeMs is the OS event time in milliseconds (GetMessageTime on Windows, the X server
 * time on X11): a 32-bit counter that wraps, compared only through unsigned differences;
 * 0 means unknown. time is the steady time the adapter saw the event.
 *
 * Trivially copyable and at most 40 bytes, so it is passed by value or const reference
 * on the keystroke path without allocation.
 */
struct KeyEvent {
    IClock::SteadyTime time{};
    char32_t deadKey = 0;
    char32_t keyCharHint = 0;
    std::array<char32_t, 2> text{};
    std::uint32_t osTimeMs = 0;
    KeyEventType type = KeyEventType::Press;
    Code code = Code::Unknown;
    Modifiers modifiers;
    std::uint8_t textLength = 0;
    bool altGr = false;
    bool autoRepeat = false;
    bool numLock = true;
    bool imeComposing = false;
    // The code was derived from the Qt::Key (a synthesized event without a scan code).
    bool codeFromFallback = false;

    [[nodiscard]] constexpr bool isPress() const noexcept { return type == KeyEventType::Press; }
    [[nodiscard]] constexpr bool isRelease() const noexcept { return type == KeyEventType::Release; }

    // True when any produced code point is printable: >= U+0020 and != U+007F. O(1).
    [[nodiscard]] bool hasPrintableText() const noexcept;
    // The produced text; a view into this event, valid while it lives.
    [[nodiscard]] std::u32string_view textView() const noexcept { return {text.data(), textLength}; }
};
static_assert(std::is_trivially_copyable_v<KeyEvent>, "KeyEvent travels by value on the keystroke path");
static_assert(sizeof(KeyEvent) <= 40, "KeyEvent stays within 40 bytes");

/*
 * The text a text widget should insert for a press the matcher passed through, or an
 * empty view. Text is typed when the event is a press with printable text and one of:
 *   - no Ctrl, Alt or Meta is held;
 *   - altGr is set;
 *   - on Windows, Ctrl and Alt are held without Meta (the OS treats Ctrl+Alt as AltGr, so
 *     LCtrl+LAlt+L types 'ł' on Polish (Programmers) when no binding claims it).
 * So Windows Alt+letter text (WM_SYSCHAR) and X11 Ctrl+letter control characters are
 * never typed. The view points into `event` and lives as long as it. O(1), noexcept.
 */
[[nodiscard]] std::u32string_view typedText(const KeyEvent& event, KeyPlatform platform) noexcept;

} // namespace frost::input
