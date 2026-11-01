// SPDX-License-Identifier: Apache-2.0
//
// NativeKeyDelivery (Windows; ui_input_native_tests runs on Qt's "windows" platform
// plugin): key messages posted to a native window that is created but never shown, as Qt
// really delivers them, through translateKeyEvent. It pins the windows_native bit values
// and the property the AltGr refinement relies on: nativeModifiers() is captured when Qt
// processes the key message, so a key-state change retrieved later (an AltGr release
// during a UI stall) cannot turn AltGr text into a Ctrl+Alt shortcut, or the reverse.
/*
 * Isolation: messages go to this process's own window only; SetKeyboardState changes only
 * this thread's key-state table ("changes made to this table do not affect keyboard input
 * to any other thread"), and the test restores it. No system input is generated, no
 * keyboard layout is touched, and no window becomes visible or takes focus. Only
 * WM_KEYDOWN is posted (never WM_SYSKEYDOWN), so no menu or beep path is involved.
 */

#include "frost/core/clock.hpp"
#include "frost/input/key.hpp"
#include "frost/input/layout.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/ui/ime_tracker.hpp"
#include "frost/ui/key_translation.hpp"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QWindow>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <windows.h>

namespace frost::ui {
namespace {

using input::KeyEvent;
using input::Modifier;
using input::Modifiers;
namespace native = windows_native;

constexpr UINT kMarker = WM_APP + 0x4B; // "Apply the later key state now."
constexpr WPARAM kVkL = 'L';
constexpr LPARAM kLParamL = 0x00260001; // Repeat count 1, scan code 0x26 (L).

using KeyState = std::array<BYTE, 256>;

KeyState keyStateOf(std::initializer_list<int> virtualKeys) {
    KeyState state{};
    for (const int vk : virtualKeys) {
        state[static_cast<std::size_t>(vk)] = 0x80;
    }
    return state;
}

// One delivered QKeyEvent, as the shell's filter would see it.
struct Delivered {
    QEvent::Type type = QEvent::None;
    std::uint32_t nativeModifiers = 0;
    KeyEvent translated;
    bool afterMarker = false;
};

class Recorder final : public QObject {
public:
    explicit Recorder(const input::LayoutTable& layout) : m_layout(&layout) {}

    bool eventFilter(QObject* /*watched*/, QEvent* event) override {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) {
            const auto& key = static_cast<const QKeyEvent&>(*event);
            delivered.push_back(
                Delivered{.type = event->type(),
                          .nativeModifiers = key.nativeModifiers(),
                          .translated = translateKeyEvent(
                              key, KeyTranslationContext{input::KeyPlatform::Windows, *m_layout, m_ime, m_clock}),
                          .afterMarker = markerSeen});
        }
        return false;
    }

    std::vector<Delivered> delivered;
    bool markerSeen = false;

private:
    const input::LayoutTable* m_layout;
    ImeTracker m_ime;
    ManualClock m_clock;
};

// Sets this thread's key state to `later` when the marker is dispatched: after Qt has
// processed the key message posted before it, before Qt delivers that key's QKeyEvent.
class LaterState final : public QAbstractNativeEventFilter {
public:
    LaterState(Recorder& recorder, const KeyState& later) : m_recorder(&recorder), m_later(later) {}

    bool nativeEventFilter(const QByteArray& /*eventType*/, void* message, qintptr* /*result*/) override {
        if (static_cast<const MSG*>(message)->message == kMarker) {
            KeyState copy = m_later;
            static_cast<void>(SetKeyboardState(copy.data()));
            m_recorder->markerSeen = true;
        }
        return false;
    }

private:
    Recorder* m_recorder;
    KeyState m_later;
};

class NativeKeyDelivery : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(QGuiApplication::platformName(), QStringLiteral("windows"));
        ASSERT_NE(GetKeyboardState(m_saved.data()), 0);
        m_window.create(); // A native HWND; never shown, never activated.
        m_hwnd = reinterpret_cast<HWND>(m_window.winId());
        ASSERT_NE(m_hwnd, nullptr);
        ASSERT_FALSE(m_window.isVisible());
    }
    void TearDown() override {
        static_cast<void>(SetKeyboardState(m_saved.data()));
        EXPECT_FALSE(m_window.isVisible());
    }

    /*
     * Posts WM_KEYDOWN 'L' while this thread's key state is `atMessage`, then the marker
     * that switches it to `later`, and pumps events until Qt has delivered the key's
     * ShortcutOverride and KeyPress (at most 5 s).
     */
    std::vector<Delivered> deliver(const input::LayoutTable& layout, const KeyState& atMessage, const KeyState& later) {
        Recorder recorder(layout);
        LaterState laterState(recorder, later);
        QCoreApplication::instance()->installEventFilter(&recorder);
        QCoreApplication::instance()->installNativeEventFilter(&laterState);
        KeyState state = atMessage;
        EXPECT_NE(SetKeyboardState(state.data()), 0);
        EXPECT_NE(PostMessageW(m_hwnd, WM_KEYDOWN, kVkL, kLParamL), 0);
        EXPECT_NE(PostMessageW(m_hwnd, kMarker, 0, 0), 0);
        QElapsedTimer timer;
        timer.start();
        const auto keyPressSeen = [&] {
            for (const Delivered& d : recorder.delivered) {
                if (d.type == QEvent::KeyPress) {
                    return true;
                }
            }
            return false;
        };
        while (!(keyPressSeen() && recorder.markerSeen) && timer.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        QCoreApplication::instance()->removeNativeEventFilter(&laterState);
        QCoreApplication::instance()->removeEventFilter(&recorder);
        EXPECT_TRUE(keyPressSeen()) << "Qt delivered no KeyPress within 5 s";
        return recorder.delivered;
    }

    QWindow m_window;
    HWND m_hwnd = nullptr;
    KeyState m_saved{};
};

// The windows_native constants are the bits Qt reports for each left/right modifier.
TEST_F(NativeKeyDelivery, NativeModifierBits) {
    struct Case {
        const char* name;
        KeyState state;
        std::uint32_t bits;
    };
    const std::vector<Case> cases = {
        {"LShift", keyStateOf({VK_SHIFT, VK_LSHIFT}), native::kShiftLeft},
        {"RShift", keyStateOf({VK_SHIFT, VK_RSHIFT}), native::kShiftRight},
        {"LCtrl", keyStateOf({VK_CONTROL, VK_LCONTROL}), native::kControlLeft},
        {"RCtrl", keyStateOf({VK_CONTROL, VK_RCONTROL}), native::kControlRight},
        {"LAlt", keyStateOf({VK_MENU, VK_LMENU}), native::kAltLeft},
        {"RAlt", keyStateOf({VK_MENU, VK_RMENU}), native::kAltRight},
        {"LWin", keyStateOf({VK_LWIN}), native::kMetaLeft},
        {"RWin", keyStateOf({VK_RWIN}), native::kMetaRight},
        {"AltGr", keyStateOf({VK_CONTROL, VK_LCONTROL, VK_MENU, VK_RMENU}), native::kAltGr},
    };
    const input::LayoutTable& us = input::simulatedLayout(input::SimulatedLayoutId::EnUs);
    for (const Case& c : cases) {
        const std::vector<Delivered> delivered = deliver(us, c.state, keyStateOf({}));
        ASSERT_FALSE(delivered.empty()) << c.name;
        for (const Delivered& d : delivered) {
            EXPECT_EQ(d.nativeModifiers, c.bits) << c.name << (d.type == QEvent::KeyPress ? " KeyPress" : " Override");
            EXPECT_EQ(d.translated.code, input::Code::KeyL) << c.name;
        }
    }
}

// pl-PL AltGr+L whose AltGr release was retrieved before Qt delivered the press: the press
// is still AltGr ('ł' text), not Ctrl+Alt+L (Reformat Code).
TEST_F(NativeKeyDelivery, AltGrReleasedBeforeDeliveryStaysAltGr) {
    const input::LayoutTable& pl = input::simulatedLayout(input::SimulatedLayoutId::PlPl);
    const std::vector<Delivered> delivered =
        deliver(pl, keyStateOf({VK_CONTROL, VK_LCONTROL, VK_MENU, VK_RMENU}), keyStateOf({}));
    ASSERT_FALSE(delivered.empty());
    for (const Delivered& d : delivered) {
        EXPECT_TRUE(d.afterMarker) << "Qt delivered the key before the later message: the race is not exercised";
        EXPECT_TRUE(d.translated.altGr);
        EXPECT_EQ(d.translated.modifiers, Modifiers{});
        EXPECT_FALSE(input::strokeFromEvent(d.translated, pl).has_value());
    }
}

// The reverse: LCtrl+LAlt+L delivered after an AltGr press was retrieved stays Ctrl+Alt+L.
TEST_F(NativeKeyDelivery, CtrlAltBeforeAnAltGrPressStaysCtrlAlt) {
    const input::LayoutTable& pl = input::simulatedLayout(input::SimulatedLayoutId::PlPl);
    const std::vector<Delivered> delivered = deliver(pl, keyStateOf({VK_CONTROL, VK_LCONTROL, VK_MENU, VK_LMENU}),
                                                     keyStateOf({VK_CONTROL, VK_LCONTROL, VK_MENU, VK_RMENU}));
    ASSERT_FALSE(delivered.empty());
    for (const Delivered& d : delivered) {
        EXPECT_TRUE(d.afterMarker) << "Qt delivered the key before the later message: the race is not exercised";
        EXPECT_FALSE(d.translated.altGr);
        EXPECT_EQ(input::strokeFromEvent(d.translated, pl),
                  (input::Stroke{Modifier::Ctrl | Modifier::Alt, input::Key::L}));
    }
}

} // namespace
} // namespace frost::ui
