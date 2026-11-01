// SPDX-License-Identifier: Apache-2.0
//
// SimulatedKeyboard (simulated_keyboard.hpp; ADR 0016 decision 13): the deterministic
// stand-in for OS keyboard input. It models what Windows and X11 deliver through Qt for
// physical key presses on a given layout, so tests never touch the machine's keyboard
// configuration or inject real input.

#include "frost/input/simulated_keyboard.hpp"

#include <chrono>
#include <iterator>

namespace frost::input {

namespace {

// The character a numpad key types with NumLock on (digits) or always (operators), or 0.
char32_t numpadChar(Code code, bool numLock) noexcept {
    if (code >= Code::Numpad0 && code <= Code::Numpad9) {
        return numLock ? static_cast<char32_t>(U'0' + (index(code) - index(Code::Numpad0))) : 0;
    }
    switch (code) {
    case Code::NumpadDecimal: return numLock ? U'.' : 0;
    case Code::NumpadAdd: return U'+';
    case Code::NumpadSubtract: return U'-';
    case Code::NumpadMultiply: return U'*';
    case Code::NumpadDivide: return U'/';
    default: return 0;
    }
}

// The control character of a named key (Enter, Tab, Backspace, Escape), or 0.
char32_t namedKeyChar(Code code) noexcept {
    switch (code) {
    case Code::Enter:
    case Code::NumpadEnter: return 0x0D;
    case Code::Tab: return 0x09;
    case Code::Backspace: return 0x08;
    case Code::Escape: return 0x1B;
    default: return 0;
    }
}

void setText(KeyEvent& event, char32_t a, char32_t b = 0) noexcept {
    event.text = {a, b};
    event.textLength = static_cast<std::uint8_t>(a == 0 ? 0 : (b == 0 ? 1 : 2));
}

} // namespace

char32_t simulatedKeyCharHint(const LayoutTable& truth, Code code, bool shift) noexcept {
    if (!isWritingKey(code)) {
        return 0;
    }
    const KeyLevels levels = truth.levels(code);
    const Level level = shift ? Level::Shift : Level::Base;
    const char32_t c = levels.at(level);
    if (levels.dead(level) || c <= U' ' || c >= 0x7F) {
        return 0;
    }
    return c >= U'A' && c <= U'Z' ? c + 0x20 : c;
}

SimulatedKeyboard::SimulatedKeyboard(const LayoutTable& truth, KeyPlatform platform, const IClock& clock) noexcept
    : m_truth(&truth), m_clock(&clock), m_clockStart(clock.steadyNow()), m_platform(platform) {}

std::uint32_t SimulatedKeyboard::osTick() const noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(m_clock->steadyNow() - m_clockStart);
    // 32-bit wrapping arithmetic, like GetTickCount and the X server time.
    return static_cast<std::uint32_t>(m_tickStart + static_cast<std::uint32_t>(elapsed.count()) + m_extraMs);
}

void SimulatedKeyboard::advance(IClock::Duration delta) noexcept {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    m_extraMs = static_cast<std::uint32_t>(m_extraMs + static_cast<std::uint32_t>(ms));
}

void SimulatedKeyboard::setOsTick(std::uint32_t tick) noexcept {
    const std::uint32_t now = osTick();
    m_tickStart = static_cast<std::uint32_t>(m_tickStart + (tick - now));
}

bool SimulatedKeyboard::altGrHeld() const noexcept { return m_truth->hasAltGr() && m_down[index(Code::AltRight)]; }

Modifiers SimulatedKeyboard::reportedModifiers() const noexcept {
    const auto down = [&](Code c) { return m_down[index(c)]; };
    Modifiers mods;
    if (down(Code::ShiftLeft) || down(Code::ShiftRight)) {
        mods = mods.with(Modifier::Shift);
    }
    if (down(Code::MetaLeft) || down(Code::MetaRight)) {
        mods = mods.with(Modifier::Meta);
    }
    bool ctrl = false;
    bool alt = false;
    if (altGrHeld()) {
        // Windows reports AltGr as LCtrl+RAlt; the adapter excludes both, keeping only
        // ControlRight and AltLeft. X11 reports AltGr as its own level-3 modifier.
        ctrl = m_platform == KeyPlatform::Windows ? down(Code::ControlRight)
                                                  : down(Code::ControlLeft) || down(Code::ControlRight);
        alt = down(Code::AltLeft);
    } else {
        ctrl = down(Code::ControlLeft) || down(Code::ControlRight) || m_fakeCtrl;
        alt = down(Code::AltLeft) || down(Code::AltRight);
    }
    if (ctrl) {
        mods = mods.with(Modifier::Ctrl);
    }
    if (alt) {
        mods = mods.with(Modifier::Alt);
    }
    return mods;
}

KeyEvent SimulatedKeyboard::makeEvent(KeyEventType type, Code code, bool autoRepeat) const noexcept {
    KeyEvent event;
    event.time = m_clock->steadyNow();
    event.osTimeMs = m_osTimestamps ? osTick() : 0;
    event.type = type;
    event.code = code;
    event.modifiers = reportedModifiers();
    event.altGr = altGrHeld();
    event.autoRepeat = autoRepeat;
    event.numLock = m_numLock;
    event.imeComposing = m_imeComposing;
    if (m_platform != KeyPlatform::Windows && isWritingKey(code) && !event.altGr) {
        // Qt's key() on X11: the character of the current Shift level.
        const bool shift = m_down[index(Code::ShiftLeft)] || m_down[index(Code::ShiftRight)];
        event.keyCharHint = simulatedKeyCharHint(*m_truth, code, shift);
    }
    return event;
}

void SimulatedKeyboard::fillPressText(KeyEvent& event) {
    if (m_imeComposing || isModifierKey(event.code)) {
        return;
    }
    const auto down = [&](Code c) { return m_down[index(c)]; };
    const bool shift = down(Code::ShiftLeft) || down(Code::ShiftRight);
    const bool meta = down(Code::MetaLeft) || down(Code::MetaRight);
    const bool ctrlPhysical = down(Code::ControlLeft) || down(Code::ControlRight);
    const bool altPhysical = down(Code::AltLeft) || down(Code::AltRight);
    // Windows treats physical Ctrl+Alt (no Win key) as AltGr for text and dead keys.
    const bool ctrlAltAsAltGr =
        m_platform == KeyPlatform::Windows && !event.altGr && ctrlPhysical && altPhysical && !meta;
    const bool altGrLevel = event.altGr || ctrlAltAsAltGr;
    const bool ctrl = event.modifiers.has(Modifier::Ctrl) && !ctrlAltAsAltGr;
    const bool alt = event.modifiers.has(Modifier::Alt) && !ctrlAltAsAltGr;
    const bool command = ctrl || alt || meta;

    // Named keys: their control character (never typed), and a pending dead key is dropped.
    if (const char32_t named = namedKeyChar(event.code); named != 0) {
        m_pendingDead = 0;
        if (!meta) {
            setText(event, named);
        }
        return;
    }

    char32_t ch = 0;
    bool dead = false;
    if (isWritingKey(event.code)) {
        const Level level =
            altGrLevel ? (shift ? Level::AltGrShift : Level::AltGr) : (shift ? Level::Shift : Level::Base);
        const KeyLevels levels = m_truth->levels(event.code);
        ch = levels.at(level);
        dead = ch != 0 && levels.dead(level);
    } else if (event.code == Code::Space) {
        ch = U' ';
    } else if (isNumpadKey(event.code)) {
        ch = alt || ctrlAltAsAltGr ? 0 : numpadChar(event.code, m_numLock);
    } else {
        m_pendingDead = 0; // Navigation and function keys.
        return;
    }

    if (command) {
        // A shortcut: the pending composition is dropped and no composed text results.
        m_pendingDead = 0;
        if (dead) {
            event.deadKey = ch; // Reported from the layout table, without composing.
            return;
        }
        if (meta) {
            return;
        }
        if (ctrl) {
            const char32_t base = isWritingKey(event.code) ? m_truth->levels(event.code).base : 0;
            const char32_t folded = base >= U'A' && base <= U'Z' ? base + 0x20 : base;
            if (folded >= U'a' && folded <= U'z') {
                setText(event, folded - U'a' + 1); // Ctrl+letter: U+0001..U+001A.
            }
            return;
        }
        if (ch != 0) {
            setText(event, ch); // Alt+key: the character (WM_SYSCHAR on Windows).
        }
        return;
    }

    if (m_pendingDead != 0) {
        const char32_t pending = m_pendingDead;
        m_pendingDead = 0;
        if (event.code == Code::Space) {
            setText(event, pending);
        } else if (ch == 0) {
            // Nothing to compose with: the composition is dropped.
        } else if (const auto composed = m_truth->compose(pending, ch)) {
            setText(event, *composed);
        } else {
            setText(event, pending, ch); // No composition (including a second dead key).
        }
        return;
    }
    if (dead) {
        event.deadKey = ch;
        m_pendingDead = ch;
        return;
    }
    if (ch != 0) {
        setText(event, ch);
    }
}

std::vector<KeyEvent> SimulatedKeyboard::press(Code code) {
    std::vector<KeyEvent> events;
    if (code == Code::Unknown || index(code) >= kCodeCount) {
        return events;
    }
    if (m_down[index(code)]) {
        KeyEvent repeat = makeEvent(KeyEventType::Press, code, true);
        fillPressText(repeat);
        events.push_back(repeat);
        return events;
    }
    const bool altGrKey = m_platform == KeyPlatform::Windows && code == Code::AltRight && m_truth->hasAltGr();
    if (altGrKey) {
        // Windows generates a fake LCtrl press ahead of AltGr.
        m_fakeCtrl = true;
        events.push_back(makeEvent(KeyEventType::Press, Code::ControlLeft, false));
    }
    m_down[index(code)] = true;
    if (altGrKey) {
        m_fakeCtrl = false; // From here on the fake Ctrl is part of AltGr.
    }
    KeyEvent event = makeEvent(KeyEventType::Press, code, false);
    fillPressText(event);
    events.push_back(event);
    return events;
}

std::vector<KeyEvent> SimulatedKeyboard::release(Code code) {
    std::vector<KeyEvent> events;
    if (code == Code::Unknown || index(code) >= kCodeCount || !m_down[index(code)]) {
        return events;
    }
    const bool altGrKey = m_platform == KeyPlatform::Windows && code == Code::AltRight && m_truth->hasAltGr();
    if (altGrKey) {
        // ... and the fake LCtrl release ahead of AltGr's release.
        events.push_back(makeEvent(KeyEventType::Release, Code::ControlLeft, false));
    }
    m_down[index(code)] = false;
    events.push_back(makeEvent(KeyEventType::Release, code, false));
    return events;
}

std::vector<KeyEvent> SimulatedKeyboard::tap(Code code) {
    std::vector<KeyEvent> events = press(code);
    const std::vector<KeyEvent> up = release(code);
    events.insert(events.end(), up.begin(), up.end());
    return events;
}

std::vector<KeyEvent> SimulatedKeyboard::chord(std::initializer_list<Code> held, Code key) {
    std::vector<KeyEvent> events;
    for (const Code c : held) {
        const std::vector<KeyEvent> down = press(c);
        events.insert(events.end(), down.begin(), down.end());
    }
    const std::vector<KeyEvent> tapped = tap(key);
    events.insert(events.end(), tapped.begin(), tapped.end());
    for (auto it = std::rbegin(held); it != std::rend(held); ++it) {
        const std::vector<KeyEvent> up = release(*it);
        events.insert(events.end(), up.begin(), up.end());
    }
    return events;
}

} // namespace frost::input
