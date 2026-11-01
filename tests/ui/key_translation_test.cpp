// SPDX-License-Identifier: Apache-2.0
//
// KeyTranslation: frost::ui::translateKeyEvent over constructed QKeyEvents (offscreen; no
// window is shown and nothing reaches the OS). Windows mode: scan codes and their traps,
// ShortcutOverride, the AltGr refinement with a scripted key-state probe, VK_PROCESSKEY,
// NumLock inference, dead keys, text, modifier normalisation, timestamps and fallback
// codes. X11 and Wayland modes: evdev codes, Mod5 and GroupSwitch, and keyCharHint.

#include "frost/core/clock.hpp"
#include "frost/input/key.hpp"
#include "frost/input/layout.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/ui/ime_tracker.hpp"
#include "frost/ui/key_translation.hpp"

#include <QEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QString>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string_view>

namespace frost::ui {
namespace {

using input::Code;
using input::KeyEvent;
using input::KeyPlatform;
using input::Modifier;
using input::Modifiers;
using input::PlatformVk;
using input::SimulatedLayoutId;

constexpr std::uint32_t kVkProcessKey = 0xE5;
constexpr std::uint32_t kX11Mod5 = 0x80;

struct Rig {
    explicit Rig(KeyPlatform p = KeyPlatform::Windows, SimulatedLayoutId id = SimulatedLayoutId::EnUs)
        : platform(p), layout(&input::simulatedLayout(id)) {}

    [[nodiscard]] KeyEvent translate(const QKeyEvent& event) const {
        return translateKeyEvent(event, KeyTranslationContext{platform, *layout, keys, ime, clock});
    }
    [[nodiscard]] KeyEvent press(int key, Qt::KeyboardModifiers mods, std::uint32_t scan, const QString& text = {},
                                 std::uint32_t vk = 0, std::uint32_t nativeMods = 0) const {
        return translate(QKeyEvent(QEvent::KeyPress, key, mods, scan, vk, nativeMods, text));
    }
    [[nodiscard]] KeyEvent release(int key, Qt::KeyboardModifiers mods, std::uint32_t scan) const {
        return translate(QKeyEvent(QEvent::KeyRelease, key, mods, scan, 0, 0));
    }
    // The native scan code Qt reports for `code` on this rig's platform.
    [[nodiscard]] std::uint32_t scanOf(Code code) const {
        return platform == KeyPlatform::Windows ? input::windowsScanCodeFor(code) : input::evdevFor(code) + 8U;
    }

    KeyPlatform platform;
    const input::LayoutTable* layout;
    input::ScriptedKeyStateProbe keys;
    ImeTracker ime;
    ManualClock clock;
};

std::u32string_view textOf(const KeyEvent& e) { return e.textView(); }

TEST(KeyTranslation, EveryWindowsScanCode) {
    const Rig rig;
    std::size_t mapped = 0;
    for (std::uint32_t scan = 0; scan < 0x200; ++scan) {
        const KeyEvent e = rig.press(0, Qt::NoModifier, scan);
        EXPECT_EQ(e.code, input::codeFromWindowsScanCode(static_cast<std::uint16_t>(scan))) << std::hex << scan;
        EXPECT_FALSE(e.codeFromFallback) << std::hex << scan;
        mapped += e.code != Code::Unknown ? 1U : 0U;
    }
    EXPECT_GT(mapped, 100U);
    // Every code with a scan code round-trips through the adapter.
    for (std::size_t i = 1; i < input::kCodeCount; ++i) {
        const auto code = static_cast<Code>(i);
        if (const std::uint16_t scan = input::windowsScanCodeFor(code); scan != 0) {
            EXPECT_EQ(rig.press(0, Qt::NoModifier, scan).code, code) << input::codeName(code);
        }
    }
    // Out of the 9-bit range.
    for (const std::uint32_t scan : {0x200U, 0x1E1EU, 0xFFFFU, 0xFFFFFFFFU}) {
        EXPECT_EQ(rig.press(0, Qt::NoModifier, scan).code, Code::Unknown) << std::hex << scan;
    }
}

TEST(KeyTranslation, WindowsScanCodeTraps) {
    const Rig rig;
    struct Case {
        std::uint32_t scan;
        Code code;
    };
    for (const Case c :
         {Case{0x145, Code::NumLock},       Case{0x045, Code::Pause},         Case{0x146, Code::Pause},
          Case{0x046, Code::ScrollLock},    Case{0x137, Code::PrintScreen},   Case{0x054, Code::PrintScreen},
          Case{0x01D, Code::ControlLeft},   Case{0x11D, Code::ControlRight},  Case{0x038, Code::AltLeft},
          Case{0x138, Code::AltRight},      Case{0x01C, Code::Enter},         Case{0x11C, Code::NumpadEnter},
          Case{0x035, Code::Slash},         Case{0x135, Code::NumpadDivide},  Case{0x047, Code::Numpad7},
          Case{0x147, Code::Home},          Case{0x053, Code::NumpadDecimal}, Case{0x153, Code::Delete},
          Case{0x056, Code::IntlBackslash}, Case{0x073, Code::IntlRo},        Case{0x07D, Code::IntlYen}}) {
        EXPECT_EQ(rig.press(0, Qt::NoModifier, c.scan).code, c.code) << std::hex << c.scan;
    }
}

TEST(KeyTranslation, ShortcutOverrideIsAPress) {
    const Rig rig;
    const KeyEvent override = rig.translate(
        QKeyEvent(QEvent::ShortcutOverride, Qt::Key_A, Qt::ControlModifier, 0x1E, 'A', 0, QStringLiteral("\x01")));
    EXPECT_TRUE(override.isPress());
    EXPECT_EQ(override.code, Code::KeyA);
    EXPECT_EQ(override.modifiers, Modifiers(Modifier::Ctrl));
    const KeyEvent release = rig.release(Qt::Key_A, Qt::ControlModifier, 0x1E);
    EXPECT_TRUE(release.isRelease());
    const KeyEvent repeat =
        rig.translate(QKeyEvent(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 0x1E, 'A', 0, QStringLiteral("a"), true));
    EXPECT_TRUE(repeat.autoRepeat);
}

TEST(KeyTranslation, AltGrRefinementOnWindows) {
    const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
    {
        // de AltGr+Q: Windows reports Ctrl+Alt; the physical keys are RightAlt and AltGr's fake LeftCtrl.
        Rig rig(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
        rig.keys.set(PlatformVk::RightAlt, true);
        rig.keys.set(PlatformVk::LeftCtrl, true);
        const KeyEvent e = rig.press(Qt::Key_At, ctrlAlt, 0x10, QStringLiteral("@"));
        EXPECT_TRUE(e.altGr);
        EXPECT_EQ(e.modifiers, Modifiers{});
        EXPECT_EQ(e.code, Code::KeyQ);
        EXPECT_EQ(textOf(e), U"@");
    }
    {
        // Left Ctrl + left Alt is a real Ctrl+Alt chord.
        Rig rig(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
        rig.keys.set(PlatformVk::LeftAlt, true);
        rig.keys.set(PlatformVk::LeftCtrl, true);
        const KeyEvent e = rig.press(Qt::Key_L, ctrlAlt, 0x26);
        EXPECT_FALSE(e.altGr);
        EXPECT_EQ(e.modifiers, Modifier::Ctrl | Modifier::Alt);
    }
    {
        // en-US has no AltGr: right Alt is Alt, and RightAlt with Ctrl stays Ctrl+Alt.
        Rig rig(KeyPlatform::Windows, SimulatedLayoutId::EnUs);
        rig.keys.set(PlatformVk::RightAlt, true);
        const KeyEvent alt = rig.press(Qt::Key_L, Qt::AltModifier, 0x26);
        EXPECT_FALSE(alt.altGr);
        EXPECT_EQ(alt.modifiers, Modifiers(Modifier::Alt));
        rig.keys.set(PlatformVk::LeftCtrl, true);
        const KeyEvent chord = rig.press(Qt::Key_L, ctrlAlt, 0x26);
        EXPECT_FALSE(chord.altGr);
        EXPECT_EQ(chord.modifiers, Modifier::Ctrl | Modifier::Alt);
    }
    {
        // AltGr + right Ctrl: AltGr plus a real Ctrl; AltGr + left Alt: AltGr plus Alt.
        Rig rig(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
        rig.keys.set(PlatformVk::RightAlt, true);
        rig.keys.set(PlatformVk::LeftCtrl, true);
        rig.keys.set(PlatformVk::RightCtrl, true);
        const KeyEvent ctrl = rig.press(Qt::Key_Q, ctrlAlt, 0x10);
        EXPECT_TRUE(ctrl.altGr);
        EXPECT_EQ(ctrl.modifiers, Modifiers(Modifier::Ctrl));
        rig.keys.set(PlatformVk::RightCtrl, false);
        rig.keys.set(PlatformVk::LeftAlt, true);
        const KeyEvent alt = rig.press(Qt::Key_Q, ctrlAlt | Qt::ShiftModifier, 0x10);
        EXPECT_TRUE(alt.altGr);
        EXPECT_EQ(alt.modifiers, Modifier::Alt | Modifier::Shift);
    }
    {
        // The windows:altgr platform option reports GroupSwitchModifier instead.
        Rig rig(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
        const KeyEvent e = rig.press(Qt::Key_At, Qt::GroupSwitchModifier, 0x10, QStringLiteral("@"));
        EXPECT_TRUE(e.altGr);
        EXPECT_EQ(e.modifiers, Modifiers{});
    }
    {
        // Without RightAlt down, Ctrl+Alt is never AltGr, whatever the layout.
        Rig rig(KeyPlatform::Windows, SimulatedLayoutId::PlPl);
        const KeyEvent e = rig.press(Qt::Key_L, ctrlAlt, 0x26);
        EXPECT_FALSE(e.altGr);
        EXPECT_EQ(e.modifiers, Modifier::Ctrl | Modifier::Alt);
    }
}

TEST(KeyTranslation, ImeComposition) {
    Rig rig;
    EXPECT_TRUE(rig.press(Qt::Key_unknown, Qt::NoModifier, 0x1E, {}, kVkProcessKey).imeComposing);
    EXPECT_FALSE(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E, QStringLiteral("a"), 'A').imeComposing);
    QInputMethodEvent preedit(QStringLiteral("ni"), {});
    rig.ime.observe(preedit);
    EXPECT_TRUE(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E, QStringLiteral("a"), 'A').imeComposing);
    Rig x11(KeyPlatform::X11);
    EXPECT_FALSE(x11.press(Qt::Key_A, Qt::NoModifier, x11.scanOf(Code::KeyA), {}, kVkProcessKey).imeComposing)
        << "VK_PROCESSKEY is a Windows convention";
}

TEST(KeyTranslation, NumLockInference) {
    const Rig rig;
    const auto keypad = Qt::KeypadModifier;
    struct Case {
        int key;
        std::uint32_t scan;
        Code code;
        bool numLock;
    };
    for (const Case c :
         {Case{Qt::Key_7, 0x47, Code::Numpad7, true}, Case{Qt::Key_Home, 0x47, Code::Numpad7, false},
          Case{Qt::Key_5, 0x4C, Code::Numpad5, true}, Case{Qt::Key_Clear, 0x4C, Code::Numpad5, false},
          Case{Qt::Key_Period, 0x53, Code::NumpadDecimal, true}, Case{Qt::Key_Comma, 0x53, Code::NumpadDecimal, true},
          Case{Qt::Key_Delete, 0x53, Code::NumpadDecimal, false}, Case{Qt::Key_Insert, 0x52, Code::Numpad0, false}}) {
        const KeyEvent e = rig.press(c.key, keypad, c.scan);
        EXPECT_EQ(e.code, c.code) << c.key;
        EXPECT_EQ(e.numLock, c.numLock) << c.key;
        EXPECT_EQ(e.modifiers, Modifiers{}) << "KeypadModifier is not a modifier";
    }
    // Navigation keys outside the numpad leave the default (on).
    EXPECT_TRUE(rig.press(Qt::Key_Home, Qt::NoModifier, 0x147).numLock);
    // Keypad operators keep the default.
    EXPECT_TRUE(rig.press(Qt::Key_Plus, keypad, 0x4E).numLock);
}

TEST(KeyTranslation, DeadKeysOnWindowsFromTheTable) {
    {
        const Rig rig(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
        const input::KeyLevels backquote = rig.layout->levels(Code::Backquote);
        ASSERT_TRUE(backquote.dead(input::Level::Base));
        // Qt reports no dead Qt::Key on some paths; the table decides.
        const KeyEvent e = rig.press(Qt::Key_unknown, Qt::NoModifier, 0x29);
        EXPECT_EQ(e.deadKey, U'^');
        EXPECT_EQ(e.textLength, 0);
        // Shift+Backquote is '°', not dead.
        const KeyEvent degree = rig.press(0x00B0, Qt::ShiftModifier, 0x29, QStringLiteral("°"));
        EXPECT_EQ(degree.deadKey, 0U);
        EXPECT_EQ(textOf(degree), U"°");
        // Shift+Equal is the dead grave.
        EXPECT_EQ(rig.press(Qt::Key_unknown, Qt::ShiftModifier, 0x0D).deadKey, U'`');
        // The release of a dead key is not a dead key.
        EXPECT_EQ(rig.release(Qt::Key_Dead_Circumflex, Qt::NoModifier, 0x29).deadKey, 0U);
    }
    {
        // fr AltGr+Digit2 is the dead tilde, selected by the refined AltGr level.
        Rig rig(KeyPlatform::Windows, SimulatedLayoutId::FrFr);
        ASSERT_TRUE(rig.layout->levels(Code::Digit2).dead(input::Level::AltGr));
        rig.keys.set(PlatformVk::RightAlt, true);
        rig.keys.set(PlatformVk::LeftCtrl, true);
        const KeyEvent e = rig.press(Qt::Key_unknown, Qt::ControlModifier | Qt::AltModifier, 0x03);
        EXPECT_TRUE(e.altGr);
        EXPECT_EQ(e.deadKey, U'~');
        // Physical Ctrl+Alt also selects the AltGr level on Windows.
        rig.keys.clear();
        rig.keys.set(PlatformVk::LeftAlt, true);
        rig.keys.set(PlatformVk::LeftCtrl, true);
        const KeyEvent chord = rig.press(Qt::Key_unknown, Qt::ControlModifier | Qt::AltModifier, 0x03);
        EXPECT_FALSE(chord.altGr);
        EXPECT_EQ(chord.deadKey, U'~');
    }
    {
        // A press with text is never a table dead key.
        const Rig rig(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
        EXPECT_EQ(rig.press(Qt::Key_AsciiCircum, Qt::NoModifier, 0x29, QStringLiteral("^^")).deadKey, 0U);
    }
}

TEST(KeyTranslation, DeadKeysFromQtKeys) {
    struct Case {
        int key;
        char32_t dead;
    };
    for (const KeyPlatform platform : {KeyPlatform::Windows, KeyPlatform::X11}) {
        const Rig rig(platform, SimulatedLayoutId::EnUs);
        for (const Case c :
             {Case{Qt::Key_Dead_Grave, U'`'}, Case{Qt::Key_Dead_Acute, U'´'}, Case{Qt::Key_Dead_Circumflex, U'^'},
              Case{Qt::Key_Dead_Tilde, U'~'}, Case{Qt::Key_Dead_Diaeresis, U'¨'}, Case{Qt::Key_Dead_Caron, U'ˇ'},
              Case{Qt::Key_Dead_Abovering, U'˚'}}) {
            const KeyEvent e = rig.press(c.key, Qt::NoModifier, rig.scanOf(Code::Quote), QStringLiteral("x"));
            EXPECT_EQ(e.deadKey, c.dead) << c.key;
            EXPECT_EQ(e.textLength, 0) << "a dead press carries no text";
        }
        EXPECT_EQ(rig.press(Qt::Key_Dead_Hook, Qt::NoModifier, rig.scanOf(Code::Quote)).deadKey, 0U)
            << "unlisted dead keys are ignored";
    }
}

TEST(KeyTranslation, TextUpToTwoCodePoints) {
    const Rig rig;
    EXPECT_EQ(textOf(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E, QStringLiteral("a"))), U"a");
    EXPECT_EQ(textOf(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E, QStringLiteral("ab"))), U"ab");
    EXPECT_EQ(textOf(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E, QStringLiteral("abc"))), U"ab");
    EXPECT_EQ(textOf(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E, QString::fromUcs4(U"\U0001F600", 1))), U"\U0001F600");
    EXPECT_EQ(textOf(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E, QString::fromUcs4(U"x\U0001F600y", 3))),
              U"x\U0001F600");
    EXPECT_EQ(textOf(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E, QString::fromUcs4(U"\U0001F600\U0001F601z", 3))),
              U"\U0001F600\U0001F601");
    EXPECT_EQ(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E).textLength, 0);
    // A lone surrogate is kept as one unit.
    const QString lone(QChar(char16_t{0xD83D}));
    EXPECT_EQ(rig.press(Qt::Key_A, Qt::NoModifier, 0x1E, lone).textLength, 1);
}

TEST(KeyTranslation, ModifierKeysAreNormalised) {
    for (const KeyPlatform platform : {KeyPlatform::Windows, KeyPlatform::X11}) {
        const Rig rig(platform);
        const std::uint32_t shift = rig.scanOf(Code::ShiftLeft);
        // X11 reports the state before the event, Windows the state after: both normalise.
        for (const Qt::KeyboardModifiers reported :
             {Qt::KeyboardModifiers(Qt::NoModifier), Qt::KeyboardModifiers(Qt::ShiftModifier)}) {
            EXPECT_EQ(rig.press(Qt::Key_Shift, reported, shift).modifiers, Modifiers(Modifier::Shift));
            EXPECT_EQ(rig.release(Qt::Key_Shift, reported, shift).modifiers, Modifiers{});
        }
        const std::uint32_t ctrl = rig.scanOf(Code::ControlRight);
        EXPECT_EQ(rig.press(Qt::Key_Control, Qt::ShiftModifier, ctrl).modifiers, Modifier::Ctrl | Modifier::Shift);
        EXPECT_EQ(rig.release(Qt::Key_Control, Qt::ControlModifier | Qt::ShiftModifier, ctrl).modifiers,
                  Modifiers(Modifier::Shift));
        const std::uint32_t meta = rig.scanOf(Code::MetaLeft);
        EXPECT_EQ(rig.press(Qt::Key_Meta, Qt::NoModifier, meta).modifiers, Modifiers(Modifier::Meta));
    }
}

TEST(KeyTranslation, Timestamps) {
    Rig rig;
    rig.clock.advance(std::chrono::seconds(3));
    QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 0x1E, 'A', 0, QStringLiteral("a"));
    event.setTimestamp(123'456);
    KeyEvent e = rig.translate(event);
    EXPECT_EQ(e.osTimeMs, 123'456U);
    EXPECT_EQ(e.time, rig.clock.steadyNow());
    event.setTimestamp(0x1'0000'0005ULL);
    e = rig.translate(event);
    EXPECT_EQ(e.osTimeMs, 5U) << "the OS counter is 32 bits";
}

TEST(KeyTranslation, FallbackCodesWithoutAScanCode) {
    struct Case {
        SimulatedLayoutId layout;
        int key;
        Qt::KeyboardModifiers mods;
        Code code;
    };
    for (const Case c : {Case{SimulatedLayoutId::EnUs, Qt::Key_A, Qt::ControlModifier, Code::KeyA},
                         Case{SimulatedLayoutId::DeDe, Qt::Key_Z, Qt::ControlModifier, Code::KeyY},
                         Case{SimulatedLayoutId::FrFr, Qt::Key_A, Qt::ControlModifier, Code::KeyQ},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Escape, Qt::NoModifier, Code::Escape},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Backtab, Qt::ShiftModifier, Code::Tab},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_F7, Qt::NoModifier, Code::F7},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_F24, Qt::NoModifier, Code::F24},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Slash, Qt::KeypadModifier, Code::NumpadDivide},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Slash, Qt::NoModifier, Code::Slash},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_3, Qt::KeypadModifier, Code::Numpad3},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Enter, Qt::KeypadModifier, Code::NumpadEnter},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Shift, Qt::ShiftModifier, Code::ShiftLeft},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_AltGr, Qt::NoModifier, Code::AltRight}}) {
        for (const KeyPlatform platform : {KeyPlatform::Windows, KeyPlatform::X11}) {
            const Rig rig(platform, c.layout);
            const KeyEvent e = rig.press(c.key, c.mods, 0);
            EXPECT_EQ(e.code, c.code) << c.key;
            EXPECT_TRUE(e.codeFromFallback) << c.key;
        }
    }
    const Rig rig;
    const KeyEvent backtab = rig.press(Qt::Key_Backtab, Qt::ShiftModifier, 0);
    EXPECT_EQ(backtab.modifiers, Modifiers(Modifier::Shift));
    for (const int unknown : {static_cast<int>(Qt::Key_LaunchMail), static_cast<int>(Qt::Key_unknown), 0}) {
        const KeyEvent e = rig.press(unknown, Qt::NoModifier, 0);
        EXPECT_EQ(e.code, Code::Unknown) << unknown;
        EXPECT_FALSE(e.codeFromFallback) << unknown;
    }
    // A scan code wins over the Qt::Key.
    const KeyEvent scanned = rig.press(Qt::Key_Z, Qt::ControlModifier, 0x2C);
    EXPECT_EQ(scanned.code, Code::KeyZ);
    EXPECT_FALSE(scanned.codeFromFallback);
}

// Synthesized shifted punctuation (Qt reports the shifted character) finds the key that
// produces it on the layout, so Ctrl+Shift+Key_Question still matches Ctrl+Shift+Slash.
TEST(KeyTranslation, FallbackCodesForShiftedCharacters) {
    struct Case {
        SimulatedLayoutId layout;
        int key;
        Code code;
    };
    for (const Case c : {Case{SimulatedLayoutId::EnUs, Qt::Key_Question, Code::Slash},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Underscore, Code::Minus},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Plus, Code::Equal},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Exclam, Code::Digit1},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Colon, Code::Semicolon},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_Less, Code::Comma},
                         Case{SimulatedLayoutId::EnUs, Qt::Key_BraceLeft, Code::BracketLeft},
                         Case{SimulatedLayoutId::DeDe, Qt::Key_Question, Code::Minus},
                         Case{SimulatedLayoutId::DeDe, Qt::Key_Plus, Code::BracketRight},
                         Case{SimulatedLayoutId::DeDe, Qt::Key_Adiaeresis, Code::Quote},
                         Case{SimulatedLayoutId::DeDe, Qt::Key_ssharp, Code::Minus},
                         Case{SimulatedLayoutId::FrFr, Qt::Key_Exclam, Code::Slash}}) {
        const Rig rig(KeyPlatform::Windows, c.layout);
        const KeyEvent e = rig.press(c.key, Qt::ControlModifier | Qt::ShiftModifier, 0);
        EXPECT_EQ(e.code, c.code) << std::hex << c.key << " on " << rig.layout->id();
        EXPECT_TRUE(e.codeFromFallback);
    }
    // The base level wins over Shift, and dead levels never match: de '^' is a dead base.
    const Rig de(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
    EXPECT_EQ(de.press(Qt::Key_AsciiCircum, Qt::NoModifier, 0).code, Code::Unknown);
    EXPECT_EQ(de.press(Qt::Key_Slash, Qt::ControlModifier, 0).code, Code::Unknown)
        << "the 11 US punctuation keys go through codeFor(); de has no Slash key";
    // Logical keys follow: Ctrl+Shift+Key_Question is Ctrl+Shift+Slash on en-US.
    const Rig us;
    const KeyEvent question = us.press(Qt::Key_Question, Qt::ControlModifier | Qt::ShiftModifier, 0);
    const auto stroke = input::strokeFromEvent(question, *us.layout);
    ASSERT_TRUE(stroke.has_value());
    EXPECT_EQ(*stroke, (input::Stroke{Modifier::Ctrl | Modifier::Shift, input::Key::Slash}));
}

// The AltGr key's own press on a Windows AltGr layout is AltGr, not Alt (as SimulatedKeyboard
// models it); on a layout without AltGr the right Alt key is Alt.
TEST(KeyTranslation, AltGrKeyPressItself) {
    const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
    Rig de(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
    de.keys.set(PlatformVk::RightAlt, true);
    de.keys.set(PlatformVk::LeftCtrl, true);
    // Qt reports the key as Key_Alt by default and as Key_AltGr (with GroupSwitchModifier)
    // under the windows:altgr option; both still carry the synthetic Ctrl+Alt.
    for (const int key : {Qt::Key_Alt, Qt::Key_AltGr}) {
        const KeyEvent press = de.press(key, ctrlAlt, 0x138);
        EXPECT_EQ(press.code, Code::AltRight) << key;
        EXPECT_TRUE(press.altGr) << key;
        EXPECT_EQ(press.modifiers, Modifiers{}) << key;
    }
    // A character typed with windows:altgr: GroupSwitchModifier plus the synthetic Ctrl+Alt.
    const KeyEvent at = de.press(Qt::Key_At, ctrlAlt | Qt::GroupSwitchModifier, 0x10, QStringLiteral("@"));
    EXPECT_TRUE(at.altGr);
    EXPECT_EQ(at.modifiers, Modifiers{});
    de.keys.set(PlatformVk::LeftAlt, true);
    EXPECT_EQ(de.press(Qt::Key_At, ctrlAlt | Qt::GroupSwitchModifier, 0x10, QStringLiteral("@")).modifiers,
              Modifiers(Modifier::Alt))
        << "a real left Alt held with AltGr stays";
    Rig us(KeyPlatform::Windows, SimulatedLayoutId::EnUs);
    us.keys.set(PlatformVk::RightAlt, true);
    const KeyEvent alt = us.press(Qt::Key_Alt, Qt::AltModifier, 0x138);
    EXPECT_FALSE(alt.altGr);
    EXPECT_EQ(alt.modifiers, Modifiers(Modifier::Alt));
}

TEST(KeyTranslation, X11EvdevCodes) {
    const Rig rig(KeyPlatform::X11);
    std::size_t mapped = 0;
    for (std::size_t i = 1; i < input::kCodeCount; ++i) {
        const auto code = static_cast<Code>(i);
        if (const std::uint16_t evdev = input::evdevFor(code); evdev != 0) {
            EXPECT_EQ(rig.press(0, Qt::NoModifier, evdev + 8U).code, code) << input::codeName(code);
            ++mapped;
        }
    }
    EXPECT_GT(mapped, 100U);
    for (std::uint32_t scan = 0; scan < 8; ++scan) {
        const KeyEvent e = rig.press(0, Qt::NoModifier, scan);
        EXPECT_EQ(e.code, Code::Unknown) << scan;
        EXPECT_FALSE(e.codeFromFallback) << scan;
    }
    for (const std::uint32_t scan : {0x108U, 0x200U, 0xFFFFFFFFU}) {
        EXPECT_EQ(rig.press(0, Qt::NoModifier, scan).code, Code::Unknown) << std::hex << scan;
    }
    // Windows scan codes mean nothing on X11: 0x1E + 8 is evdev 30, KEY_A.
    EXPECT_EQ(rig.press(0, Qt::NoModifier, 0x1E + 8).code, Code::KeyA);
}

TEST(KeyTranslation, X11AltGrAndKeyCharHint) {
    const Rig x11(KeyPlatform::X11, SimulatedLayoutId::DeDe);
    const std::uint32_t q = x11.scanOf(Code::KeyQ);
    const KeyEvent mod5 = x11.press(Qt::Key_At, Qt::NoModifier, q, QStringLiteral("@"), 0, kX11Mod5);
    EXPECT_TRUE(mod5.altGr);
    EXPECT_EQ(mod5.keyCharHint, 0U) << "Key_At is not a letter";
    const KeyEvent group = x11.press(Qt::Key_At, Qt::GroupSwitchModifier, q, QStringLiteral("@"));
    EXPECT_TRUE(group.altGr);
    const KeyEvent plain = x11.press(Qt::Key_Q, Qt::ControlModifier, q);
    EXPECT_FALSE(plain.altGr);
    EXPECT_EQ(plain.keyCharHint, U'q');
    EXPECT_EQ(plain.modifiers, Modifiers(Modifier::Ctrl));
    EXPECT_EQ(x11.press(Qt::Key_Z, Qt::NoModifier, x11.scanOf(Code::KeyY)).keyCharHint, U'z');
    EXPECT_EQ(x11.press(Qt::Key_F1, Qt::NoModifier, x11.scanOf(Code::F1)).keyCharHint, 0U);
    // X11 has no Ctrl+Alt refinement: Ctrl+Alt stays Ctrl+Alt even with RightAlt "down".
    Rig refined(KeyPlatform::X11, SimulatedLayoutId::DeDe);
    refined.keys.set(PlatformVk::RightAlt, true);
    refined.keys.set(PlatformVk::LeftCtrl, true);
    const KeyEvent ctrlAlt = refined.press(Qt::Key_Q, Qt::ControlModifier | Qt::AltModifier, q);
    EXPECT_FALSE(ctrlAlt.altGr);
    EXPECT_EQ(ctrlAlt.modifiers, Modifier::Ctrl | Modifier::Alt);

    const Rig windows(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
    EXPECT_EQ(windows.press(Qt::Key_Q, Qt::ControlModifier, 0x10).keyCharHint, 0U) << "0 on Windows";
    EXPECT_FALSE(windows.press(Qt::Key_At, Qt::NoModifier, 0x10, QStringLiteral("@"), 0, kX11Mod5).altGr)
        << "Mod5 is an X11 convention";
}

TEST(KeyTranslation, Wayland) {
    const Rig wayland(KeyPlatform::Wayland, SimulatedLayoutId::DeDe);
    const std::uint32_t q = wayland.scanOf(Code::KeyQ);
    EXPECT_EQ(wayland.press(0, Qt::NoModifier, q).code, Code::KeyQ) << "evdev + 8";
    EXPECT_EQ(wayland.press(Qt::Key_Q, Qt::NoModifier, q).keyCharHint, U'q');
    EXPECT_TRUE(wayland.press(Qt::Key_At, Qt::GroupSwitchModifier, q, QStringLiteral("@")).altGr);
    EXPECT_FALSE(wayland.press(Qt::Key_At, Qt::NoModifier, q, QStringLiteral("@"), 0, kX11Mod5).altGr)
        << "Mod5 is read on X11 only";
}

TEST(KeyTranslation, PlatformNames) {
#ifdef _WIN32
    constexpr KeyPlatform kNative = KeyPlatform::Windows;
#else
    constexpr KeyPlatform kNative = KeyPlatform::X11;
#endif
    EXPECT_EQ(keyPlatformFromName(QStringLiteral("windows")), KeyPlatform::Windows);
    EXPECT_EQ(keyPlatformFromName(QStringLiteral("xcb")), KeyPlatform::X11);
    EXPECT_EQ(keyPlatformFromName(QStringLiteral("wayland")), KeyPlatform::Wayland);
    EXPECT_EQ(keyPlatformFromName(QStringLiteral("wayland-egl")), KeyPlatform::Wayland);
    EXPECT_EQ(keyPlatformFromName(QStringLiteral("offscreen")), kNative);
    EXPECT_EQ(keyPlatformFromName(QStringLiteral("minimal")), kNative);
    EXPECT_EQ(keyPlatformFromName(QString()), kNative);
    EXPECT_EQ(keyPlatformFromName(QStringLiteral("Windows")), kNative) << "names are case-sensitive";
    EXPECT_EQ(currentKeyPlatform(), kNative) << "the tests run offscreen";
}

} // namespace
} // namespace frost::ui
