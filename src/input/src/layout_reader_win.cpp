// SPDX-License-Identifier: Apache-2.0
//
// The Win32 active-layout reader (layout.hpp; ADR 0016 decision 12).
/*
 * This file only queries: it reads the calling thread's current keyboard layout, and
 * never changes the user's input configuration (no layout is selected, loaded
 * or removed, and no input is generated). ToUnicodeEx runs with flag 0x4 ("do not change
 * keyboard state", Windows 10 1607 and later), so a dead key the user has pending is
 * neither consumed nor disturbed. Wide APIs only.
 */

#include "frost/input/layout.hpp"

#include <array>
#include <format>
#include <string>
#include <string_view>

#include <windows.h>

namespace frost::input {

namespace {

// UTF-8 from UTF-16; ids and locale names are ASCII in practice.
std::string narrowFrom(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string lastErrorText(DWORD error) {
    std::array<wchar_t, 512> buffer{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                                        buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    std::wstring_view text(buffer.data(), length);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.remove_suffix(1);
    }
    return std::format("{} (error {})", narrowFrom(text), error);
}

class Win32LayoutQuery final : public ILayoutQuery {
public:
    Win32LayoutQuery(HKL layout, std::string locale, std::string klid)
        : m_layout(layout), m_locale(std::move(locale)), m_klid(std::move(klid)) {}

    [[nodiscard]] std::uint8_t vkFromScan(std::uint16_t scan) const override {
        // MAPVK_VSC_TO_VK_EX takes the extended flag as the 0xE0 prefix byte.
        const UINT code = (scan & 0x100) != 0 ? (0xE000U | (scan & 0xFFU)) : scan;
        return static_cast<std::uint8_t>(MapVirtualKeyExW(code, MAPVK_VSC_TO_VK_EX, m_layout) & 0xFFU);
    }

    [[nodiscard]] int toUnicode(std::uint8_t vk, std::uint16_t scan, Level level,
                                std::span<char32_t, 4> out) const override {
        std::array<BYTE, 256> state{};
        if (level == Level::Shift || level == Level::AltGrShift) {
            state[VK_SHIFT] = 0x80;
        }
        if (level == Level::AltGr || level == Level::AltGrShift) {
            state[VK_CONTROL] = 0x80;
            state[VK_MENU] = 0x80;
        }
        std::array<wchar_t, 8> buffer{};
        constexpr UINT kDoNotChangeKeyboardState = 0x4;
        const int result = ToUnicodeEx(vk, scan & 0xFFU, state.data(), buffer.data(), static_cast<int>(buffer.size()),
                                       kDoNotChangeKeyboardState, m_layout);
        if (result == 0) {
            return 0;
        }
        const int units = result < 0 ? 1 : result;
        // Join UTF-16 surrogate pairs into code points.
        int count = 0;
        for (int i = 0; i < units && i < static_cast<int>(buffer.size()); ++i) {
            const auto unit = static_cast<char32_t>(buffer[static_cast<std::size_t>(i)]);
            char32_t c = unit;
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units) {
                const auto low = static_cast<char32_t>(buffer[static_cast<std::size_t>(i) + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    c = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                }
            }
            if (count < 4) {
                out[static_cast<std::size_t>(count)] = c;
            }
            ++count;
        }
        return result < 0 ? -1 : count;
    }

    [[nodiscard]] std::string_view localeName() const override { return m_locale; }
    [[nodiscard]] std::string_view klid() const override { return m_klid; }

private:
    HKL m_layout;
    std::string m_locale;
    std::string m_klid;
};

} // namespace

Result<LayoutTable> readActiveLayout() {
    const HKL layout = GetKeyboardLayout(0);
    if (layout == nullptr) {
        return fail(ErrorCode::IoError, "GetKeyboardLayout: " + lastErrorText(GetLastError()));
    }
    // The low word of the HKL is the input language.
    const auto language = static_cast<LANGID>(reinterpret_cast<std::uintptr_t>(layout) & 0xFFFFU);
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> localeBuffer{};
    const int localeLength = LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), localeBuffer.data(),
                                              static_cast<int>(localeBuffer.size()), 0);
    std::string locale =
        localeLength > 1
            ? narrowFrom(std::wstring_view(localeBuffer.data(), static_cast<std::size_t>(localeLength - 1)))
            : std::string("und");
    std::array<wchar_t, KL_NAMELENGTH> klidBuffer{};
    std::string klid = GetKeyboardLayoutNameW(klidBuffer.data()) != 0 ? narrowFrom(klidBuffer.data())
                                                                      : std::format("{:08X}", language);
    const Win32LayoutQuery query(layout, std::move(locale), std::move(klid));
    return buildLayoutTable(query);
}

} // namespace frost::input
