// SPDX-License-Identifier: Apache-2.0
//
// The second source file of the lldb-dap fixture debuggee: the worker thread's body, the
// function main steps into, and the null-pointer crash. Lines ending in a "BP:<name>" comment
// are where tests/sdk/lldb_dap_test.cpp and tools/s10_dap_spike.py set breakpoints; they find
// them by the comment, so the code may move.

#include "debuggee.hpp"

namespace debuggee {

int sumWithBias(const Point& point, int bias) {
    const int total = point.x + point.y + bias; // BP:other
    return total;
}

void workerMain(int* counter) {
    const int before = *counter; // BP:worker
    *counter = before + 1;
}

void crashWithNullPointer() {
    volatile int* nowhere = nullptr;
    *nowhere = 42; // FAULT:null
}

} // namespace debuggee
