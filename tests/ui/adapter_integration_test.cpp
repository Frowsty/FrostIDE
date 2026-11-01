// SPDX-License-Identifier: Apache-2.0
//
// AdapterIntegration: synthesized QKeyEvents through translateKeyEvent, ImeTracker and
// KeymapService with the built-in IntelliJ keymap, as the shell's event filter will run
// them (offscreen; no window, no OS input). Covers the M1 exit cases end to end: a Polish
// Ctrl+Alt chord fires, German AltGr text passes through, IME composition blocks the
// double-Shift gesture, a QTest-style event without a scan code still fires, and on Linux
// (the product's LettersByLabel table) the French ',' key is not the letter M.

#include "frost/core/clock.hpp"
#include "frost/input/action.hpp"
#include "frost/input/key_event.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/ui/ime_tracker.hpp"
#include "frost/ui/key_translation.hpp"

#include <QEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QString>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frost::ui {
namespace {

using input::ActionContext;
using input::Code;
using input::KeyEvent;
using input::KeyPlatform;
using input::MatchOutcome;
using input::MatchResult;
using input::SimulatedLayoutId;
namespace native = windows_native;

// The registry of every action the built-in keymap names, enabled everywhere.
void registerIntelliJActions(input::ActionRegistry& registry) {
    auto keymap = input::builtinKeymap("intellij");
    ASSERT_TRUE(keymap.has_value());
    for (const input::KeymapSection section : {input::KeymapSection::Default, input::KeymapSection::Windows,
                                               input::KeymapSection::Linux, input::KeymapSection::MacOS}) {
        for (const input::Binding& binding : keymap->section(section)) {
            if (registry.find(binding.actionId)) {
                continue;
            }
            input::ActionSpec spec;
            spec.id = binding.actionId;
            spec.name = binding.actionId;
            spec.handler = [](const ActionContext&, const input::InvokeInfo&) {};
            ASSERT_TRUE(registry.add(std::move(spec)).has_value()) << binding.actionId;
        }
    }
}

class AdapterIntegration : public ::testing::Test {
protected:
    // The product's set-up for `platform`: on Windows the active layout's full table (here
    // the simulated one) and the Windows keymap; on X11 linuxM1Layout() and the Linux keymap.
    void start(KeyPlatform platform, SimulatedLayoutId layout) {
        registerIntelliJActions(m_registry);
        m_platform = platform;
        const bool windows = platform == KeyPlatform::Windows;
        m_layout = windows ? &input::simulatedLayout(layout) : &input::linuxM1Layout();
        auto service = input::KeymapService::create(
            m_registry, m_settings, windows ? input::KeymapPlatform::Windows : input::KeymapPlatform::Linux, *m_layout);
        ASSERT_TRUE(service.has_value());
        m_service.emplace(std::move(*service));
    }

    // What the shell's filter does with one event: track IME state, reset the matcher on a
    // composition start, translate and match key events.
    std::optional<MatchResult> deliver(const QEvent& event) {
        m_ime.observe(event);
        if (m_ime.takeCompositionStarted()) {
            m_service->reset();
        }
        const QEvent::Type type = event.type();
        if (type != QEvent::KeyPress && type != QEvent::KeyRelease && type != QEvent::ShortcutOverride) {
            return std::nullopt;
        }
        const KeyEvent translated = translateKeyEvent(static_cast<const QKeyEvent&>(event),
                                                      KeyTranslationContext{m_platform, *m_layout, m_ime, m_clock});
        return m_service->handle(translated, m_context);
    }
    // A key event with the native modifiers Qt captured for it (windows_native bits on Windows).
    MatchResult key(QEvent::Type type, int qtKey, Qt::KeyboardModifiers mods, std::uint32_t scan,
                    std::uint32_t nativeMods = 0, const QString& text = {}) {
        return *deliver(QKeyEvent(type, qtKey, mods, scan, 0, nativeMods, text));
    }
    [[nodiscard]] std::string fired(const MatchResult& result) const {
        return result.outcome == MatchOutcome::Fire ? m_registry.info(result.action).id : std::string();
    }

    input::ActionRegistry m_registry;
    input::MemoryKeymapSettings m_settings;
    ManualClock m_clock;
    ImeTracker m_ime;
    KeyPlatform m_platform = KeyPlatform::Windows;
    const input::LayoutTable* m_layout = nullptr;
    std::optional<input::KeymapService> m_service;
    ActionContext m_context{.keys = input::ContextKeys::all(), .level = input::AvailabilityLevel::L4, .trusted = true};
};

constexpr auto kPress = QEvent::KeyPress;
constexpr auto kRelease = QEvent::KeyRelease;

TEST_F(AdapterIntegration, PolishCtrlAltLFires) {
    start(KeyPlatform::Windows, SimulatedLayoutId::PlPl);
    const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
    const std::uint32_t chord = native::kControlLeft | native::kAltLeft;
    EXPECT_EQ(key(kPress, Qt::Key_Control, Qt::ControlModifier, 0x1D, native::kControlLeft).outcome,
              MatchOutcome::PassThrough);
    EXPECT_EQ(key(kPress, Qt::Key_Alt, ctrlAlt, 0x38, chord).outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(fired(key(kPress, Qt::Key_L, ctrlAlt, 0x26, chord)), "code.reformat");
    EXPECT_EQ(key(kRelease, Qt::Key_L, ctrlAlt, 0x26, chord).outcome, MatchOutcome::PassThrough);

    // AltGr+L on the same layout types 'ł' instead.
    const MatchResult altGr = key(kPress, 0x0141, ctrlAlt, 0x26, native::kAltGr, QString::fromUtf8("\xC5\x82"));
    EXPECT_EQ(altGr.outcome, MatchOutcome::PassThrough);
}

TEST_F(AdapterIntegration, GermanAltGrQPassesThrough) {
    start(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
    const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
    EXPECT_EQ(key(kPress, Qt::Key_Control, Qt::ControlModifier, 0x1D, native::kControlLeft).outcome,
              MatchOutcome::PassThrough);
    EXPECT_EQ(key(kPress, Qt::Key_AltGr, ctrlAlt, 0x138, native::kAltGr).outcome, MatchOutcome::PassThrough);
    QKeyEvent at(kPress, Qt::Key_At, ctrlAlt, 0x10, 0, native::kAltGr, QStringLiteral("@"));
    const KeyEvent translated = translateKeyEvent(at, KeyTranslationContext{m_platform, *m_layout, m_ime, m_clock});
    EXPECT_EQ(input::typedText(translated, m_platform), U"@");
    EXPECT_EQ(deliver(at)->outcome, MatchOutcome::PassThrough);
}

TEST_F(AdapterIntegration, ImeCompositionBlocksDoubleShift) {
    start(KeyPlatform::Windows, SimulatedLayoutId::EnUs);
    const auto shiftTap = [&] {
        MatchResult last;
        last = key(kPress, Qt::Key_Shift, Qt::ShiftModifier, 0x2A, native::kShiftLeft);
        last = key(kRelease, Qt::Key_Shift, Qt::NoModifier, 0x2A);
        return last;
    };
    {
        const QInputMethodEvent preedit(QStringLiteral("ni"), {});
        EXPECT_FALSE(deliver(preedit).has_value());
    }
    EXPECT_TRUE(m_ime.composing());
    EXPECT_EQ(shiftTap().outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(shiftTap().outcome, MatchOutcome::PassThrough) << "no gesture while composing";

    // After the commit the gesture works again.
    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("\xE4\xBD\xA0"));
    EXPECT_FALSE(deliver(commit).has_value());
    EXPECT_FALSE(m_ime.composing());
    EXPECT_EQ(shiftTap().outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(fired(shiftTap()), "search.everywhere");
}

TEST_F(AdapterIntegration, QTestStyleEventWithoutScanCodeFires) {
    start(KeyPlatform::Windows, SimulatedLayoutId::PlPl);
    // QTest::keyClick builds events with the short constructor: no scan code, no virtual key.
    const MatchResult result = *deliver(QKeyEvent(kPress, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier));
    EXPECT_EQ(fired(result), "search.findAction");
    // The same through ShortcutOverride, which the shell sees first.
    const MatchResult override = *deliver(QKeyEvent(QEvent::ShortcutOverride, Qt::Key_S, Qt::ControlModifier));
    EXPECT_EQ(fired(override), "file.saveAll");
    // Synthesized Ctrl+Alt carries no per-side bits, so it is Ctrl+Alt even on an AltGr layout.
    const MatchResult ctrlAlt = *deliver(QKeyEvent(kPress, Qt::Key_L, Qt::ControlModifier | Qt::AltModifier));
    EXPECT_EQ(fired(ctrlAlt), "code.reformat");
}

TEST_F(AdapterIntegration, X11CtrlAltLFiresOnPolish) {
    start(KeyPlatform::X11, SimulatedLayoutId::PlPl);
    const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
    const std::uint32_t l = input::evdevFor(Code::KeyL) + 8U;
    EXPECT_EQ(fired(key(kPress, Qt::Key_L, ctrlAlt, l)), "code.reformat");
    // X11 AltGr (Mod5) + L types 'ł'.
    const MatchResult altGr = key(kPress, 0x0142, Qt::NoModifier, l, 0x80, QString::fromUtf8("\xC5\x82"));
    EXPECT_EQ(altGr.outcome, MatchOutcome::PassThrough);
}

// French AZERTY on Linux: the key typing ',' (code KeyM; Qt reports Key_Comma) is Comma,
// and the key typing 'm' (code Semicolon) is M, so Ctrl+Alt+',' is not Extract Method and
// Ctrl+Shift+'?' is not Matching Brace.
TEST_F(AdapterIntegration, X11FrenchCommaKeyIsNotTheLetterM) {
    start(KeyPlatform::X11, SimulatedLayoutId::FrFr);
    const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
    const Qt::KeyboardModifiers ctrlShift = Qt::ControlModifier | Qt::ShiftModifier;
    const std::uint32_t commaKey = input::evdevFor(Code::KeyM) + 8U;
    const std::uint32_t mKey = input::evdevFor(Code::Semicolon) + 8U;
    EXPECT_EQ(fired(key(kPress, Qt::Key_Comma, ctrlAlt, commaKey)), "");
    EXPECT_EQ(fired(key(kPress, Qt::Key_M, ctrlAlt, mKey)), "refactor.extractMethod");
    EXPECT_EQ(fired(key(kPress, Qt::Key_Question, ctrlShift, commaKey)), "");
    EXPECT_EQ(fired(key(kPress, Qt::Key_M, ctrlShift, mKey)), "navigate.matchingBrace");
    // AZERTY letters follow their labels: the key typing 'a' (code KeyQ) is A.
    EXPECT_EQ(fired(key(kPress, Qt::Key_A, ctrlShift, input::evdevFor(Code::KeyQ) + 8U)), "search.findAction");
}

} // namespace
} // namespace frost::ui
