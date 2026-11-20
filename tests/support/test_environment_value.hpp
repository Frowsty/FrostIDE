// SPDX-License-Identifier: Apache-2.0
// Read optional UTF-8 process settings for acceptance fixtures on Windows and Linux.
#pragma once

#include <optional>
#include <string>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <utility>

#include <windows.h>
#else
#include <cstdlib>
#endif

namespace frost::test {

// Read tool paths and other UTF-8 settings, preserving the distinction between absent and empty.
[[nodiscard]] inline std::optional<std::string> environmentValue(const char* name) {
#ifdef _WIN32
    const auto wideName = io::toWide(name);
    if (!wideName)
        return std::nullopt;

    std::wstring value(256, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(wideName->c_str(), value.data(), static_cast<DWORD>(value.size()));
        if (length == 0) {
            if (GetLastError() == ERROR_SUCCESS)
                return std::string{};
            return std::nullopt;
        }
        if (length < value.size()) {
            value.resize(length);
            auto utf8 = io::fromWide(value);
            if (utf8)
                return std::move(*utf8);
            return std::nullopt;
        }
        value.resize(length); // Win32 reports the required size including the terminator.
    }
#else
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

} // namespace frost::test
