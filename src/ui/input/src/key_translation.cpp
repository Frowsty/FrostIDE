// SPDX-License-Identifier: Apache-2.0
//
// translateKeyEvent and currentKeyPlatform (frost/ui/key_translation.hpp; ADR 0016
// decisions 2, 4 and 5): QKeyEvent to frost::input::KeyEvent.

#include "frost/ui/key_translation.hpp"

#include "frost/input/key.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QString>

#include <cstdint>

namespace frost::ui {

using input::Code;
using input::Key;
using input::KeyPlatform;
using input::Level;
using input::Modifier;
using input::Modifiers;

namespace {

// The X11 Mod5 bit, which xkeyboard-config maps to ISO_Level3_Shift (AltGr).
constexpr std::uint32_t kX11Mod5 = 0x80;
// VK_PROCESSKEY: Windows delivers it for keys the IME consumes.
constexpr std::uint32_t kVkProcessKey = 0xE5;

// The spacing form of a Qt dead key, or 0.
char32_t deadKeyChar(int key) noexcept {
    switch (key) {
    case Qt::Key_Dead_Grave: return U'`';
    case Qt::Key_Dead_Acute: return U'´';
    case Qt::Key_Dead_Circumflex: return U'^';
    case Qt::Key_Dead_Tilde: return U'~';
    case Qt::Key_Dead_Diaeresis: return U'¨';
    case Qt::Key_Dead_Cedilla: return U'¸';
    case Qt::Key_Dead_Caron: return U'ˇ';
    case Qt::Key_Dead_Ogonek: return U'˛';
    case Qt::Key_Dead_Abovedot: return U'˙';
    case Qt::Key_Dead_Doubleacute: return U'˝';
    case Qt::Key_Dead_Breve: return U'˘';
    case Qt::Key_Dead_Macron: return U'¯';
    case Qt::Key_Dead_Abovering: return U'˚';
    default: return 0;
    }
}

// The code of a named or modifier Qt::Key (keypad: the numpad code), or Unknown.
Code namedKeyCode(int key, bool keypad) noexcept {
    if (keypad) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            return static_cast<Code>(input::index(Code::Numpad0) + static_cast<std::size_t>(key - Qt::Key_0));
        }
        switch (key) {
        case Qt::Key_Plus: return Code::NumpadAdd;
        case Qt::Key_Minus: return Code::NumpadSubtract;
        case Qt::Key_Asterisk: return Code::NumpadMultiply;
        case Qt::Key_Slash: return Code::NumpadDivide;
        case Qt::Key_Period:
        case Qt::Key_Comma: return Code::NumpadDecimal;
        case Qt::Key_Enter:
        case Qt::Key_Return: return Code::NumpadEnter;
        // NumLock off: the navigation meanings of the numpad keys.
        case Qt::Key_Insert: return Code::Numpad0;
        case Qt::Key_End: return Code::Numpad1;
        case Qt::Key_Down: return Code::Numpad2;
        case Qt::Key_PageDown: return Code::Numpad3;
        case Qt::Key_Left: return Code::Numpad4;
        case Qt::Key_Clear: return Code::Numpad5;
        case Qt::Key_Right: return Code::Numpad6;
        case Qt::Key_Home: return Code::Numpad7;
        case Qt::Key_Up: return Code::Numpad8;
        case Qt::Key_PageUp: return Code::Numpad9;
        case Qt::Key_Delete: return Code::NumpadDecimal;
        default: break;
        }
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return static_cast<Code>(input::index(Code::F1) + static_cast<std::size_t>(key - Qt::Key_F1));
    }
    switch (key) {
    case Qt::Key_Escape: return Code::Escape;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return Code::Tab;
    case Qt::Key_Backspace: return Code::Backspace;
    case Qt::Key_Return:
    case Qt::Key_Enter: return Code::Enter;
    case Qt::Key_Insert: return Code::Insert;
    case Qt::Key_Delete: return Code::Delete;
    case Qt::Key_Home: return Code::Home;
    case Qt::Key_End: return Code::End;
    case Qt::Key_PageUp: return Code::PageUp;
    case Qt::Key_PageDown: return Code::PageDown;
    case Qt::Key_Left: return Code::ArrowLeft;
    case Qt::Key_Right: return Code::ArrowRight;
    case Qt::Key_Up: return Code::ArrowUp;
    case Qt::Key_Down: return Code::ArrowDown;
    case Qt::Key_Space: return Code::Space;
    case Qt::Key_Menu: return Code::ContextMenu;
    case Qt::Key_Print: return Code::PrintScreen;
    case Qt::Key_Pause: return Code::Pause;
    case Qt::Key_Shift: return Code::ShiftLeft;
    case Qt::Key_Control: return Code::ControlLeft;
    case Qt::Key_Alt: return Code::AltLeft;
    case Qt::Key_AltGr: return Code::AltRight;
    case Qt::Key_Meta: return Code::MetaLeft;
    default: return Code::Unknown;
    }
}

// The logical key a Qt::Key names among letters, digits and the 11 punctuation keys.
Key writingKeyOf(int key) noexcept {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<Key>(input::index(Key::A) + static_cast<std::size_t>(key - Qt::Key_A));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<Key>(input::index(Key::Digit0) + static_cast<std::size_t>(key - Qt::Key_0));
    }
    switch (key) {
    case Qt::Key_QuoteLeft: return Key::Backquote;
    case Qt::Key_Minus: return Key::Minus;
    case Qt::Key_Equal: return Key::Equal;
    case Qt::Key_BracketLeft: return Key::BracketLeft;
    case Qt::Key_BracketRight: return Key::BracketRight;
    case Qt::Key_Backslash: return Key::Backslash;
    case Qt::Key_Semicolon: return Key::Semicolon;
    case Qt::Key_Apostrophe: return Key::Quote;
    case Qt::Key_Comma: return Key::Comma;
    case Qt::Key_Period: return Key::Period;
    case Qt::Key_Slash: return Key::Slash;
    default: return Key::None;
    }
}

// The writing code whose base (preferred) or Shift level produces `c`, comparing with
// simpleUpper because Qt reports a printable key as its uppercase code point; Unknown if
// none. Dead levels never match. O(writing keys), no allocation.
Code codeProducing(char32_t c, const input::LayoutTable& layout) noexcept {
    for (const input::Level level : {input::Level::Base, input::Level::Shift}) {
        for (std::size_t i = input::index(Code::KeyA); i <= input::index(Code::IntlYen); ++i) {
            const auto code = static_cast<Code>(i);
            const input::KeyLevels levels = layout.levels(code);
            const char32_t produced = levels.at(level);
            if (produced != 0 && !levels.dead(level) && (produced == c || input::simpleUpper(produced) == c)) {
                return code;
            }
        }
    }
    return Code::Unknown;
}

Code fallbackCode(int key, bool keypad, const input::LayoutTable& layout) noexcept {
    if (const Code named = namedKeyCode(key, keypad); named != Code::Unknown) {
        return named;
    }
    if (const Key writing = writingKeyOf(key); writing != Key::None) {
        return layout.codeFor(writing);
    }
    // Other printable characters (Qt::Key_Question, Key_Underscore, Key_Adiaeresis, ...):
    // Qt::Key values below 0x01000000 are code points.
    if (key > 0x20 && key < 0x110000) {
        return codeProducing(static_cast<char32_t>(key), layout);
    }
    return Code::Unknown;
}

bool isNavigationKey(int key) noexcept {
    switch (key) {
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Insert:
    case Qt::Key_Delete:
    case Qt::Key_Clear: return true;
    default: return false;
    }
}

Modifier modifierOf(Code code) noexcept {
    switch (code) {
    case Code::ShiftLeft:
    case Code::ShiftRight: return Modifier::Shift;
    case Code::ControlLeft:
    case Code::ControlRight: return Modifier::Ctrl;
    case Code::AltLeft:
    case Code::AltRight: return Modifier::Alt;
    default: return Modifier::Meta;
    }
}

} // namespace

input::KeyPlatform currentKeyPlatform() noexcept { return keyPlatformFromName(QGuiApplication::platformName()); }

input::KeyPlatform keyPlatformFromName(const QString& name) noexcept {
    if (name == QLatin1String("windows")) {
        return KeyPlatform::Windows;
    }
    if (name == QLatin1String("xcb")) {
        return KeyPlatform::X11;
    }
    if (name.startsWith(QLatin1String("wayland"))) {
        return KeyPlatform::Wayland;
    }
#ifdef _WIN32
    return KeyPlatform::Windows;
#else
    return KeyPlatform::X11;
#endif
}

input::KeyEvent translateKeyEvent(const QKeyEvent& event, const KeyTranslationContext& context) noexcept {
    const KeyPlatform platform = context.platform;
    input::KeyEvent out;
    out.time = context.clock.steadyNow();
    out.osTimeMs = static_cast<std::uint32_t>(event.timestamp());
    out.type = event.type() == QEvent::KeyRelease ? input::KeyEventType::Release : input::KeyEventType::Press;
    out.autoRepeat = event.isAutoRepeat();

    const int key = event.key();
    const Qt::KeyboardModifiers qtMods = event.modifiers();
    const bool keypad = qtMods.testFlag(Qt::KeypadModifier);

    // The physical key.
    const std::uint32_t scan = event.nativeScanCode();
    Code code = Code::Unknown;
    if (platform == KeyPlatform::Windows) {
        code = scan < 0x200 ? input::codeFromWindowsScanCode(static_cast<std::uint16_t>(scan)) : Code::Unknown;
    } else if (scan >= 8 && scan - 8 < 0x100) {
        code = input::codeFromEvdev(static_cast<std::uint16_t>(scan - 8));
    }
    if (code == Code::Unknown) {
        code = fallbackCode(key, keypad, context.layout);
        out.codeFromFallback = code != Code::Unknown;
    }
    out.code = code;

    // Modifiers, normalised for modifier keys.
    Modifiers mods;
    if (qtMods.testFlag(Qt::ControlModifier)) {
        mods = mods.with(Modifier::Ctrl);
    }
    if (qtMods.testFlag(Qt::AltModifier)) {
        mods = mods.with(Modifier::Alt);
    }
    if (qtMods.testFlag(Qt::ShiftModifier)) {
        mods = mods.with(Modifier::Shift);
    }
    if (qtMods.testFlag(Qt::MetaModifier)) {
        mods = mods.with(Modifier::Meta);
    }
    if (input::isModifierKey(code)) {
        mods = out.isPress() ? mods.with(modifierOf(code)) : mods.without(modifierOf(code));
    }
    // Ctrl+Alt as reported, before the AltGr refinement: Windows selects the AltGr levels for it.
    const bool reportedCtrlAlt = mods.has(Modifier::Ctrl) && mods.has(Modifier::Alt) && !mods.has(Modifier::Meta);

    // AltGr.
    // QKeyEvent::modifiers() also reports GroupSwitchModifier for the Key_AltGr key itself
    // (windows:altgr), which may still carry the synthetic Ctrl+Alt, so the refinement
    // runs whichever way altGr was detected.
    bool altGr = qtMods.testFlag(Qt::GroupSwitchModifier);
    if (platform == KeyPlatform::Windows) {
        if (mods.has(Modifier::Ctrl) && mods.has(Modifier::Alt) &&
            context.keyState.isDown(input::PlatformVk::RightAlt) && context.layout.hasAltGr()) {
            altGr = true;
            mods = mods.without(Modifier::Ctrl).without(Modifier::Alt);
            if (context.keyState.isDown(input::PlatformVk::LeftAlt)) {
                mods = mods.with(Modifier::Alt);
            }
            if (context.keyState.isDown(input::PlatformVk::RightCtrl)) {
                mods = mods.with(Modifier::Ctrl);
            }
        }
    } else if (platform == KeyPlatform::X11 && (event.nativeModifiers() & kX11Mod5) != 0) {
        altGr = true;
    }
    out.modifiers = mods;
    out.altGr = altGr;

    out.imeComposing =
        context.ime.composing() || (platform == KeyPlatform::Windows && event.nativeVirtualKey() == kVkProcessKey);

    if (input::isNumpadKey(code)) {
        if ((key >= Qt::Key_0 && key <= Qt::Key_9) || key == Qt::Key_Period || key == Qt::Key_Comma) {
            out.numLock = true;
        } else if (isNavigationKey(key)) {
            out.numLock = false;
        }
    }

    // Text: at most two code points of QKeyEvent::text().
    const QString& text = event.text();
    std::uint8_t length = 0;
    for (qsizetype i = 0; i < text.size() && length < out.text.size(); ++i) {
        const char16_t unit = text.at(i).unicode();
        char32_t c = unit;
        if (QChar::isHighSurrogate(unit) && i + 1 < text.size() && QChar::isLowSurrogate(text.at(i + 1).unicode())) {
            c = QChar::surrogateToUcs4(unit, text.at(i + 1).unicode());
            ++i;
        }
        out.text[length++] = c;
    }
    out.textLength = length;

    // Dead keys.
    char32_t dead = 0;
    if (platform == KeyPlatform::Windows && out.isPress() && length == 0 && input::isWritingKey(code)) {
        const bool shift = mods.has(Modifier::Shift);
        const bool altGrLevel = altGr || reportedCtrlAlt;
        const Level level =
            altGrLevel ? (shift ? Level::AltGrShift : Level::AltGr) : (shift ? Level::Shift : Level::Base);
        const input::KeyLevels levels = context.layout.levels(code);
        if (levels.dead(level)) {
            dead = levels.at(level);
        }
    }
    if (dead == 0) {
        dead = deadKeyChar(key);
    }
    if (dead != 0 && out.isPress()) {
        out.deadKey = dead;
        out.text = {};
        out.textLength = 0;
    }

    if (platform != KeyPlatform::Windows && key >= Qt::Key_A && key <= Qt::Key_Z) {
        out.keyCharHint = U'a' + static_cast<char32_t>(key - Qt::Key_A);
    }
    return out;
}

} // namespace frost::ui
