// SPDX-License-Identifier: Apache-2.0
//
// Helpers shared by frost::diag's integration tests: running crash_child (and other helper
// programs) through frost::io's real launcher, on Windows on a private desktop with hard-error
// boxes off, with the sanitizers' own fault handling switched off in sanitizer trees (so the
// fault reaches crashpad); and listing what a crashpad database holds.
// Header-only; test code only.
#pragma once

#include "frost/io/process.hpp"

#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "support/private_desktop.hpp"
#else
#include <csignal>

#include <signal.h>
#endif

namespace frost::test {

struct ChildRun {
    int exitCode = -1;  // -1: not started or not finished (see `error`).
    int signal = 0;     // POSIX: the signal that ended the child.
    std::string output; // stdout and stderr, merged.
    std::string error;
};

using ChildEnvironment = std::vector<std::pair<std::string, std::optional<std::string>>>;

// The environment overrides every crashing child gets: in sanitizer trees the sanitizer must not
// handle the fault itself (handle_segv=0), so crashpad sees it, as in a production build.
inline ChildEnvironment crashEnvironment(ChildEnvironment extra = {}) {
    if constexpr (kSanitized) {
        for (const char* name : {"ASAN_OPTIONS", "TSAN_OPTIONS"}) {
            std::string value = environment(name);
            value += value.empty() ? "handle_segv=0" : ":handle_segv=0";
            extra.emplace_back(name, value);
        }
    }
    return extra;
}

/*
 * Runs `executable` with `arguments` (Windows: plus "--desktop <private desktop>" when
 * `passDesktop`, so a crashpad handler the child starts lands on the same private desktop) and
 * waits at most `timeout`. `stdinBytes` is written to the child's stdin, which is then closed.
 */
inline ChildRun runChild(const std::string& executable, std::vector<std::string> arguments,
                         ChildEnvironment environment = {}, const std::string& stdinBytes = {},
                         std::chrono::milliseconds timeout = std::chrono::seconds(60), bool passDesktop = true) {
    io::ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = std::move(arguments);
    spec.environment = std::move(environment);
    spec.stdinMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::MergeIntoStdout;
#ifdef _WIN32
    const NoHardErrorBoxes noBoxes;
    const PrivateDesktop desktop;
    if (!desktop.valid()) {
        return {-1, 0, {}, "no private desktop"};
    }
    spec.desktop = desktop.name();
    if (passDesktop) {
        spec.arguments.emplace_back("--desktop");
        spec.arguments.push_back(desktop.name());
    }
#else
    static_cast<void>(passDesktop);
#endif
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, stdinBytes, timeout);
    if (!result) {
        return {-1, 0, {}, result.error().describe()};
    }
    return {result->exitCode, result->signal, std::move(result->stdoutBytes), {}};
}

// The value after "<key>=" on the first output line containing it, or "".
inline std::string valueAfter(std::string_view output, std::string_view key) {
    const std::string needle = std::string(key) + "=";
    const std::size_t at = output.find(needle);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t begin = at + needle.size();
    const std::size_t end = output.find_first_of("\r\n", begin);
    return std::string(output.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
}

// The minidumps below a crashpad database: finished ones (Windows reports/, Linux pending/ and
// completed/) and unfinished ones (new/).
struct DatabaseReports {
    std::vector<std::filesystem::path> finished;
    std::vector<std::filesystem::path> unfinished;
};

inline DatabaseReports listReports(const std::filesystem::path& database) {
    DatabaseReports reports;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(database, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (it->path().extension() != ".dmp") {
            continue;
        }
        (it->path().parent_path().filename() == "new" ? reports.unfinished : reports.finished).push_back(it->path());
    }
    return reports;
}

// listReports() once the database holds `finished` finished reports and nothing unfinished,
// or after `limit` (the handler may still be moving the report out of new/).
inline DatabaseReports waitForReports(const std::filesystem::path& database, std::size_t finished,
                                      std::chrono::milliseconds limit = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        DatabaseReports reports = listReports(database);
        if ((reports.finished.size() >= finished && reports.unfinished.empty()) ||
            std::chrono::steady_clock::now() >= deadline) {
            return reports;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

inline std::string readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Whether process `pid` still exists.
inline bool processAlive(std::uint64_t pid) {
#ifdef _WIN32
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return false;
    }
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

// Waits up to `limit` for process `pid` to end; true when it did.
inline bool waitForProcessGone(std::uint64_t pid, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (processAlive(pid)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

} // namespace frost::test
