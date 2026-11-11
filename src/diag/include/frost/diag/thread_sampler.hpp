// SPDX-License-Identifier: Apache-2.0
//
// IThreadSampler: captures the call stack of one other thread of this process without its
// cooperation, for freeze reports (ADR 0022, D-I).
//
/*
 * Windows: the target is suspended (SuspendThread), its context read (GetThreadContext) and the
 *   live part of its stack, [Rsp, StackBase), copied into a preallocated 1 MiB buffer; the
 *   thread is then resumed and the copy unwound with RtlLookupFunctionEntry and bounded
 *   RtlVirtualUnwind2, after every value in the copy (and the context) that pointed into the
 *   original stack has been rewritten to point into the copy. Nothing that can take a lock runs
 *   while the target is suspended (no allocation, no loader or heap call), so a target holding
 *   the heap or loader lock cannot deadlock the sampler. The approach follows Chromium's
 *   base/profiler (stack_copier_suspend, native_unwinder_win; BSD-3-Clause). Temporary module
 *   references protect image/unwind data after resumption. Missing bounded-unwind API,
 *   dynamically registered unwind data and unsupported bounds are reported as Unsupported.
 *   An image lost after resumption discards that walk and permits up to three fresh captures;
 *   repeated image loss is Unsupported. Busy/ResumeFailed and other errors never retry.
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
    enum class Outcome : std::uint8_t { Empty, Captured, Busy, ResumeFailed };
    std::array<std::uintptr_t, kMaxSampleFrames> frames{};
    std::size_t count = 0;
    Outcome outcome = Outcome::Empty;

    // A completed attempt is not necessarily a capture. Never persist an unavailable attempt,
    // even if a faulty producer left frame bytes behind.
    [[nodiscard]] bool captured() const noexcept { return outcome == Outcome::Captured && count != 0; }
};

class IThreadSampler {
public:
    virtual ~IThreadSampler() = default;
    // Completes one sampling attempt. A usable capture requires out.captured(), not merely a
    // successful Result. Windows Busy/ResumeFailed outcomes have no frames and return success
    // without constructing Error: even an empty Error can allocate MSVC Debug iterator proxies
    // while the target still owns the heap lock. Errors safe to construct: Timeout (Linux:
    // the handler did not run in time),
    // IoError (the thread could not be suspended or read; it may have exited), Unsupported
    // (Windows: metadata has no stable image owner or cannot be unwound within the stack copy).
    [[nodiscard]] virtual Result<void> sample(StackSample& out) noexcept = 0;
    // The target's OS thread id.
    [[nodiscard]] virtual std::uint64_t threadId() const noexcept = 0;
};

// A sampler for the calling thread. Errors: IoError (no handle, no free signal), Unsupported.
[[nodiscard]] Result<std::unique_ptr<IThreadSampler>> createSamplerForCurrentThread();

/*
 * A test double: every sample() returns `frames` (truncated to kMaxSampleFrames) and counts the
 * call; `failWith` makes it fail instead, `unavailableWith` completes an unavailable attempt.
 * Not thread-safe beyond one sampling thread, like the
 * real samplers; `calls` may be read from any thread.
 */
class FakeThreadSampler final : public IThreadSampler {
public:
    FakeThreadSampler(std::initializer_list<std::uintptr_t> frames = {0x1000}, std::uint64_t threadId = 7)
        : m_threadId(threadId) {
        for (const std::uintptr_t frame : frames) {
            if (m_count < kMaxSampleFrames) {
                m_frames[m_count++] = frame;
            }
        }
    }

    std::atomic<std::size_t> calls{0};
    std::optional<ErrorCode> failWith;
    std::optional<StackSample::Outcome> unavailableWith;

    [[nodiscard]] Result<void> sample(StackSample& out) noexcept override {
        calls.fetch_add(1, std::memory_order_relaxed);
        out.count = 0;
        out.outcome = StackSample::Outcome::Empty;
        if (failWith) {
            return fail(*failWith, "FakeThreadSampler: scripted failure");
        }
        if (unavailableWith) {
            out.outcome = *unavailableWith;
            return {};
        }
        out.frames = m_frames;
        out.count = m_count;
        out.outcome = m_count != 0 ? StackSample::Outcome::Captured : StackSample::Outcome::Empty;
        return {};
    }
    [[nodiscard]] std::uint64_t threadId() const noexcept override { return m_threadId; }

private:
    std::array<std::uintptr_t, kMaxSampleFrames> m_frames{};
    std::size_t m_count = 0;
    std::uint64_t m_threadId;
};

} // namespace frost::diag
