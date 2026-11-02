// SPDX-License-Identifier: Apache-2.0
//
// frost::vcs's private helpers (environment.hpp): the environment reader and ASCII folding.

#include "environment.hpp"

#include "frost/io/utf.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace frost::vcs::detail {

std::string environmentVariable(const char* name) {
#ifdef _WIN32
    auto wide = io::toWide(name);
    if (!wide) {
        return {};
    }
    const DWORD needed = GetEnvironmentVariableW(wide->c_str(), nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(wide->c_str(), value.data(), needed);
    if (length == 0 || length >= needed) {
        return {};
    }
    value.resize(length);
    auto utf8 = io::fromWide(value);
    return utf8 ? std::move(*utf8) : std::string();
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::string asciiLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

} // namespace frost::vcs::detail
