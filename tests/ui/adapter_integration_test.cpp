// SPDX-License-Identifier: Apache-2.0
//
// AdapterIntegration: synthesized QKeyEvents through translateKeyEvent, ImeTracker and
// KeymapService with the built-in IntelliJ keymap, as the shell's event filter will run
// them (offscreen; no window, no OS input). Covers the M1 exit cases end to end: a Polish
// Ctrl+Alt chord fires, German AltGr text passes through, IME composition blocks the
// double-Shift gesture, and a QTest-style event without a scan code still fires.

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
using input::PlatformVk;
using input::SimulatedLayoutId;

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
    void start(KeyPlatform platform, SimulatedLayoutId layout) {
        registerIntelliJActions(m_registry);
        m_platform = platform;
        m_layout = &input::simulatedLayout(layout);
        auto service = input::KeymapService::create(m_registry, m_settings, input::KeymapPlatform::Windows, *m_layout);
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
        const KeyEvent translated = translateKeyEvent(
            static_cast<const QKeyEvent&>(event), KeyTranslationContext{m_platform, *m_layout, m_keys, m_ime, m_clock});
        return m_service->handle(translated, m_context);
    }
    MatchResult key(QEvent::Type type, int qtKey, Qt::KeyboardModifiers mods, std::uint32_t scan,
                    const QString& text = {}) {
        return *deliver(QKeyEvent(type, qtKey, mods, scan, 0, 0, text));
    }
    [[nodiscard]] std::string fired(const MatchResult& result) const {
        return result.outcome == MatchOutcome::Fire ? m_registry.info(result.action).id : std::string();
    }

    input::ActionRegistry m_registry;
    input::MemoryKeymapSettings m_settings;
    ManualClock m_clock;
    input::ScriptedKeyStateProbe m_keys;
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
    m_keys.set(PlatformVk::LeftCtrl, true);
    EXPECT_EQ(key(kPress, Qt::Key_Control, Qt::ControlModifier, 0x1D).outcome, MatchOutcome::PassThrough);
    m_keys.set(PlatformVk::LeftAlt, true);
    EXPECT_EQ(key(kPress, Qt::Key_Alt, ctrlAlt, 0x38).outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(fired(key(kPress, Qt::Key_L, ctrlAlt, 0x26)), "code.reformat");
    EXPECT_EQ(key(kRelease, Qt::Key_L, ctrlAlt, 0x26).outcome, MatchOutcome::PassThrough);

    // AltGr+L on the same layout types 'ł' instead.
    m_keys.clear();
    m_keys.set(PlatformVk::RightAlt, true);
    m_keys.set(PlatformVk::LeftCtrl, true);
    const MatchResult altGr = key(kPress, 0x0141, ctrlAlt, 0x26, QString::fromUtf8("\xC5\x82"));
    EXPECT_EQ(altGr.outcome, MatchOutcome::PassThrough);
}

TEST_F(AdapterIntegration, GermanAltGrQPassesThrough) {
    start(KeyPlatform::Windows, SimulatedLayoutId::DeDe);
    m_keys.set(PlatformVk::RightAlt, true);
    m_keys.set(PlatformVk::LeftCtrl, true);
    const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
    EXPECT_EQ(key(kPress, Qt::Key_Control, Qt::ControlModifier, 0x1D).outcome, MatchOutcome::PassThrough);
    EXPECT_EQ(key(kPress, Qt::Key_AltGr, ctrlAlt, 0x138).outcome, MatchOutcome::PassThrough);
    QKeyEvent at(kPress, Qt::Key_At, ctrlAlt, 0x10, 0, 0, QStringLiteral("@"));
    const KeyEvent translated =
        translateKeyEvent(at, KeyTranslationContext{m_platform, *m_layout, m_keys, m_ime, m_clock});
    EXPECT_EQ(input::typedText(translated, m_platform), U"@");
    EXPECT_EQ(deliver(at)->outcome, MatchOutcome::PassThrough);
}

TEST_F(AdapterIntegration, ImeCompositionBlocksDoubleShift) {
    start(KeyPlatform::Windows, SimulatedLayoutId::EnUs);
    const auto shiftTap = [&] {
        MatchResult last;
        last = key(kPress, Qt::Key_Shift, Qt::ShiftModifier, 0x2A);
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
}

TEST_F(AdapterIntegration, X11CtrlAltLFiresOnPolish) {
    start(KeyPlatform::X11, SimulatedLayoutId::PlPl);
    const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
    const std::uint32_t l = input::evdevFor(Code::KeyL) + 8U;
    EXPECT_EQ(fired(key(kPress, Qt::Key_L, ctrlAlt, l)), "code.reformat");
    // X11 AltGr (Mod5) + L types 'ł'.
    const MatchResult altGr =
        *deliver(QKeyEvent(kPress, 0x0142, Qt::NoModifier, l, 0, 0x80, QString::fromUtf8("\xC5\x82")));
    EXPECT_EQ(altGr.outcome, MatchOutcome::PassThrough);
}

} // namespace
} // namespace frost::ui
