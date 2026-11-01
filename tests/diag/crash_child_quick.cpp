// SPDX-License-Identifier: Apache-2.0
//
// crash_child's quick_enforce contract (crash_child.cpp, mode contract-quick): a violated
// FROST_PRE traps at once (__builtin_trap), without the violation handler, and crashpad reports
// the trap (STATUS_ILLEGAL_INSTRUCTION on Windows, SIGILL on Linux).
#define FROST_CONTRACT_SEMANTIC 3
#include "frost/core/assert.hpp"

#include <cstdio>

namespace frost::test {

[[clang::noinline]] void contractQuickSite(int value) {
    FROST_PRE(value >= 0);
    std::printf("not reached %d\n", value);
}

} // namespace frost::test
