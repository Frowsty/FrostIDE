// SPDX-License-Identifier: Apache-2.0
/*
 * The system's process table for the RunManager Stop tests (run_manager_tree_test.cpp): every live
 * process with its parent, image name and (Windows) creation time, read from the OS (a Toolhelp32
 * snapshot on Windows, /proc on Linux) independently of frost::io, so it is an oracle for "Stop
 * leaves no descendant".
 *
 * On Windows a parent link is trusted only when creation times prove it (verifiedLink). Windows
 * never clears a child's parent id when the parent exits, so after PID reuse an unrelated, older
 * process can name a process of the tree as its parent. Linux reparents orphans, so /proc's ppid
 * is always the live parent.
 */
#pragma once

#include "frost/io/process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
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
    // Windows: the creation time (FILETIME, 100 ns units); 0 when it could not be read, which leaves
    // every link to or from the process unverified. Linux: always 0 (not needed, see verifiedLink).
    std::uint64_t created = 0;
};

[[nodiscard]] inline std::string lowerAscii(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

#ifdef _WIN32
// The creation time of the process `handle` refers to (FILETIME, 100 ns units); 0 on failure.
[[nodiscard]] inline std::uint64_t creationTimeOf(HANDLE handle) {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(handle, &created, &exited, &kernel, &user) == FALSE) {
        return 0;
    }
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
}

/*
 * The creation time of the process that holds `pid` now; 0 when it cannot be opened. Only meaningful
 * while the caller holds a handle to the process it means (as a ProcessWatch does): a pid is not
 * reused while a handle to its process is open, otherwise the pid may already name another process.
 */
[[nodiscard]] inline std::uint64_t creationTime(io::ProcessId pid) {
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        return 0;
    }
    const std::uint64_t created = creationTimeOf(handle);
    CloseHandle(handle);
    return created;
}

// One Toolhelp32 snapshot: pid, parent and image of every process the OS lists (created = 0). Empty on failure.
[[nodiscard]] inline std::vector<OsProcess> toolhelpSnapshot() {
    std::vector<OsProcess> table;
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
    return table;
}
#endif

/*
 * Every process that is running now (a snapshot). Linux skips zombies (state Z): they have ended
 * and only wait to be reaped. Windows lists processes until their last handle closes, as the OS
 * does. Empty when the table cannot be read.
 *
 * Windows: handles are opened to the processes of a first snapshot and held over a second one, which
 * is the table; each listed process's creation time is read through its held handle. A pid is never
 * reused while a handle to its process is open, so every time belongs to exactly the process the
 * second snapshot lists under that pid. A process that started between the snapshots (no handle) or
 * cannot be opened (protected, another user's) keeps created = 0. O(n log n), n OpenProcess calls.
 */
[[nodiscard]] inline std::vector<OsProcess> processTable() {
#ifdef _WIN32
    std::map<io::ProcessId, HANDLE> held;
    for (const OsProcess& process : toolhelpSnapshot()) {
        if (process.pid == 0) {
            continue; // The idle process cannot be opened.
        }
        if (const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid)) {
            held.emplace(process.pid, handle);
        }
    }
    std::vector<OsProcess> table = toolhelpSnapshot();
    for (OsProcess& process : table) {
        if (const auto at = held.find(process.pid); at != held.end()) {
            process.created = creationTimeOf(at->second);
        }
    }
    for (const auto& [pid, handle] : held) {
        CloseHandle(handle);
    }
    return table;
#else
    std::vector<OsProcess> table;
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
    return table;
#endif
}

/*
 * Whether `child`'s parent link to `parent` (the process the same table lists under child.parent)
 * is real. Windows: only if both creation times are known and the child started at or after the
 * parent and at or after `notBefore` (the root's start). This is sound: the real parent was alive
 * when it created the child, so a process that holds the parent's pid and is at least as old as the
 * child must be that parent; a reused pid belongs to a process started after the child. Linux: always
 * (the kernel reparents orphans, so ppid never names a stale process).
 */
[[nodiscard]] inline bool verifiedLink([[maybe_unused]] const OsProcess& child,
                                       [[maybe_unused]] const OsProcess& parent,
                                       [[maybe_unused]] std::uint64_t notBefore) {
#ifdef _WIN32
    return child.created != 0 && parent.created != 0 && child.created >= parent.created && child.created >= notBefore;
#else
    return true;
#endif
}

/*
 * The processes of `table` below `root` (children, their children, ...), following only parent
 * links verifiedLink accepts. Empty when `root` is not in the table (or, on Windows, its creation
 * time is unknown). O(n^2).
 */
[[nodiscard]] inline std::set<io::ProcessId> descendantsOf(io::ProcessId root, const std::vector<OsProcess>& table) {
    std::map<io::ProcessId, const OsProcess*> byPid;
    for (const OsProcess& process : table) {
        byPid.emplace(process.pid, &process);
    }
    const auto rootAt = byPid.find(root);
    if (rootAt == byPid.end()) {
        return {};
    }
    const std::uint64_t rootCreated = rootAt->second->created;
#ifdef _WIN32
    if (rootCreated == 0) {
        return {};
    }
#endif
    std::set<io::ProcessId> found;
    std::vector<const OsProcess*> frontier{rootAt->second};
    while (!frontier.empty()) {
        const OsProcess* parent = frontier.back();
        frontier.pop_back();
        for (const OsProcess& process : table) {
            // pid != parent: the idle process on Windows is its own parent.
            if (process.parent == parent->pid && process.pid != parent->pid && process.pid != root &&
                verifiedLink(process, *parent, rootCreated) && found.insert(process.pid).second) {
                frontier.push_back(&process);
            }
        }
    }
    return found;
}

} // namespace frost::test
