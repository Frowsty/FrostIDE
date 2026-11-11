// SPDX-License-Identifier: Apache-2.0
//
// The Linux thread sampler (frost/diag/thread_sampler.hpp): a real-time signal makes the target
// walk its own stack in a signal handler.
//
/*
 * Async-signal safety. The handler calls only _Unwind_Backtrace / _Unwind_GetIP and sem_post.
 * libgcc's unwinder (GCC 12 and later) finds unwind tables through glibc's _dl_find_object
 * (glibc 2.35 and later), which takes no lock, and keeps frames registered through
 * __register_frame_info in a lock-free b-tree (GCC 13 and later); Ubuntu 24.04 ships GCC 14's
 * libgcc_s and glibc 2.39. An older unwinder that falls back to dl_iterate_phdr could deadlock
 * when the target is interrupted inside the loader; ThreadSampler.NeverDeadlocksUnderHeap-
 * AndLoaderChurn pins the current behaviour.
 *
 * One request at a time: a process-wide mutex serialises sample() calls and a single slot
 * carries the request to the handler. The slot's state machine (Idle -> Requested -> Running ->
 * Done) lets a timed-out sampler withdraw a request the handler has not picked up yet; once
 * the handler runs, the sampler waits for it to finish, so the slot is never written after
 * sample() returns.
 */

#include "frost/diag/thread_sampler.hpp"

#include <sys/syscall.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <format>
#include <mutex>

#include <semaphore.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

namespace frost::diag {
namespace {

enum SlotState : int { kIdle, kRequested, kRunning, kDone };

struct SampleSlot {
    std::atomic<int> state{kIdle};
    pid_t tid = 0;
    StackSample* out = nullptr;
    sem_t done;
};

constinit std::atomic<SampleSlot*> g_slot{nullptr};
constinit std::atomic<int> g_signal{0};

pid_t currentTid() noexcept { return static_cast<pid_t>(::syscall(SYS_gettid)); }

struct UnwindState {
    StackSample* out;
    std::uintptr_t interruptedIp;
    bool found;
};

_Unwind_Reason_Code collect(_Unwind_Context* context, void* argument) {
    auto& state = *static_cast<UnwindState*>(argument);
    const std::uintptr_t ip = _Unwind_GetIP(context);
    if (ip == 0) {
        return _URC_END_OF_STACK;
    }
    // Frames of the handler and the signal trampoline come first; the walk starts at the
    // interrupted instruction.
    if (!state.found) {
        if (ip != state.interruptedIp) {
            return state.out->count++ < 16 ? _URC_NO_REASON : _URC_END_OF_STACK;
        }
        state.found = true;
        state.out->count = 0;
    }
    state.out->frames[state.out->count++] = ip;
    return state.out->count == kMaxSampleFrames ? _URC_END_OF_STACK : _URC_NO_REASON;
}

void sampleHandler(int, siginfo_t*, void* rawContext) noexcept {
    const int savedErrno = errno;
    SampleSlot* slot = g_slot.load(std::memory_order_acquire);
    int expected = kRequested;
    if (slot != nullptr && slot->tid == currentTid() &&
        slot->state.compare_exchange_strong(expected, kRunning, std::memory_order_acq_rel)) {
        const auto* context = static_cast<const ucontext_t*>(rawContext);
        const auto ip = static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
        StackSample& out = *slot->out;
        out.count = 0;
        UnwindState state{&out, ip, false};
        (void)_Unwind_Backtrace(&collect, &state);
        if (!state.found) {
            // The unwinder could not step out of the signal frame: keep the interrupted
            // instruction alone.
            out.frames[0] = ip;
            out.count = 1;
        }
        slot->state.store(kDone, std::memory_order_release);
        (void)sem_post(&slot->done);
    }
    errno = savedErrno;
}

// Chooses and installs the sampling signal once: the first SIGRTMIN+k, k >= 4, still at SIG_DFL
// (glibc and sanitizers use the lowest real-time signals).
Result<int> samplingSignal() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int signal = SIGRTMIN + 4; signal <= SIGRTMAX; ++signal) {
            struct sigaction current{};
            if (::sigaction(signal, nullptr, &current) != 0 || current.sa_handler != SIG_DFL ||
                (current.sa_flags & SA_SIGINFO) != 0) {
                continue;
            }
            struct sigaction action{};
            action.sa_sigaction = &sampleHandler;
            action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
            sigemptyset(&action.sa_mask);
            if (::sigaction(signal, &action, nullptr) == 0) {
                g_signal.store(signal, std::memory_order_release);
                return;
            }
        }
    });
    const int signal = g_signal.load(std::memory_order_acquire);
    if (signal == 0) {
        return fail(ErrorCode::IoError, "no free real-time signal for the thread sampler");
    }
    return signal;
}

std::mutex& sampleMutex() {
    [[clang::no_destroy]] static std::mutex mutex;
    return mutex;
}

timespec deadlineAfter(long millis) noexcept {
    timespec now{};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    now.tv_nsec += millis * 1'000'000L;
    now.tv_sec += now.tv_nsec / 1'000'000'000L;
    now.tv_nsec %= 1'000'000'000L;
    return now;
}

// sem_clockwait on CLOCK_MONOTONIC, retried on EINTR; true when the semaphore was taken.
bool waitFor(sem_t& semaphore, long millis) noexcept {
    const timespec deadline = deadlineAfter(millis);
    for (;;) {
        if (sem_clockwait(&semaphore, CLOCK_MONOTONIC, &deadline) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

class PosixThreadSampler final : public IThreadSampler {
public:
    PosixThreadSampler(int signal, pid_t tid) noexcept : m_signal(signal), m_tid(tid) {
        m_slot->tid = tid;
        (void)sem_init(&m_slot->done, 0, 0);
    }
    PosixThreadSampler(const PosixThreadSampler&) = delete ("owns the request slot");
    PosixThreadSampler& operator=(const PosixThreadSampler&) = delete ("owns the request slot");
    ~PosixThreadSampler() override {
        // A slot a handler may still write (see sample()) is leaked rather than freed.
        if (!m_abandoned) {
            (void)sem_destroy(&m_slot->done);
            delete m_slot;
        }
    }

    Result<void> sample(StackSample& out) noexcept override {
        if (m_abandoned) {
            return fail(ErrorCode::IoError, "the thread sampler was abandoned after a stuck handler");
        }
        const std::scoped_lock lock(sampleMutex());
        out.count = 0;
        out.outcome = StackSample::Outcome::Empty;
        m_slot->out = &out;
        m_slot->state.store(kRequested, std::memory_order_release);
        g_slot.store(m_slot, std::memory_order_release);
        if (::syscall(SYS_tgkill, ::getpid(), m_tid, m_signal) != 0) {
            const int error = errno;
            m_slot->state.store(kIdle, std::memory_order_release);
            g_slot.store(nullptr, std::memory_order_release);
            return fail(ErrorCode::IoError, std::format("tgkill failed (errno {})", error));
        }
        bool taken = waitFor(m_slot->done, 20);
        if (!taken) {
            int expected = kRequested;
            if (m_slot->state.compare_exchange_strong(expected, kIdle, std::memory_order_acq_rel)) {
                g_slot.store(nullptr, std::memory_order_release);
                return fail(ErrorCode::Timeout, "the sampled thread did not run its handler within 20 ms");
            }
            // The handler is running (or just finished): wait for it.
            taken = waitFor(m_slot->done, 1000);
            if (!taken) {
                m_abandoned = true;
                return fail(ErrorCode::IoError, "the sampling handler did not finish");
            }
        }
        g_slot.store(nullptr, std::memory_order_release);
        m_slot->state.store(kIdle, std::memory_order_release);
        out.outcome = out.count != 0 ? StackSample::Outcome::Captured : StackSample::Outcome::Empty;
        return {};
    }

    [[nodiscard]] std::uint64_t threadId() const noexcept override { return static_cast<std::uint64_t>(m_tid); }

private:
    int m_signal;
    pid_t m_tid;
    SampleSlot* m_slot = new SampleSlot;
    bool m_abandoned = false;
};

} // namespace

Result<std::unique_ptr<IThreadSampler>> createSamplerForCurrentThread() {
    auto signal = samplingSignal();
    if (!signal) {
        return std::unexpected(std::move(signal.error()));
    }
    return std::unique_ptr<IThreadSampler>(std::make_unique<PosixThreadSampler>(*signal, currentTid()));
}

} // namespace frost::diag
