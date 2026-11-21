// SPDX-License-Identifier: Apache-2.0
//
// Native smoke for the Windows root and owned-job memory counters. The worker
// commits and touches memory in a real descendant, and the parent samples only
// after the worker signals that the allocation is live.

#include "child_process.hpp"

#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace {

constexpr auto waitMs = 10'000;
constexpr std::uint64_t allocationBytes = 48 * 1024 * 1024;

struct CloseHandleOnExit {
    void operator()(void* handle) const { CloseHandle(handle); }
};

using Handle = std::unique_ptr<void, CloseHandleOnExit>;

int allocate(const wchar_t* readyName, const wchar_t* releaseName) {
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName));
    Handle release(OpenEventW(SYNCHRONIZE, FALSE, releaseName));
    auto* memory =
        static_cast<volatile char*>(VirtualAlloc(nullptr, allocationBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!ready || !release || memory == nullptr) {
        return 1;
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    for (std::uint64_t offset = 0; offset < allocationBytes; offset += info.dwPageSize) {
        memory[offset] = 1;
    }
    if (SetEvent(ready.get()) == 0) {
        return 1;
    }
    return WaitForSingleObject(release.get(), waitMs) == WAIT_OBJECT_0 ? 0 : 1;
}

int launchDescendant(const wchar_t* executable, const wchar_t* readyName, const wchar_t* releaseName) {
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --allocate " + readyName + L" " + releaseName;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr,
                       &startup, &child) == 0) {
        return 1;
    }
    Handle process(child.hProcess);
    CloseHandle(child.hThread);
    DWORD exitCode = 1;
    return WaitForSingleObject(process.get(), waitMs) == WAIT_OBJECT_0 &&
                   GetExitCodeProcess(process.get(), &exitCode) != 0 && exitCode == 0
               ? 0
               : 1;
}

int smoke(const wchar_t* executable) {
    const auto name =
        L"Local\\FrostPerfMemory-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto readyName = name + L"-ready";
    const auto releaseName = name + L"-release";
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
    Handle release(CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str()));
    if (!ready || !release) {
        return 1;
    }
    auto child = frost::s1::ChildProcess::launch({.executable = executable,
                                                  .arguments = L"--launch-descendant " + readyName + L" " + releaseName,
                                                  .windowPosition = std::nullopt,
                                                  .noConsole = true});
    if (!child || WaitForSingleObject(ready.get(), waitMs) != WAIT_OBJECT_0) {
        std::cerr << "owned memory worker did not signal a live allocation\n";
        return 1;
    }
    const auto root = child->memory();
    const auto tree = child->memoryTree();
    if (!root || !tree) {
        std::cerr << (root ? tree.error().describe() : root.error().describe()) << '\n';
        return 1;
    }
    std::cout << "root private=" << root->privateBytes << " working_set=" << root->workingSetBytes
              << "\ntree members=" << tree->memberPids.size() << " private=" << tree->total.privateBytes
              << " summed_working_set=" << tree->total.workingSetBytes
              << " kernel_job_peak_commit=" << tree->peakJobCommitBytes << '\n';
    if (root->privateBytes == 0 || root->workingSetBytes == 0 || tree->memberPids.size() != 2 ||
        tree->total.privateBytes < root->privateBytes + allocationBytes ||
        tree->total.workingSetBytes < root->workingSetBytes + allocationBytes / 2 ||
        tree->peakJobCommitBytes < allocationBytes || SetEvent(release.get()) == 0 || child->wait(waitMs) != 0 ||
        child->memory() || child->memoryTree()) {
        std::cerr << "owned-job counters or teardown did not match the smoke fixture\n";
        return 1;
    }
    return 0;
}

int sampleWorkbench(const wchar_t* executable) {
    auto child =
        frost::s1::ChildProcess::launch({.executable = executable, .arguments = L"", .windowPosition = std::nullopt});
    if (!child) {
        std::cerr << child.error().describe() << '\n';
        return 1;
    }
    std::cout << "root " << child->pid() << std::endl;
    // The driving test proves phase via the authenticated Workbench endpoint
    // before requesting a sample. Keeping the launcher alive owns the whole job.
    for (std::string request; std::getline(std::cin, request);) {
        if (request == "finish") {
            return child->wait(waitMs) == 0 ? 0 : 1;
        }
        if (request != "sample") {
            return 1;
        }
        const auto root = child->memory();
        const auto tree = child->memoryTree();
        if (!root || !tree) {
            std::cerr << (root ? tree.error().describe() : root.error().describe()) << '\n';
            return 1;
        }
        std::cout << "sample " << root->privateBytes << ' ' << root->workingSetBytes << ' ' << tree->total.privateBytes
                  << ' ' << tree->total.workingSetBytes << ' ' << tree->peakJobCommitBytes;
        for (const auto pid : tree->memberPids) {
            std::cout << ' ' << pid;
        }
        std::cout << std::endl;
    }
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--allocate") {
        return allocate(argv[2], argv[3]);
    }
    wchar_t executable[32768];
    const auto length = GetModuleFileNameW(nullptr, executable, std::size(executable));
    if (length == 0 || length == std::size(executable)) {
        return 1;
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--launch-descendant") {
        return launchDescendant(executable, argv[2], argv[3]);
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--sample-workbench") {
        return sampleWorkbench(argv[2]);
    }
    return argc == 1 ? smoke(executable) : 1;
}
