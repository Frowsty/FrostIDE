// SPDX-License-Identifier: Apache-2.0
//
// The keyboard doubles of frost::input (PLAN.md section 6.2; ADR 0016 decision 13):
// simulated en-US, de-DE, pl-PL and fr-FR layouts, and SimulatedKeyboard, which turns
// physical key presses into the KeyEvents the ui adapter would deliver on Windows or X11.
// They live in the library, like MemFs, so every consumer's tests can use them. No keyboard
// layout is ever switched on the machine: the layouts are data.
/*
 * The simulated layouts are a model of the Windows layouts named below, transcribed from
 * Microsoft's public layout charts (learn.microsoft.com/globalization/windows-keyboard-
 * layouts: US 00000409, German 00000407, Polish (Programmers) 00000415, French (Legacy,
 * AZERTY) 0000040C). They hold the 48 ANSI/ISO writing keys at four levels (IntlRo and
 * IntlYen are empty) and every dead-key composition the charts define. X11 layouts of the
 * same names define more level-3 characters (German AltGr+L types 'ł' there); the tests
 * that depend on a key producing nothing at AltGr say so.
 *
 * SimulatedKeyboard separates the OS truth (the table passed in, which decides what text the
 * "OS" produces) from the table the matcher sees (whatever the product would read on that
 * platform: the same full table on Windows, linuxM1Layout() on X11). Its semantics:
 *
 *   Modifier state is the physically held set, normalised as for KeyEvent (a modifier's
 *   press includes its own bit, its release excludes it).
 *
 *   Windows. On a layout with AltGr (truth.hasAltGr()), AltRight is AltGr: pressing it
 *   first delivers the fake ControlLeft press Windows generates, then the AltRight press;
 *   events while it is held carry altGr, Ctrl only for ControlRight and Alt only for
 *   AltLeft (the adapter reads ControlLeft as part of AltGr). Releasing it delivers the
 *   fake ControlLeft release, then the AltRight release. On a layout without AltGr,
 *   AltRight is plain Alt. Physical Ctrl+Alt (no Meta) selects the AltGr levels for text
 *   and dead keys, as the OS does, but altGr stays false. Other Ctrl combinations give
 *   control characters for letters (U+0001..U+001A) and no text for other keys;
 *   Alt+key gives the key's character (WM_SYSCHAR), which typedText() rejects.
 *
 *   X11 and Wayland. AltRight selects level 3 and sets altGr only when truth.hasAltGr();
 *   otherwise it is plain Alt. Ctrl+Alt never selects level 3. keyCharHint is
 *   simulatedKeyCharHint() (Qt's key() on X11: the printable ASCII character of the current
 *   Shift level, letters lowercase) when AltGr is not held; otherwise 0.
 *
 *   Dead keys. A dead press without Ctrl, Alt or Meta (on Windows, Ctrl+Alt counts as
 *   AltGr here) delivers an event with deadKey set and no text, and makes a composition
 *   pending. The next typing key delivers the composed character if the table defines one,
 *   else both characters; Space delivers the spacing character; the same dead key again
 *   delivers two spacing characters. A press with Ctrl, Alt or Meta held clears the pending
 *   composition and delivers the shortcut event with no text (a documented approximation
 *   of the OS). Other named keys clear it too. deadKey is also reported, without
 *   composing, when a dead level is pressed with Ctrl, Alt or Meta, as the adapter reports
 *   it from the layout table.
 *
 *   Numpad. With NumLock on, digits and the operators type their character (no text while
 *   Alt is held: Windows Alt-code entry composes on Alt's release); with NumLock off the
 *   digits type nothing.
 *
 *   IME. While setImeComposing(true), every event carries imeComposing and no text.
 *
 *   Time. Each event's time is clock.steadyNow(). osTimeMs is a simulated 32-bit OS tick
 *   that follows the clock (milliseconds since construction, plus a start value) and
 *   wraps; advance() moves the tick alone, to model a UI-thread stall that delivers events
 *   with distant OS times at nearly the same steady time, and setOsTick() sets it (to
 *   cross the wrap). setOsTimestamps(false) delivers osTimeMs = 0 (unknown).
 *
 * Not thread-safe; single owner. The truth table and the clock must outlive the keyboard.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/input/key.hpp"
#include "frost/input/key_event.hpp"
#include "frost/input/layout.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace frost::input {

enum class SimulatedLayoutId : std::uint8_t { EnUs, DeDe, PlPl, FrFr };

// The constinit simulated table for `id` (FullTable mode). The reference is valid for the
// program's lifetime; thread-safe (immutable).
[[nodiscard]] const LayoutTable& simulatedLayout(SimulatedLayoutId id) noexcept;
// The en-US table in LettersByLabel mode: the table the product uses on Linux in M1 (the
// same object readActiveLayout() returns there).
[[nodiscard]] const LayoutTable& linuxM1Layout() noexcept;
// "en-US", "de-DE", "pl-PL", "fr-FR".
[[nodiscard]] std::string_view simulatedLayoutName(SimulatedLayoutId id) noexcept;
// The keyCharHint SimulatedKeyboard reports on X11 and Wayland for a press of `code`
// without AltGr: the printable ASCII character `truth` gives the base level (or the Shift
// level when `shift`), letters folded to lowercase; 0 for dead levels, other characters
// and non-writing codes. O(1), thread-safe.
[[nodiscard]] char32_t simulatedKeyCharHint(const LayoutTable& truth, Code code, bool shift) noexcept;

class SimulatedKeyboard {
public:
    SimulatedKeyboard(const LayoutTable& truth, KeyPlatform platform, const IClock& clock) noexcept;

    // Presses `code` (an autorepeat press when it is already down).
    [[nodiscard]] std::vector<KeyEvent> press(Code code);
    // Releases `code`; nothing is delivered when it is not down.
    [[nodiscard]] std::vector<KeyEvent> release(Code code);
    // press(code) then release(code).
    [[nodiscard]] std::vector<KeyEvent> tap(Code code);
    // Presses `held` in order, taps `key`, then releases `held` in reverse order.
    [[nodiscard]] std::vector<KeyEvent> chord(std::initializer_list<Code> held, Code key);

    void setNumLock(bool on) noexcept { m_numLock = on; }
    void setImeComposing(bool composing) noexcept { m_imeComposing = composing; }
    // Moves the simulated OS tick forward by `delta` (whole milliseconds) without moving steady time.
    void advance(IClock::Duration delta) noexcept;
    // Makes the current OS tick `tick` (later ticks follow the clock from here).
    void setOsTick(std::uint32_t tick) noexcept;
    [[nodiscard]] std::uint32_t osTick() const noexcept;
    // false: events carry osTimeMs = 0 (unknown), as when Qt provides no timestamp.
    void setOsTimestamps(bool enabled) noexcept { m_osTimestamps = enabled; }

    // The pending dead key (spacing form), or 0.
    [[nodiscard]] char32_t pendingDeadKey() const noexcept { return m_pendingDead; }
    [[nodiscard]] bool isDown(Code code) const noexcept { return m_down[index(code)]; }
    [[nodiscard]] KeyPlatform platform() const noexcept { return m_platform; }
    [[nodiscard]] const LayoutTable& truth() const noexcept { return *m_truth; }

private:
    [[nodiscard]] bool altGrHeld() const noexcept;
    [[nodiscard]] Modifiers reportedModifiers() const noexcept;
    [[nodiscard]] KeyEvent makeEvent(KeyEventType type, Code code, bool autoRepeat) const noexcept;
    void fillPressText(KeyEvent& event);

    const LayoutTable* m_truth;
    const IClock* m_clock;
    IClock::SteadyTime m_clockStart;
    std::uint32_t m_tickStart = 1000;
    std::uint32_t m_extraMs = 0;
    KeyPlatform m_platform;
    std::array<bool, kCodeCount> m_down{};
    char32_t m_pendingDead = 0;
    bool m_numLock = true;
    bool m_imeComposing = false;
    bool m_osTimestamps = true;
    bool m_fakeCtrl = false;
};

} // namespace frost::input
