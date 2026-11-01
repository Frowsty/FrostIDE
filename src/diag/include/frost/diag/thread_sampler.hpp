// SPDX-License-Identifier: Apache-2.0
//
// IThreadSampler: captures the call stack of one other thread of this process without its
// cooperation, for freeze reports (ADR 0022, D-I).
//
/*
 * Windows: the target is suspended (SuspendThread), its context read (GetThreadContext) and the
 *   live part of its stack, [Rsp, StackBase), copied into a preallocated 1 MiB buffer; the
 *   thread is then resumed and the copy unwound with RtlLookupFunctionEntry and
 *   RtlVirtualUnwind, after every value in the copy (and the context) that pointed into the
 *   original stack has been rewritten to point into the copy. Nothing that can take a lock runs
 *   while the target is suspended (no allocation, no loader or heap call), so a target holding
 *   the heap or loader lock cannot deadlock the sampler. The approach follows Chromium's
 *   base/profiler (stack_copier_suspend, native_unwinder_win; BSD-3-Clause).
 * Linux: a real-time signal (the first SIGRTMIN+k, k >= 4, whose action is SIG_DFL) is sent to
 *   the target with tgkill; its handler walks the stack with _Unwind_Backtrace into a
 *   preallocated slot and posts a semaphore, which the sampler waits for for at most 20 ms.
 *
 * Thread safety: one sampler serves one sampling thread (the watchdog); sample() is not
 * re-entrant. Create it on the target thread (it records that thread's identity there).
 */
#pragma once

#include "frost/core/error.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>

namespace frost::diag {

// The most frames one sample keeps (Budget: B19, with 200 samples per report).
inline constexpr std::size_t kMaxSampleFrames = 64;

// One stack: frames[0] is the instruction pointer, the rest are return addresses.
struct StackSample {
    std::array<std::uintptr_t, kMaxSampleFrames> frames{};
    std::size_t count = 0;
};

class IThreadSampler {
public:
    virtual ~IThreadSampler() = default;
    // Captures the target's stack. Errors: Timeout (Linux: the handler did not run in time),
    // IoError (the thread could not be suspended or read; it may have exited).
    [[nodiscard]] virtual Result<void> sample(StackSample& out) noexcept = 0;
    // The target's OS thread id.
    [[nodiscard]] virtual std::uint64_t threadId() const noexcept = 0;
};

// A sampler for the calling thread. Errors: IoError (no handle, no free signal), Unsupported.
[[nodiscard]] Result<std::unique_ptr<IThreadSampler>> createSamplerForCurrentThread();

/*
 * A test double: every sample() returns `frames` (truncated to kMaxSampleFrames) and counts the
 * call; `failWith` makes it fail instead. Not thread-safe beyond one sampling thread, like the
 * real samplers; `calls` may be read from any thread.
 */
class FakeThreadSampler final : public IThreadSampler {
public:
    explicit FakeThreadSampler(std::initializer_list<std::uintptr_t> frames = {0x1000}, std::uint64_t threadId = 7)
        : m_threadId(threadId) {
        for (const std::uintptr_t frame : frames) {
            if (m_count < kMaxSampleFrames) {
                m_frames[m_count++] = frame;
            }
        }
    }

    std::atomic<std::size_t> calls{0};
    std::optional<ErrorCode> failWith;

    [[nodiscard]] Result<void> sample(StackSample& out) noexcept override {
        calls.fetch_add(1, std::memory_order_relaxed);
        if (failWith) {
            return fail(*failWith, "FakeThreadSampler: scripted failure");
        }
        out.frames = m_frames;
        out.count = m_count;
        return {};
    }
    [[nodiscard]] std::uint64_t threadId() const noexcept override { return m_threadId; }

private:
    std::array<std::uintptr_t, kMaxSampleFrames> m_frames{};
    std::size_t m_count = 0;
    std::uint64_t m_threadId;
};

} // namespace frost::diag
