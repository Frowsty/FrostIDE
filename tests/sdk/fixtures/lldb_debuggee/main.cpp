// SPDX-License-Identifier: Apache-2.0
//
// The lldb-dap fixture debuggee: a small C++ program for tests/sdk/lldb_dap_test.cpp and spike
// S10 (tools/s10_dap_spike.py, ADR 0049). It is built with the test toolchain by tests/sdk, with
// cl.exe /Zi /Od by the spike on Windows and with clang -g on Linux.
/*
 * Usage: lldb_debuggee [run [<exit code>] | crash | abort | throw]
 *   run    (default) prints the working directory and the arguments, starts and joins a second
 *          std::thread, calls into worker.cpp and exits with <exit code> (default 0);
 *   crash  dereferences a null pointer; abort calls std::abort(); throw lets a C++ exception escape.
 * Locals in main cover what the debugger's Variables view must show: an int, a pointer, an array,
 * a struct, a std::string, a std::vector, a std::map, a std::unique_ptr, a std::optional and a
 * std::wstring.
 * Lines ending in a "BP:<name>" comment are breakpoint targets, found by their comment.
 * On Windows the error mode and the CRT's abort behaviour are set first, so a crash outside a
 * debugger never raises a Windows Error Reporting or CRT box.
 */

#include "debuggee.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    const std::string mode = argc > 1 ? argv[1] : "run";
    const int exitCode = argc > 2 ? std::atoi(argv[2]) : 0;
    std::printf("cwd=%s\n", std::filesystem::current_path().string().c_str());
    std::printf("argc=%d mode=%s\n", argc, mode.c_str());
    std::fflush(stdout);

    int counter = 41;
    int* const counterPointer = &counter;
    const int squares[4] = {0, 1, 4, 9};
    const debuggee::Point origin{3, 4};
    const std::string greeting = "hello frost";
    const std::vector<int> primes{2, 3, 5, 7, 11};
    const std::map<std::string, int> ranks{{"one", 1}, {"two", 2}};
    const auto boxed = std::make_unique<int>(99);
    const std::optional<int> maybe = 5;
    const std::wstring wide = L"wide frost";

    std::thread worker(debuggee::workerMain, &counter);
    worker.join();
    counter += 1;                                           // BP:main
    const int sum = debuggee::sumWithBias(origin, counter); // BP:call
    std::printf("sum=%d size=%zu greeting=%s ranks=%zu boxed=%d maybe=%d wide=%zu squares=%d pointer=%d\n", sum,
                primes.size(), greeting.c_str(), ranks.size(), *boxed, maybe.value_or(0), wide.size(), squares[3],
                *counterPointer);
    std::fflush(stdout);

    if (mode == "crash") {
        debuggee::crashWithNullPointer();
    } else if (mode == "abort") {
        std::abort();
    } else if (mode == "throw") {
        throw std::runtime_error("frost unhandled exception");
    }
    return exitCode; // BP:exit
}
