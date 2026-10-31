// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for the frost::sched tests: build-type detection (budgets are
// asserted only in optimised, unsanitised builds), the FROST_STRESS_FULL switch
// and bounded waiting, so a scheduler bug fails a test instead of hanging it.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace frost::sched::test {

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

// The value of environment variable `name`, or "" if unset.
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

// FROST_STRESS_FULL=1 selects the M0 exit scale for stress tests.
inline bool stressFull() { return environment("FROST_STRESS_FULL") == "1"; }

// Polls `predicate` until it is true or `timeout` passes; returns its last value.
template<class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return predicate();
        }
        std::this_thread::yield();
    }
    return true;
}

} // namespace frost::sched::test
