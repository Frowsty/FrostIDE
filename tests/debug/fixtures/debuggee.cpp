// SPDX-License-Identifier: Apache-2.0
//
// The program the real lldb-dap test (lldb_session_test.cpp) debugs: it computes a local, prints it
// and exits with 0. The test sets a breakpoint on the line whose comment is the break marker and reads
// `local` there, so keep that line after the assignment. Uses only the C runtime.

#include <cstdio>

namespace {

int compute(int base) {
    const int local = base * 6;
    std::printf("local=%d\n", local); // FROST_BREAK_HERE
    return local;
}

} // namespace

int main() { return compute(7) == 42 ? 0 : 1; }
