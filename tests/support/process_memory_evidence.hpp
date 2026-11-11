// SPDX-License-Identifier: Apache-2.0
// Independent Windows process-memory evidence for the real corpus gates, including commit and paging cost.
#pragma once

#include "frost/io/process.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>

namespace frost::test {

// Queries the live child directly. These counters are independent of the governor's sampled metrics.
inline void reportProcessMemoryEvidence(io::ProcessId pid, std::string_view phase, std::uint64_t indexElapsedMs,
                                        std::chrono::steady_clock::time_point attemptStarted,
                                        std::uint64_t governorRssCapBytes,
                                        std::optional<std::uint64_t> expectedResidentLimit = std::nullopt) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
    ASSERT_NE(process, nullptr) << "OpenProcess memory evidence failed: " << GetLastError();
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    const BOOL read =
        K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    const DWORD error = read ? ERROR_SUCCESS : GetLastError();
    SIZE_T minimum = 0;
    SIZE_T maximum = 0;
    DWORD flags = 0;
    const BOOL policyRead = GetProcessWorkingSetSizeEx(process, &minimum, &maximum, &flags);
    const DWORD policyError = policyRead ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    ASSERT_NE(read, FALSE) << "GetProcessMemoryInfo evidence failed: " << error;
    ASSERT_NE(policyRead, FALSE) << "GetProcessWorkingSetSizeEx evidence failed: " << policyError;
    EXPECT_GT(counters.PeakWorkingSetSize, 0U);
    EXPECT_LT(counters.PeakWorkingSetSize, governorRssCapBytes);
    if (expectedResidentLimit) {
        EXPECT_EQ(maximum, *expectedResidentLimit);
        EXPECT_NE(flags & QUOTA_LIMITS_HARDWS_MAX_ENABLE, 0U);
        EXPECT_EQ(flags & QUOTA_LIMITS_HARDWS_MIN_ENABLE, 0U);
    }
    const auto wallMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - attemptStarted)
            .count();
    std::printf("[corpus memory] phase=%.*s pid=%llu indexElapsedMs=%llu attemptWallElapsedMs=%lld "
                "currentRssBytes=%llu peakRssBytes=%llu "
                "privateCommitBytes=%llu peakPrivateCommitBytes=%llu pageFaultCount=%lu "
                "residentLimitBytes=%llu residentLimitFlags=%lu\n",
                static_cast<int>(phase.size()), phase.data(), static_cast<unsigned long long>(pid),
                static_cast<unsigned long long>(indexElapsedMs), static_cast<long long>(wallMs),
                static_cast<unsigned long long>(counters.WorkingSetSize),
                static_cast<unsigned long long>(counters.PeakWorkingSetSize),
                static_cast<unsigned long long>(counters.PrivateUsage),
                static_cast<unsigned long long>(counters.PeakPagefileUsage), counters.PageFaultCount,
                static_cast<unsigned long long>(maximum), flags);
    std::fflush(stdout);
}

} // namespace frost::test
#endif
