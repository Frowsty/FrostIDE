// SPDX-License-Identifier: Apache-2.0
//
// Internal helper of the Windows probes: UTF-16 to UTF-8 for report strings.
// TODO(M1): use frost::io's UTF conversions once m0/io is merged.
#pragma once

#include <string>
#include <string_view>

#include <windows.h>

namespace frost::perf::win::detail {

// Converts UTF-16 to UTF-8; unpaired surrogates become U+FFFD. O(n).
inline std::string toUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int length = static_cast<int>(wide.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, utf8.data(), bytes, nullptr, nullptr);
    }
    return utf8;
}

} // namespace frost::perf::win::detail
