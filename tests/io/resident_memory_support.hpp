// SPDX-License-Identifier: Apache-2.0
// Native working-set assertions shared by the real blocking and asynchronous launcher regressions.
#pragma once

#ifdef _WIN32
#include "process_internal.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>

namespace frost::test {

// Keep the original 128 MiB peak budget and 256 MiB workload. Apply a lower exact OS quota because
// successful HARDWS_MAX readback alone does not establish the kernel's lifetime peak measurement.
inline constexpr std::uint64_t kResidentTestQuota = 112ULL * 1024 * 1024;
inline constexpr std::uint64_t kResidentTestPeakBudget = 128ULL * 1024 * 1024;
inline constexpr std::uint64_t kResidentTestAllocation = 256ULL * 1024 * 1024;

inline void expectResidentMemoryOracle(const io::ProcessResult& result) {
    ASSERT_EQ(result.exitCode, 0) << result.stderrBytes << result.stdoutBytes;
    std::istringstream report(result.stdoutBytes);
    std::string label;
    std::uint64_t firstMaximum = 0;
    DWORD firstFlags = 0;
    std::uint64_t peak = 0;
    std::uint64_t allocated = 0;
    int inJob = 0;
    std::uint64_t peakBudget = 0;
    ASSERT_TRUE(report >> label >> firstMaximum >> firstFlags >> peak >> allocated >> inJob >> peakBudget);
    EXPECT_EQ(label, "resident-memory");
    EXPECT_EQ(firstMaximum, kResidentTestQuota);
    EXPECT_NE(firstFlags & QUOTA_LIMITS_HARDWS_MAX_ENABLE, 0u);
    EXPECT_EQ(firstFlags & QUOTA_LIMITS_HARDWS_MAX_DISABLE, 0u);
    EXPECT_GT(peak, 0u);
    EXPECT_LT(peak, kResidentTestPeakBudget);
    EXPECT_EQ(allocated, kResidentTestAllocation);
    EXPECT_EQ(inJob, TRUE);
    EXPECT_EQ(peakBudget, kResidentTestPeakBudget);
    EXPECT_FALSE(report >> label);
}

// The callback duplicates real OS handles before a post-create failure. Keeping a process handle
// prevents PID reuse, and retaining only the read end independently proves all pipe writers closed.
class ResidentMemoryFailureProbe {
public:
    explicit ResidentMemoryFailureProbe(io::detail::ResidentMemoryLaunchFault fault) : m_fault(fault) {
        s_current = this;
        io::detail::setResidentMemoryLaunchHookForTesting(capture);
    }
    ~ResidentMemoryFailureProbe() {
        io::detail::setResidentMemoryLaunchHookForTesting(nullptr);
        s_current = nullptr;
        if (m_process.valid() && WaitForSingleObject(m_process.get(), 0) != WAIT_OBJECT_0) {
            TerminateProcess(m_process.get(), 1);
            WaitForSingleObject(m_process.get(), 5000);
        }
    }
    ResidentMemoryFailureProbe(const ResidentMemoryFailureProbe&) = delete ("owns probe handles");
    ResidentMemoryFailureProbe& operator=(const ResidentMemoryFailureProbe&) = delete ("owns probe handles");

    [[nodiscard]] bool capturedChild() const noexcept { return m_process.valid(); }

    void expectEndedWithoutExecuting() const {
        ASSERT_TRUE(m_process.valid());
        EXPECT_EQ(WaitForSingleObject(m_process.get(), 0), WAIT_OBJECT_0);
        ASSERT_TRUE(m_stdout.valid());
        DWORD available = 0;
        const BOOL peeked = PeekNamedPipe(m_stdout.get(), nullptr, 0, nullptr, &available, nullptr);
        const DWORD pipeError = GetLastError();
        EXPECT_EQ(peeked, FALSE);
        EXPECT_EQ(pipeError, ERROR_BROKEN_PIPE);
        EXPECT_EQ(available, 0u);
    }

private:
    static io::detail::ResidentMemoryLaunchFault capture(HANDLE process, HANDLE stdoutPipe) {
        HANDLE duplicate = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &duplicate, 0, FALSE,
                            DUPLICATE_SAME_ACCESS) != FALSE) {
            s_current->m_process.reset(duplicate);
        }
        duplicate = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), stdoutPipe, GetCurrentProcess(), &duplicate, 0, FALSE,
                            DUPLICATE_SAME_ACCESS) != FALSE) {
            s_current->m_stdout.reset(duplicate);
        }
        return s_current->m_fault;
    }

    inline static thread_local ResidentMemoryFailureProbe* s_current = nullptr;
    io::detail::ResidentMemoryLaunchFault m_fault;
    io::detail::UniqueHandle m_process;
    io::detail::UniqueHandle m_stdout;
};

} // namespace frost::test
#endif
