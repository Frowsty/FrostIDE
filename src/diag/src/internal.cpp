// SPDX-License-Identifier: Apache-2.0
//
// frost::diag's shared helpers (internal.hpp).

#include "internal.hpp"

#include "frost/diag/fatal_handlers.hpp"

#include <chrono>
#include <format>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <sys/syscall.h>

#include <cstdlib>

#include <pthread.h>
#include <unistd.h>
#endif

namespace frost::diag {

std::string compactUtc(IClock::SystemTime time) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(time);
    return std::format("{:%Y%m%dT%H%M%S}Z", seconds);
}

std::string isoUtcMillis(IClock::SystemTime time) {
    const auto millis = std::chrono::floor<std::chrono::milliseconds>(time);
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", millis);
}

std::string environmentValue(std::string_view name) {
#ifdef _WIN32
    auto wideName = io::toWide(name);
    if (!wideName) {
        return {};
    }
    const DWORD needed = GetEnvironmentVariableW(wideName->c_str(), nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD got = GetEnvironmentVariableW(wideName->c_str(), value.data(), needed);
    if (got == 0 || got >= needed) {
        return {};
    }
    value.resize(got);
    return io::fromWide(value).value_or(std::string());
#else
    const std::string key(name);
    const char* value = std::getenv(key.c_str()); // frost never calls setenv concurrently.
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::uint32_t currentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

std::uint64_t currentThreadId() noexcept {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}

void nameAndGuardThisThread(const char* name) noexcept {
#ifdef _WIN32
    wchar_t wide[32] = {};
    for (int i = 0; i < 31 && name[i] != '\0'; ++i) {
        wide[i] = static_cast<wchar_t>(name[i]); // ASCII only.
    }
    (void)SetThreadDescription(GetCurrentThread(), wide);
#else
    char shortName[16] = {};
    for (int i = 0; i < 15 && name[i] != '\0'; ++i) {
        shortName[i] = name[i];
    }
    (void)pthread_setname_np(pthread_self(), shortName);
#endif
    installThreadFatalHandlers();
}

} // namespace frost::diag
