// SPDX-License-Identifier: Apache-2.0
//
// The Windows thread sampler (frost/diag/thread_sampler.hpp): suspend, copy the stack, resume,
// unwind the copy. The technique follows Chromium's base/profiler (stack_copier_suspend.cc,
// stack_copier.cc's pointer rewriting, native_unwinder_win.cc; BSD-3-Clause): only the copy is
// unwound, so nothing that can block on a lock the suspended thread holds runs while it is
// suspended.

#include "frost/diag/thread_sampler.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>

#include <windows.h>

#if !defined(_M_X64)
#error "frost::diag's thread sampler supports x64 only"
#endif

namespace frost::diag {
namespace {

// The most stack bytes copied per sample (Budget: B19).
constexpr std::size_t kStackCopyBytes = std::size_t{1} << 20;

/*
 * Copies [top, top + bytes) of the suspended thread's stack into `copy`, rewriting every word
 * that points into the original range to the same offset in the copy, so the unwinder can
 * follow frame pointers and spilled stack addresses inside the copy. Not instrumented: the
 * original stack holds the target's ASan redzones, which this copy must read. Calls nothing.
 */
[[clang::no_sanitize("address")]] void copyAndRewrite(const std::uintptr_t* top, std::size_t words,
                                                      std::uintptr_t* copy) noexcept {
    const auto low = reinterpret_cast<std::uintptr_t>(top);
    const std::uintptr_t high = low + words * sizeof(std::uintptr_t);
    const auto delta = reinterpret_cast<std::uintptr_t>(copy) - low;
    for (std::size_t i = 0; i < words; ++i) {
        const std::uintptr_t value = static_cast<const volatile std::uintptr_t*>(top)[i];
        copy[i] = value >= low && value < high ? value + delta : value;
    }
}

void rewriteRegister(DWORD64& reg, std::uintptr_t low, std::uintptr_t high, std::uintptr_t copy) noexcept {
    if (reg >= low && reg < high) {
        reg = reg - low + copy;
    }
}

class WinThreadSampler final : public IThreadSampler {
public:
    WinThreadSampler(HANDLE thread, DWORD threadId, std::uintptr_t stackBase) noexcept
        : m_thread(thread), m_threadId(threadId), m_stackBase(stackBase),
          m_copy(std::make_unique_for_overwrite<std::uintptr_t[]>(kStackCopyBytes / sizeof(std::uintptr_t))) {}

    WinThreadSampler(const WinThreadSampler&) = delete ("owns a thread handle");
    WinThreadSampler& operator=(const WinThreadSampler&) = delete ("owns a thread handle");
    ~WinThreadSampler() override { CloseHandle(m_thread); }

    Result<void> sample(StackSample& out) noexcept override {
        out.count = 0;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        std::size_t words = 0;
        std::uintptr_t low = 0;
        // --- The target is suspended from here: no allocation, no lock, no loader call. ---
        if (SuspendThread(m_thread) == static_cast<DWORD>(-1)) {
            return fail(ErrorCode::IoError, std::format("SuspendThread failed ({})", GetLastError()));
        }
        const bool gotContext = GetThreadContext(m_thread, &context) != FALSE;
        if (gotContext) {
            low = static_cast<std::uintptr_t>(context.Rsp) & ~std::uintptr_t{7};
            if (low < m_stackBase) {
                const std::size_t bytes = std::min<std::size_t>(m_stackBase - low, kStackCopyBytes);
                words = bytes / sizeof(std::uintptr_t);
                copyAndRewrite(reinterpret_cast<const std::uintptr_t*>(low), words, m_copy.get());
            }
        }
        ResumeThread(m_thread);
        // --- Resumed. ---
        if (!gotContext) {
            return fail(ErrorCode::IoError, "GetThreadContext failed");
        }
        if (words == 0) {
            return fail(ErrorCode::IoError, "the sampled stack pointer is outside the thread's stack");
        }
        const std::uintptr_t high = low + words * sizeof(std::uintptr_t);
        const auto copy = reinterpret_cast<std::uintptr_t>(m_copy.get());
        for (DWORD64* reg : {&context.Rax, &context.Rbx, &context.Rcx, &context.Rdx, &context.Rsi, &context.Rdi,
                             &context.Rbp, &context.Rsp, &context.R8, &context.R9, &context.R10, &context.R11,
                             &context.R12, &context.R13, &context.R14, &context.R15}) {
            rewriteRegister(*reg, low, high, copy);
        }
        unwind(context, copy, copy + words * sizeof(std::uintptr_t), out);
        return {};
    }

    [[nodiscard]] std::uint64_t threadId() const noexcept override { return m_threadId; }

private:
    // Walks the copied stack with the image's unwind data (RtlLookupFunctionEntry /
    // RtlVirtualUnwind); a frame without unwind data is a leaf whose return address is at Rsp.
    static void unwind(CONTEXT& context, std::uintptr_t low, std::uintptr_t high, StackSample& out) noexcept {
        out.frames[out.count++] = static_cast<std::uintptr_t>(context.Rip);
        while (out.count < kMaxSampleFrames) {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (function != nullptr) {
                void* handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                       &establisherFrame, nullptr);
            } else {
                if (context.Rsp < low || context.Rsp + sizeof(DWORD64) > high) {
                    break;
                }
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += sizeof(DWORD64);
            }
            // Leaving the copy (or reaching the thread's first frame) ends the walk.
            if (context.Rip == 0 || context.Rsp < low || context.Rsp >= high) {
                break;
            }
            out.frames[out.count++] = static_cast<std::uintptr_t>(context.Rip);
        }
    }

    HANDLE m_thread;
    DWORD m_threadId;
    std::uintptr_t m_stackBase;
    std::unique_ptr<std::uintptr_t[]> m_copy;
};

} // namespace

Result<std::unique_ptr<IThreadSampler>> createSamplerForCurrentThread() {
    const HANDLE thread =
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    if (thread == nullptr) {
        return fail(ErrorCode::IoError, std::format("OpenThread failed ({})", GetLastError()));
    }
    ULONG_PTR stackLow = 0;
    ULONG_PTR stackHigh = 0;
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    return std::unique_ptr<IThreadSampler>(
        std::make_unique<WinThreadSampler>(thread, GetCurrentThreadId(), static_cast<std::uintptr_t>(stackHigh)));
}

} // namespace frost::diag
