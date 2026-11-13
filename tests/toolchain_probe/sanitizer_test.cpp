// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe: in AddressSanitizer builds, proves the instrumentation is
// live by requiring deliberate heap/global overflows to be reported, and checks that
// valid reads of empty/suffix literals pass. Always compiled; the tests are defined only
// when this translation unit is ASan-instrumented, so other
// configurations register no placeholder test.
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

const char* volatile g_emptyLiteral = "";
const char* volatile g_suffixLiteral = "suffix";
const char* volatile g_longLiteral = "prefix-suffix";
volatile char g_globalBuffer[4] = {};

TEST(ToolchainProbe, AddressSanitizerAllowsEmptyAndSuffixLiterals) {
    // Volatile pointers/reads keep the actual ASan-instrumented memory accesses present.
    // lld may otherwise fold a literal into another global's poisoned tail (LLVM issue 62078).
    const volatile char* empty = g_emptyLiteral;
    const volatile char* suffix = g_suffixLiteral;
    const volatile char* longer = g_longLiteral;
    EXPECT_EQ(empty[0], '\0');
    EXPECT_EQ(suffix[0], 's');
    EXPECT_EQ(suffix[6], '\0');
    EXPECT_EQ(longer[0], 'p');
    EXPECT_EQ(longer[13], '\0');
}

TEST(ToolchainProbeDeathTest, AddressSanitizerReportsGlobalOverflow) {
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            volatile unsigned index = sizeof(g_globalBuffer);
            g_globalBuffer[index] = 1; // One past the global, preserving the global-redzone check.
        },
        "global-buffer-overflow");
}

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
