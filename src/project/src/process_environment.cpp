// SPDX-License-Identifier: Apache-2.0
//
// processEnvironmentVariable (process_environment.hpp): GetEnvironmentVariableW on Windows,
// getenv elsewhere.

#include "process_environment.hpp"

#include "frost/io/utf.hpp"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::project::detail {

std::optional<std::string> processEnvironmentVariable(std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
#ifdef _WIN32
    auto wide = io::toWide(name);
    if (!wide) {
        return std::nullopt;
    }
    const DWORD needed = GetEnvironmentVariableW(wide->c_str(), nullptr, 0);
    if (needed == 0) {
        return std::nullopt;
    }
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(wide->c_str(), value.data(), needed);
    if (length == 0 || length >= needed) {
        return length == 0 ? std::optional<std::string>(std::string()) : std::nullopt;
    }
    value.resize(length);
    auto utf8 = io::fromWide(value);
    return utf8 ? std::optional<std::string>(std::move(*utf8)) : std::nullopt;
#else
    const std::string key(name);
    const char* value = std::getenv(key.c_str()); // NOLINT(concurrency-mt-unsafe): see the header.
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

} // namespace frost::project::detail
