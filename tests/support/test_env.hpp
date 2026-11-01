// SPDX-License-Identifier: Apache-2.0
//
// Shared test environment helpers: build-type detection (budgets are asserted only in optimised,
// unsanitised builds), environment variables, and the FROST_STRESS_FULL switch that stress tests
// of every module use to select their milestone exit scale. Header-only; no gtest dependency, so
// fuzz harnesses may include it too.
#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace frost::test {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
inline constexpr bool kSanitized = true;
#else
inline constexpr bool kSanitized = false;
#endif
#else
inline constexpr bool kSanitized = false;
#endif

#ifdef NDEBUG
inline constexpr bool kOptimized = true;
#else
inline constexpr bool kOptimized = false;
#endif

// Timing budgets are enforced only where timings mean something.
inline constexpr bool kAssertBudgets = kOptimized && !kSanitized;

// The value of environment variable `name`, or "" if unset. Names and values are ASCII (flags and
// numbers); longer values than 255 characters read as unset.
inline std::string environment(const char* name) {
#ifdef _WIN32
    std::wstring wide(name, name + std::char_traits<char>::length(name)); // ASCII names only.
    wchar_t buffer[256];
    const DWORD length = GetEnvironmentVariableW(wide.c_str(), buffer, 256);
    if (length == 0 || length >= 256) {
        return {};
    }
    std::string value;
    for (DWORD i = 0; i < length; ++i) {
        value.push_back(static_cast<char>(buffer[i])); // ASCII values only (flags and numbers).
    }
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

// FROST_STRESS_FULL=1 selects the milestone exit scale for stress tests (M0: sched's 10k seeds and
// 100 pool iterations; M1: the piece tree's 10M-op differential).
inline bool stressFull() { return environment("FROST_STRESS_FULL") == "1"; }

} // namespace frost::test
