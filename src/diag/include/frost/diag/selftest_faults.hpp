// SPDX-License-Identifier: Apache-2.0
//
// The deliberate faults of `frost --selftest=crash` and `--selftest=freeze` (ADR 0022). They
// live in frost::diag, in a file of their own, so tools/symbolize.py can check that a crash
// dump or a freeze report names exactly these functions at their marker lines
// (FROST-SELFTEST-CRASH-SITE, FROST-SELFTEST-FREEZE-SITE in selftest_faults.cpp).
#pragma once

#include <chrono>

namespace frost::diag {

// The address selfTestCrash() writes to.
inline constexpr unsigned kSelfTestCrashAddress = 0xF00D;

// Writes through a volatile pointer to kSelfTestCrashAddress: an access violation (Windows,
// 0xC0000005) or SIGSEGV (Linux). Never returns. noinline.
[[noreturn]] void selfTestCrash() noexcept;

// Keeps the calling thread busy for `duration` without returning to its event loop, in a loop
// whose body calls a function the optimiser cannot remove, so stack samples land in this
// function. noinline.
void selfTestFreeze(std::chrono::milliseconds duration) noexcept;

} // namespace frost::diag
