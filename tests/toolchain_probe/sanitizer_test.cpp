// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe: in AddressSanitizer builds, proves the instrumentation is
// live by requiring a deliberate heap overflow to be reported. Compiled into the
// probe only when ASan is enabled, so other configurations run no placeholder.
// CMake defines FROST_EXPECT_ASAN when FROST_SANITIZE contains "address", so a
// build whose sanitizer flags were lost fails to compile instead of silently
// dropping the probe.

#include "support/crash_dialogs.hpp"
#include <gtest/gtest.h>

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
#define FROST_ASAN_ACTIVE 1
#endif

#if defined(FROST_EXPECT_ASAN) && !defined(FROST_ASAN_ACTIVE)
#error "FROST_SANITIZE requests AddressSanitizer, but this translation unit is not instrumented"
#endif

#if defined(FROST_ASAN_ACTIVE)

namespace {

TEST(ToolchainProbeDeathTest, AddressSanitizerReportsHeapOverflow) {
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            char* volatile buffer = new char[4];
            buffer[4] = 1; // One past the end.
            delete[] buffer;
        },
        "heap-buffer-overflow");
}

} // namespace

#endif
