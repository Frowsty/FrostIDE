// SPDX-License-Identifier: Apache-2.0
//
// The self-test faults (frost/diag/selftest_faults.hpp). tools/symbolize.py looks for the
// marker comments below, so keep each on the line of the faulting statement or the spin.

#include "frost/diag/selftest_faults.hpp"

#include <cstdint>
#include <cstdlib>

namespace frost::diag {
namespace {

// The call in selfTestFreeze's loop: opaque to the optimiser, so the loop stays a loop with a
// call in it, and each sample shows selfTestFreeze as the caller. Only the
// calling thread accesses counter. Volatile keeps its load/store observable
// without Debug STL atomic helper frames hiding that caller from the sampler.
[[clang::noinline]] void spinOnce(volatile std::uint64_t& counter) noexcept { counter = counter + 1; }

} // namespace

[[clang::noinline]] void selfTestCrash() noexcept {
    // A volatile store the compiler must emit, to an address no mapping covers.
    auto* volatile target = reinterpret_cast<volatile int*>(static_cast<std::uintptr_t>(kSelfTestCrashAddress));
    *target = 0x46524f53; // FROST-SELFTEST-CRASH-SITE
    std::abort();         // Not reached; [[noreturn]] needs it if the store somehow succeeds.
}

[[clang::noinline]] void selfTestFreeze(std::chrono::milliseconds duration) noexcept {
    volatile std::uint64_t counter = 0;
    const auto end = std::chrono::steady_clock::now() + duration;
    // The clock is read once per 4096 calls, so nearly every sample lands in spinOnce or this
    // loop rather than in the clock's own frames.
    while (std::chrono::steady_clock::now() < end) {
        for (int i = 0; i < 4096; ++i) {
            spinOnce(counter); // FROST-SELFTEST-FREEZE-SITE
        }
    }
}

} // namespace frost::diag
