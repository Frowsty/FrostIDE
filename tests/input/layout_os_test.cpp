// SPDX-License-Identifier: Apache-2.0
//
// LayoutOs (input_layout_os_tests, label integration): read-only checks of the active-
// layout reader against the OS. On Windows the table readActiveLayout() builds is cross-
// checked with independent user32 queries on whatever layout is active, and compared
// with simulatedLayout(EnUs) when that layout is US (an independent oracle: the OS's own
// layout). Nothing here changes the user's input configuration: every call only queries.
// On Linux the reader returns the M1 LettersByLabel table (ADR 0016 decision 12).

#include "frost/input/layout.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::input {
namespace {

void expectInvariants(const LayoutTable& table) {
    for (std::size_t k = index(Key::A); k <= index(Key::Z); ++k) {
        EXPECT_TRUE(table.reachable(static_cast<Key>(k))) << keyName(static_cast<Key>(k));
    }
    for (std::size_t d = 0; d < 10; ++d) {
        EXPECT_EQ(table.logicalKey(static_cast<Code>(index(Code::Digit0) + d)),
                  static_cast<Key>(index(Key::Digit0) + d))
            << "digits are positional";
    }
}

#ifdef _WIN32

std::wstring layoutName() {
    std::array<wchar_t, KL_NAMELENGTH> name{};
    EXPECT_TRUE(GetKeyboardLayoutNameW(name.data()));
    return std::wstring(name.data());
}

// The scan code argument of MapVirtualKeyExW: extended keys carry the 0xE0 prefix.
UINT scanArgument(std::uint16_t scan) { return (scan & 0x100U) != 0 ? (0xE000U | (scan & 0xFFU)) : scan; }

TEST(LayoutOs, ReadsTheActiveLayoutWithoutChangingIt) {
    const HKL before = GetKeyboardLayout(0);
    const std::wstring nameBefore = layoutName();
    auto table = readActiveLayout();
    ASSERT_TRUE(table.has_value()) << table.error().message();
    EXPECT_EQ(GetKeyboardLayout(0), before);
    EXPECT_EQ(layoutName(), nameBefore);
    EXPECT_EQ(table->mappingMode(), MappingMode::FullTable);
    EXPECT_NE(table->id().find(':'), std::string_view::npos) << table->id();
    expectInvariants(*table);
    std::cout << "[ layout ] active layout " << table->id() << "\n";
}

TEST(LayoutOs, BaseCharactersAgreeWithMapVirtualKey) {
    auto table = readActiveLayout();
    ASSERT_TRUE(table.has_value());
    const HKL hkl = GetKeyboardLayout(0);
    std::size_t checked = 0;
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const auto code = static_cast<Code>(i);
        const UINT vk = MapVirtualKeyExW(scanArgument(windowsScanCodeFor(code)), MAPVK_VSC_TO_VK_EX, hkl);
        if (vk == 0) {
            EXPECT_EQ(table->levels(code), KeyLevels{}) << codeName(code);
            continue;
        }
        const KeyLevels levels = table->levels(code);
        if ((levels.ligatureMask & kDeadBase) != 0) {
            continue;
        }
        const UINT mapped = MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, hkl);
        const bool dead = (mapped & 0x80000000U) != 0;
        const auto ch = static_cast<char32_t>(mapped & 0xFFFFU);
        EXPECT_EQ(dead, levels.dead(Level::Base)) << codeName(code);
        if (levels.base >= U'a' && levels.base <= U'z') {
            EXPECT_EQ(ch, levels.base - 0x20) << codeName(code) << ": letters map to uppercase";
        } else if (levels.base < 0x10000) {
            EXPECT_TRUE(ch == levels.base || ch == simpleUpper(levels.base)) << codeName(code);
        }
        ++checked;
    }
    EXPECT_GE(checked, 40U);
}

TEST(LayoutOs, VkKeyScanFindsEveryCharacterWhereTheTableHasIt) {
    auto table = readActiveLayout();
    ASSERT_TRUE(table.has_value());
    const HKL hkl = GetKeyboardLayout(0);
    // The writing code of each virtual key on this layout.
    std::array<Code, 256> codeOfVk{};
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const auto code = static_cast<Code>(i);
        const UINT vk = MapVirtualKeyExW(scanArgument(windowsScanCodeFor(code)), MAPVK_VSC_TO_VK_EX, hkl);
        if (vk != 0 && vk < 256 && codeOfVk[vk] == Code::Unknown) {
            codeOfVk[vk] = code;
        }
    }
    std::size_t checked = 0;
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const KeyLevels levels = table->levels(static_cast<Code>(i));
        for (const Level level : {Level::Base, Level::Shift}) {
            const char32_t c = levels.at(level);
            if (c == 0 || c > 0xFFFF || levels.dead(level) ||
                (levels.ligatureMask & (1U << static_cast<unsigned>(level))) != 0) {
                continue;
            }
            const SHORT scanned = VkKeyScanExW(static_cast<WCHAR>(c), hkl);
            ASSERT_NE(scanned, -1) << "U+" << std::hex << static_cast<unsigned>(c);
            const auto vk = static_cast<std::uint8_t>(scanned & 0xFF);
            const auto shiftState = static_cast<unsigned>((scanned >> 8) & 0xFF);
            const Code code = codeOfVk[vk];
            ASSERT_NE(code, Code::Unknown) << "vk " << static_cast<unsigned>(vk);
            const Level found = shiftState == 0   ? Level::Base
                                : shiftState == 1 ? Level::Shift
                                : shiftState == 6 ? Level::AltGr
                                                  : Level::AltGrShift;
            EXPECT_EQ(table->levels(code).at(found), c)
                << "U+" << std::hex << static_cast<unsigned>(c) << " via " << codeName(code) << " state " << shiftState;
            ++checked;
        }
    }
    EXPECT_GE(checked, 80U);
}

TEST(LayoutOs, NamedScanCodesAgreeWithMapVirtualKey) {
    const HKL hkl = GetKeyboardLayout(0);
    struct Case {
        std::uint16_t scan;
        Code code;
        UINT vk;
    };
    const Case cases[] = {
        {0x001, Code::Escape, VK_ESCAPE},
        {0x00E, Code::Backspace, VK_BACK},
        {0x00F, Code::Tab, VK_TAB},
        {0x01C, Code::Enter, VK_RETURN},
        {0x039, Code::Space, VK_SPACE},
        {0x03A, Code::CapsLock, VK_CAPITAL},
        {0x03B, Code::F1, VK_F1},
        {0x058, Code::F12, VK_F12},
        {0x02A, Code::ShiftLeft, VK_LSHIFT},
        {0x036, Code::ShiftRight, VK_RSHIFT},
        {0x01D, Code::ControlLeft, VK_LCONTROL},
        {0x11D, Code::ControlRight, VK_RCONTROL},
        {0x038, Code::AltLeft, VK_LMENU},
        {0x138, Code::AltRight, VK_RMENU},
        {0x15B, Code::MetaLeft, VK_LWIN},
        {0x15C, Code::MetaRight, VK_RWIN},
        {0x147, Code::Home, VK_HOME},
        {0x14F, Code::End, VK_END},
        {0x149, Code::PageUp, VK_PRIOR},
        {0x151, Code::PageDown, VK_NEXT},
        {0x152, Code::Insert, VK_INSERT},
        {0x153, Code::Delete, VK_DELETE},
        {0x148, Code::ArrowUp, VK_UP},
        {0x150, Code::ArrowDown, VK_DOWN},
        {0x14B, Code::ArrowLeft, VK_LEFT},
        {0x14D, Code::ArrowRight, VK_RIGHT},
        {0x145, Code::NumLock, VK_NUMLOCK},
        {0x135, Code::NumpadDivide, VK_DIVIDE},
        {0x037, Code::NumpadMultiply, VK_MULTIPLY},
        {0x04A, Code::NumpadSubtract, VK_SUBTRACT},
        {0x04E, Code::NumpadAdd, VK_ADD},
        {0x15D, Code::ContextMenu, VK_APPS},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(codeFromWindowsScanCode(c.scan), c.code) << std::hex << c.scan;
        // Key messages flag NumLock as extended (0x145), but the OS's scan-to-VK table lists it
        // under the plain make code 0x45 (the E1-prefixed Pause shares that code).
        const UINT argument = c.code == Code::NumLock ? 0x45U : scanArgument(c.scan);
        EXPECT_EQ(MapVirtualKeyExW(argument, MAPVK_VSC_TO_VK_EX, hkl), c.vk) << std::hex << c.scan;
    }
}

TEST(LayoutOs, UsLayoutEqualsTheSimulatedTable) {
    const std::wstring klid = layoutName();
    if (klid != L"00000409") {
        GTEST_SKIP() << "active layout is " << std::string(klid.begin(), klid.end()) << "; en-US equality not run";
    }
    auto table = readActiveLayout();
    ASSERT_TRUE(table.has_value());
    const LayoutTable& simulated = simulatedLayout(SimulatedLayoutId::EnUs);
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const auto code = static_cast<Code>(i);
        EXPECT_EQ(table->levels(code), simulated.levels(code)) << codeName(code);
    }
    EXPECT_EQ(*table == simulated, true) << table->id();
}

#else

TEST(LayoutOs, LinuxReturnsTheM1Table) {
    auto table = readActiveLayout();
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(*table == linuxM1Layout(), true);
    EXPECT_EQ(table->mappingMode(), MappingMode::LettersByLabel);
    expectInvariants(*table);
}

#endif

} // namespace
} // namespace frost::input
