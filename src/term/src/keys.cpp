// SPDX-License-Identifier: Apache-2.0
//
// The "terminal keeps these keys" table (frost/term/keys.hpp).

#include "frost/term/keys.hpp"

namespace frost::term {

bool terminalKeepsKey(TermKey key, KeyMods mods) noexcept {
    switch (key) {
    case TermKey::Escape:
        // Shift+Escape hides the tool window (window.hideActive).
        return !mods.shift;
    case TermKey::Tab:
        // Ctrl+Tab and Ctrl+Shift+Tab are the switcher.
        return !mods.ctrl;
    case TermKey::Enter:
    case TermKey::Backspace:
    case TermKey::Up:
    case TermKey::Down:
    case TermKey::Left:
    case TermKey::Right:
    case TermKey::Insert:
    case TermKey::Delete:
    case TermKey::Home:
    case TermKey::End:
    case TermKey::PageUp:
    case TermKey::PageDown: return true;
    case TermKey::F11:
    case TermKey::F12: return false;
    case TermKey::F1:
    case TermKey::F2:
    case TermKey::F3:
    case TermKey::F4:
    case TermKey::F5:
    case TermKey::F6:
    case TermKey::F7:
    case TermKey::F8:
    case TermKey::F9:
    case TermKey::F10: return mods.none();
    }
    return false;
}

bool terminalKeepsChar(char32_t base, KeyMods mods) noexcept {
    const bool letter = base >= U'a' && base <= U'z';
    const bool digit = base >= U'0' && base <= U'9';
    if (!mods.ctrl && !mods.alt) {
        return true; // Text, shifted or not.
    }
    if (mods.ctrl && mods.alt) {
        return false;
    }
    if (mods.ctrl && mods.shift) {
        return base == U'c' || base == U'v';
    }
    if (mods.ctrl) {
        return letter || base == U'[' || base == U']' || base == U'\\' || base == U'/' || base == U' ';
    }
    // Alt (with or without Shift): readline's meta keys, but not the tool-window digits.
    return letter || (!digit && base != U' ');
}

} // namespace frost::term
