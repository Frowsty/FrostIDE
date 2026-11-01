// SPDX-License-Identifier: Apache-2.0
//
// Integration tests of readWindowsAppearance (frost/ui/model/os_appearance.hpp) against an
// independent oracle: reg.exe from the System32 directory, run through realProcessLauncher.
// Windows only.

#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"
#include "frost/ui/model/os_appearance.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

#include <windows.h>

namespace frost::ui {
namespace {

std::string regExePath() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    EXPECT_GT(length, 0U);
    EXPECT_LT(length, static_cast<UINT>(MAX_PATH));
    auto utf8 = io::fromWide(std::wstring_view(buffer, length));
    EXPECT_TRUE(utf8);
    return utf8.value_or("") + "\\reg.exe";
}

// Runs `reg.exe query <key> /v <value>` and returns its exit code and stdout.
io::ProcessResult regQuery(std::string key, std::string value) {
    io::ProcessSpec spec;
    spec.executable = regExePath();
    spec.arguments = {"query", std::move(key), "/v", std::move(value)};
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, "", std::chrono::seconds(10));
    EXPECT_TRUE(result) << result.error().describe();
    return result.value_or(io::ProcessResult{});
}

// The REG_DWORD value printed by reg.exe ("    AppsUseLightTheme    REG_DWORD    0x1").
std::optional<unsigned long> parseDword(const std::string& out) {
    const std::size_t type = out.find("REG_DWORD");
    if (type == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t hex = out.find("0x", type);
    if (hex == std::string::npos) {
        return std::nullopt;
    }
    return std::stoul(out.substr(hex + 2), nullptr, 16);
}

TEST(WindowsAppearance, MatchesRegExe) {
    const io::ProcessResult reg =
        regQuery("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", "AppsUseLightTheme");
    const OsAppearance os = readWindowsAppearance();
    if (reg.exitCode == 1) {
        // The value (or key) does not exist: Windows' default is the light apps mode.
        EXPECT_EQ(os.scheme, ColorScheme::Light);
        return;
    }
    ASSERT_EQ(reg.exitCode, 0) << reg.stderrBytes;
    const auto value = parseDword(reg.stdoutBytes);
    ASSERT_TRUE(value) << reg.stdoutBytes;
    EXPECT_EQ(os.scheme, *value == 0 ? ColorScheme::Dark : ColorScheme::Light);
}

TEST(WindowsAppearance, HighContrastMatchesRegExe) {
    const io::ProcessResult reg = regQuery("HKCU\\Control Panel\\Accessibility\\HighContrast", "Flags");
    const OsAppearance os = readWindowsAppearance();
    if (reg.exitCode == 1) {
        EXPECT_FALSE(os.highContrast) << "no HighContrast flags value";
        return;
    }
    ASSERT_EQ(reg.exitCode, 0) << reg.stderrBytes;
    // Flags is a REG_SZ holding a decimal number; bit 0 is HCF_HIGHCONTRASTON.
    const std::size_t type = reg.stdoutBytes.find("REG_SZ");
    ASSERT_NE(type, std::string::npos) << reg.stdoutBytes;
    const std::size_t digits = reg.stdoutBytes.find_first_of("0123456789", type);
    ASSERT_NE(digits, std::string::npos) << reg.stdoutBytes;
    const unsigned long flags = std::stoul(reg.stdoutBytes.substr(digits));
    EXPECT_EQ(os.highContrast, (flags & 1UL) != 0);
}

} // namespace
} // namespace frost::ui
