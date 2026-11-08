// SPDX-License-Identifier: Apache-2.0
//
// The program the dap-real scenario suite (tests/debug/dap_real_test.cpp, DBG-1 and DBG-2a) debugs
// under the pinned lldb-dap: built with clang-cl and a PDB on Windows, clang and DWARF on Linux,
// without optimisation. argv[1] picks what it does; every mode prints what it computed, so the
// tests compare what lldb-dap reports with the program's own output (the oracle), never with
// numbers written into the test.
/*
 * Modes (argv[1]; none means "values"):
 *   values        builds nested structs, a std::string and a std::vector, prints them, then loops
 *                 ten times through accumulate() and exits with the sum modulo 256.
 *   exit <n>      exits with code n (decimal).
 *   output        writes one line to stdout and one to stderr, then exits with 0.
 *   throw         throws std::runtime_error from thrower() and catches it in main (exit 0).
 *   threads       starts two worker threads that each call worker_body(), joins them, exits 0.
 *   spin          spins until g_release becomes non-zero (set it from the debugger) or 120 s pass;
 *                 exits 0 when released, 3 on the timeout.
 *   wait          prints "pid=<pid>" and then spins like "spin" (the attach scenarios). On Linux it
 *                 first allows any tracer (prctl PR_SET_PTRACER_ANY): with the default Yama policy
 *                 (ptrace_scope 1) a debugger that is not its ancestor cannot attach otherwise.
 *   setvar        stops on the set-variable marker; prints "changed=<value>" of a local the test
 *                 may change with setVariable, then exits 0.
 *   deep          recurses five levels into recurse() and exits 0 (step out, stack depth).
 *   args <a>...   prints every argument as "arg<i>=<text>" and exits with the argument count.
 *   env           prints FROST_SCENARIO_ENV as "env=<value>" and exits 0.
 *   cwd           prints the working directory as "cwd=<path>" and exits 0.
 * The markers ("// MARK: <name>") name the lines the tests break on; tests find them in this file,
 * so keep each on the line it describes.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define FROST_NOINLINE __declspec(noinline)
#else
#include <sys/prctl.h>
#include <unistd.h>
#define FROST_NOINLINE __attribute__((noinline))
#endif

struct Inner {
    int count;
    double ratio;
};

struct Outer {
    Inner inner;
    int values[3];
    const char* label;
};

// volatile int rather than std::atomic: lldb assigns it through evaluate ("g_release = 1").
volatile int g_release = 0;
volatile int g_counter = 0;

FROST_NOINLINE int accumulate(int total, int i) {
    const int next = total + i * 3; // MARK: accumulate
    return next;
}

FROST_NOINLINE int frost_function_target(int value) {
    const int doubled = value * 2; // MARK: function-body
    return doubled;
}

FROST_NOINLINE void thrower(int code) {
    if (code > 0) {
        throw std::runtime_error("frost-scenario-throw"); // MARK: throw
    }
}

FROST_NOINLINE int recurse(int depth) {
    if (depth == 0) {
        return 0; // MARK: recurse-bottom
    }
    const int below = recurse(depth - 1); // MARK: recurse-call
    return below + 1;
}

FROST_NOINLINE void worker_body(int index) {
    // The second worker reaches the marker only after the first passed it, so a breakpoint there
    // stops each worker on its own (two threads reaching it at once would share one stop).
    while (index > 1 && g_counter == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g_counter += index; // MARK: worker
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

FROST_NOINLINE int spin_until_released() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (g_release == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // MARK: spin
    }
    return 0;
}

int run_values() {
    Outer outer{{7, 0.5}, {10, 20, 30}, "frost"};
    std::string text = "frost-scenario-string";
    std::vector<int> numbers{4, 8, 15, 16, 23, 42};
    std::printf("outer.inner.count=%d\n", outer.inner.count);
    std::printf("outer.values[2]=%d\n", outer.values[2]);
    std::printf("text=%s\n", text.c_str());
    std::printf("numbers.size=%zu\n", numbers.size());
    std::printf("numbers[5]=%d\n", numbers[5]);
    std::fflush(stdout); // MARK: values
    int total = 0;
    for (int i = 0; i < 10; ++i) {
        total = accumulate(total, i); // MARK: loop
    }
    total += frost_function_target(static_cast<int>(numbers.size()));
    std::printf("total=%d\n", total);
    std::fflush(stdout);
    return total % 256; // MARK: values-return
}

int run_threads() {
    std::thread first(worker_body, 1);
    std::thread second(worker_body, 2);
    first.join();
    second.join();
    std::printf("counter=%d\n", g_counter); // MARK: threads-joined
    std::fflush(stdout);
    return 0;
}

int run_setvar() {
    int changed = 5;
    std::fflush(stdout); // MARK: setvar
    std::printf("changed=%d\n", changed);
    std::fflush(stdout);
    return 0;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "values";
    if (std::strcmp(mode, "values") == 0) {
        return run_values();
    }
    if (std::strcmp(mode, "exit") == 0) {
        return argc > 2 ? std::atoi(argv[2]) : 0;
    }
    if (std::strcmp(mode, "output") == 0) {
        std::printf("frost-stdout-line\n");
        std::fflush(stdout);
        std::fprintf(stderr, "frost-stderr-line\n");
        std::fflush(stderr);
        return 0;
    }
    if (std::strcmp(mode, "throw") == 0) {
        try {
            thrower(1); // MARK: before-throw
        } catch (const std::runtime_error& error) {
            std::printf("caught=%s\n", error.what()); // MARK: catch
            std::fflush(stdout);
        }
        return 0;
    }
    if (std::strcmp(mode, "threads") == 0) {
        return run_threads();
    }
    if (std::strcmp(mode, "spin") == 0) {
        return spin_until_released();
    }
    if (std::strcmp(mode, "wait") == 0) {
#ifdef _WIN32
        std::printf("pid=%d\n", _getpid());
#else
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
        std::printf("pid=%d\n", static_cast<int>(getpid()));
#endif
        std::fflush(stdout);
        return spin_until_released();
    }
    if (std::strcmp(mode, "setvar") == 0) {
        return run_setvar();
    }
    if (std::strcmp(mode, "deep") == 0) {
        const int depth = recurse(5);
        std::printf("depth=%d\n", depth);
        std::fflush(stdout);
        return 0;
    }
    if (std::strcmp(mode, "args") == 0) {
        for (int i = 2; i < argc; ++i) {
            std::printf("arg%d=%s\n", i - 2, argv[i]);
        }
        std::fflush(stdout);
        return argc - 2;
    }
    if (std::strcmp(mode, "env") == 0) {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t length = 0;
        const bool found = _dupenv_s(&value, &length, "FROST_SCENARIO_ENV") == 0 && value != nullptr;
        std::printf("env=%s\n", found ? value : "(unset)");
        std::free(value);
#else
        const char* value = std::getenv("FROST_SCENARIO_ENV");
        std::printf("env=%s\n", value != nullptr ? value : "(unset)");
#endif
        std::fflush(stdout);
        return 0;
    }
    if (std::strcmp(mode, "cwd") == 0) {
        std::error_code error;
        const std::string cwd = std::filesystem::current_path(error).string();
        std::printf("cwd=%s\n", error ? "(unknown)" : cwd.c_str());
        std::fflush(stdout);
        return 0;
    }
    std::fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
