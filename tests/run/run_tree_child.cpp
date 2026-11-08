// SPDX-License-Identifier: Apache-2.0
//
// run_tree_child: the helper program the RunManager Stop tests start (run_manager_tree_test.cpp). It
// builds a real process tree, so the tests can check by process enumeration that Stop ends every
// descendant, not just the program.
//
//   tree DEPTH WIDTH   prints "pid <own pid>" on stdout (one write, flushed), then starts WIDTH
//                      copies of itself as "tree DEPTH-1 WIDTH" when DEPTH > 0, all sharing its
//                      standard handles, and sleeps 60 s. DEPTH 2 WIDTH 2 is a tree of 1 + 2 + 4
//                      processes. On POSIX the first copy a DEPTH 2 process starts leads a process
//                      group of its own (setpgid, as ninja starts every command), so only a
//                      session-wide kill reaches it and its children.
//
// It uses only the OS and the C runtime, not frost::io (as io_test_child does), so the tree it
// builds does not depend on the code under test. A usage error exits with 64.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>

#include <spawn.h>
#include <unistd.h>

// POSIX requires the application to declare environ (see io_test_child.cpp).
extern char** environ; // NOLINT(readability-redundant-declaration): POSIX requires the declaration
#endif

namespace {

void line(const std::string& text) {
    const std::string bytes = text + "\n";
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

unsigned long ownPid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Starts "<this program> tree <depth> <width>" with this process's standard handles; false on failure.
bool spawnCopy(int depth, int width, [[maybe_unused]] bool ownGroup) {
#ifdef _WIN32
    std::wstring self(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (length == 0 || length >= self.size()) {
        return false;
    }
    self.resize(length);
    std::wstring commandLine = L"\"" + self + L"\" tree " + std::to_wstring(depth) + L" " + std::to_wstring(width);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    // bInheritHandles = TRUE: the standard handles this process inherited are still inheritable.
    if (CreateProcessW(self.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                       &info) == FALSE) {
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
#else
    char self[PATH_MAX + 1] = {};
    const ssize_t length = readlink("/proc/self/exe", self, PATH_MAX);
    if (length <= 0) {
        return false;
    }
    std::string mode = "tree";
    std::string depthText = std::to_string(depth);
    std::string widthText = std::to_string(width);
    char* argv[] = {self, mode.data(), depthText.data(), widthText.data(), nullptr};
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) {
        return false;
    }
    if (ownGroup && (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) != 0 ||
                     posix_spawnattr_setpgroup(&attributes, 0) != 0)) {
        posix_spawnattr_destroy(&attributes);
        return false;
    }
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, self, nullptr, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
    return spawned == 0;
#endif
}

// The whole decimal number in `text` in [0, 8]; -1 otherwise.
int smallNumber(const std::string& text) {
    if (text.size() != 1 || text[0] < '0' || text[0] > '8') {
        return -1;
    }
    return text[0] - '0';
}

int run(const std::vector<std::string>& args) {
    if (args.size() != 4 || args[1] != "tree") {
        std::fputs("usage: run_tree_child tree DEPTH WIDTH\n", stderr);
        return 64;
    }
    const int depth = smallNumber(args[2]);
    const int width = smallNumber(args[3]);
    if (depth < 0 || width < 0) {
        std::fputs("run_tree_child: DEPTH and WIDTH are single digits\n", stderr);
        return 64;
    }
    line("pid " + std::to_string(ownPid()));
    if (depth > 0) {
        for (int i = 0; i < width; ++i) {
            // A DEPTH 2 process's first copy leads its own process group (POSIX; see the header).
            if (!spawnCopy(depth - 1, width, depth == 2 && i == 0)) {
                line("spawn failed");
                return 3;
            }
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        // The arguments are ASCII (a mode and digits); anything else fails the usage check.
        std::string arg;
        for (const wchar_t* c = argv[i]; *c != L'\0'; ++c) {
            arg += *c < 128 ? static_cast<char>(*c) : '?';
        }
        args.push_back(std::move(arg));
    }
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
