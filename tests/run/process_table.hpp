// SPDX-License-Identifier: Apache-2.0
//
// The system's process table for the RunManager Stop tests (run_manager_tree_test.cpp): every live
// process with its parent and image name, read from the OS (a Toolhelp32 snapshot on Windows,
// /proc on Linux) independently of frost::io, so it is an oracle for "Stop leaves no descendant".
#pragma once

#include "frost/io/process.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <tlhelp32.h>
#else
#include <filesystem>
#include <fstream>
#include <sstream>
#endif

namespace frost::test {

struct OsProcess {
    io::ProcessId pid = 0;
    io::ProcessId parent = 0;
    std::string image{}; // The executable's file name, lower-case ASCII ("run_tree_child.exe").
};

[[nodiscard]] inline std::string lowerAscii(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/*
 * Every process that is running now (a snapshot). Linux skips zombies (state Z): they have ended
 * and only wait to be reaped. Windows lists processes until their last handle closes, as the OS
 * does; the tests close theirs first. Empty when the table cannot be read.
 */
[[nodiscard]] inline std::vector<OsProcess> processTable() {
    std::vector<OsProcess> table;
#ifdef _WIN32
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return table;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE; more = Process32NextW(snapshot, &entry)) {
        std::string image;
        for (const wchar_t* c = entry.szExeFile; *c != L'\0'; ++c) {
            image += *c < 128 ? static_cast<char>(*c) : '?';
        }
        table.push_back({entry.th32ProcessID, entry.th32ParentProcessID, lowerAscii(image)});
    }
    CloseHandle(snapshot);
#else
    std::error_code error;
    for (const auto& item : std::filesystem::directory_iterator("/proc", error)) {
        const std::string name = item.path().filename().string();
        if (name.empty() || !std::ranges::all_of(name, [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }
        std::ifstream file(item.path() / "stat");
        std::string stat((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        // "<pid> (<comm>) <state> <ppid> ...": comm may hold spaces and parentheses, so cut at the last ')'.
        const auto open = stat.find('(');
        const auto close = stat.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open) {
            continue; // The process ended while the table was read.
        }
        std::istringstream rest(stat.substr(close + 1));
        char state = 0;
        unsigned long parent = 0;
        if (!(rest >> state >> parent) || state == 'Z' || state == 'X') {
            continue;
        }
        table.push_back({static_cast<io::ProcessId>(std::stoul(name)), static_cast<io::ProcessId>(parent),
                         lowerAscii(stat.substr(open + 1, close - open - 1))});
    }
#endif
    return table;
}

// The processes of `table` below `root` (children, their children, ...), by parent links. O(n^2).
[[nodiscard]] inline std::set<io::ProcessId> descendantsOf(io::ProcessId root, const std::vector<OsProcess>& table) {
    std::set<io::ProcessId> found;
    std::vector<io::ProcessId> frontier{root};
    while (!frontier.empty()) {
        const io::ProcessId parent = frontier.back();
        frontier.pop_back();
        for (const OsProcess& process : table) {
            // pid != parent: the idle process on Windows is its own parent.
            if (process.parent == parent && process.pid != parent && process.pid != root &&
                found.insert(process.pid).second) {
                frontier.push_back(process.pid);
            }
        }
    }
    return found;
}

} // namespace frost::test
