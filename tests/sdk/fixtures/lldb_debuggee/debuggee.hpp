// SPDX-License-Identifier: Apache-2.0
//
// Shared declarations of the lldb-dap fixture debuggee (main.cpp and worker.cpp): the struct
// whose fields the debugger shows and the functions that live in the second source file, so a
// breakpoint there tests a line in a file other than main's.
#pragma once

namespace debuggee {

// A plain aggregate for the debugger's Variables view.
struct Point {
    int x;
    int y;
};

// x + y + bias. Holds the "BP:other" line.
int sumWithBias(const Point& point, int bias);

// The body of the second thread: adds 1 to *counter. Holds the "BP:worker" line.
void workerMain(int* counter);

// Dereferences a null pointer (the "crash" mode).
void crashWithNullPointer();

} // namespace debuggee
