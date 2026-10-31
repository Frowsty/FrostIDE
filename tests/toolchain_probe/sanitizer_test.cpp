// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe: in AddressSanitizer builds, proves the instrumentation is
// live by requiring a deliberate heap overflow to be reported. Compiled into the
// probe only when ASan is enabled, so other configurations run no placeholder.

#include "support/crash_dialogs.hpp"
#include <gtest/gtest.h>

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))

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
