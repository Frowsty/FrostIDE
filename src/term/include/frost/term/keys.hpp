// SPDX-License-Identifier: Apache-2.0
//
// The terminal's key vocabulary (ADR 0051): the non-text keys VtScreen encodes for the child, the
// modifier set, and the table of IDE shortcuts the terminal keeps for itself while it has focus.
#pragma once

#include <cstdint>

namespace frost::term {

// The keys that are not plain text. Letters, digits and punctuation go through VtScreen::sendChar.
enum class TermKey : std::uint8_t {
    Enter,
    Tab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
};

// A Windows KEY_EVENT_RECORD represented without platform headers. Unicode is one UTF-16 unit.
struct Win32KeyEvent {
    std::uint16_t virtualKey = 0;
    std::uint16_t scanCode = 0;
    std::uint16_t unicode = 0;
    bool down = false;
    std::uint32_t controlState = 0;
    std::uint16_t repeatCount = 1;
};

// Modifiers held with a key. A plain value.
struct KeyMods {
    bool shift = false;
    bool ctrl = false;
    bool alt = false;

    [[nodiscard]] constexpr bool none() const noexcept { return !shift && !ctrl && !alt; }
    friend constexpr bool operator==(const KeyMods&, const KeyMods&) = default;
};

/*
 * Whether the terminal keeps `key` with `mods` while it has focus, so the IDE's keymap does not fire
 * on it (the shortcut dispatcher passes it through). The table follows what shells and full-screen
 * programs need, IntelliJ-style: every unmodified or Shift-only navigation and editing key (Escape
 * for vim, Tab for completion, the arrows, Home/End, PageUp/PageDown, Insert/Delete, Enter,
 * Backspace), the same keys with Ctrl or Alt (word movement), and the plain F1-F10 that htop and mc
 * use. The IDE keeps Shift+Escape (hide the tool window), Ctrl+Tab and Ctrl+Shift+Tab (the switcher),
 * F11, F12 and every modified F-key (Run, Debug, the tool window toggles). O(1).
 */
[[nodiscard]] bool terminalKeepsKey(TermKey key, KeyMods mods) noexcept;

/*
 * The same decision for a character key, `base` being the key's unshifted character as on US QWERTY
 * ('a'..'z', '0'..'9', '[', '/', ' ', ...). The terminal keeps every Ctrl+letter (readline and the
 * shells: Ctrl+C, Ctrl+D, Ctrl+R, Ctrl+E, ...) and Ctrl+[ ] \ / Space, every Alt+letter (readline's
 * meta keys), Ctrl+Shift+C and Ctrl+Shift+V (its own copy and paste), and plain or Shift-only text.
 * The IDE keeps Alt+digits (the tool windows), every other Ctrl+Shift combination (Find Action,
 * Find in Files) and Ctrl+Alt combinations (AltGr text arrives as text, not here). O(1).
 */
[[nodiscard]] bool terminalKeepsChar(char32_t base, KeyMods mods) noexcept;

} // namespace frost::term
