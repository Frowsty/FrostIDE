// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the S1 child-process launcher (see child_process.hpp).

#include "child_process.hpp"

#include "frost/perf/win/timing.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <psapi.h>

namespace frost::s1 {

namespace {

bool isAbsolute(const std::wstring& path) {
    // Drive-absolute (C:\...) or UNC (\\server\...).
    return (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) || path.starts_with(L"\\\\");
}

std::wstring parentDirectory(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

} // namespace

Result<ChildProcess> ChildProcess::launch(const LaunchOptions& options) {
    if (!isAbsolute(options.executable)) {
        return fail(ErrorCode::InvalidArgument, "the executable path must be absolute");
    }
    std::wstring commandLine = L"\"" + options.executable + L"\"";
    if (!options.arguments.empty()) {
        commandLine += L" " + options.arguments;
    }
    std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
    mutableLine.push_back(L'\0');
    const std::wstring directory = parentDirectory(options.executable);

    ChildProcess child;
    child.m_job = CreateJobObjectW(nullptr, nullptr);
    if (child.m_job == nullptr) {
        return fail(ErrorCode::IoError, "CreateJobObjectW failed with error " + std::to_string(GetLastError()));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(child.m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0) {
        return fail(ErrorCode::IoError, "SetInformationJobObject failed with error " + std::to_string(GetLastError()));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (options.noActivate) {
        startup.dwFlags |= STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNOACTIVATE;
    }
    if (options.windowPosition) {
        startup.dwFlags |= STARTF_USEPOSITION;
        startup.dwX = static_cast<DWORD>(options.windowPosition->first);
        startup.dwY = static_cast<DWORD>(options.windowPosition->second);
    }
    PROCESS_INFORMATION info{};
    // Suspended until it is in the job, so it cannot escape it; resuming costs microseconds.
    child.m_startTicks = perf::win::qpcNow();
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | (options.noConsole ? DETACHED_PROCESS : 0U);
    if (CreateProcessW(options.executable.c_str(), mutableLine.data(), nullptr, nullptr, FALSE, flags, nullptr,
                       directory.empty() ? nullptr : directory.c_str(), &startup, &info) == 0) {
        return fail(ErrorCode::IoError, "CreateProcessW failed with error " + std::to_string(GetLastError()));
    }
    child.m_process = info.hProcess;
    child.m_pid = info.dwProcessId;
    if (AssignProcessToJobObject(child.m_job, info.hProcess) == 0) {
        const DWORD error = GetLastError();
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hThread);
        return fail(ErrorCode::IoError, "AssignProcessToJobObject failed with error " + std::to_string(error));
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    return child;
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : m_process(std::exchange(other.m_process, nullptr)), m_job(std::exchange(other.m_job, nullptr)),
      m_pid(other.m_pid), m_startTicks(other.m_startTicks) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        close();
        m_process = std::exchange(other.m_process, nullptr);
        m_job = std::exchange(other.m_job, nullptr);
        m_pid = other.m_pid;
        m_startTicks = other.m_startTicks;
    }
    return *this;
}

ChildProcess::~ChildProcess() { close(); }

void ChildProcess::close() noexcept {
    if (m_process != nullptr) {
        CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_job != nullptr) {
        CloseHandle(m_job); // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ends a child still running.
        m_job = nullptr;
    }
}

std::optional<std::uint32_t> ChildProcess::wait(std::uint32_t timeoutMs) const {
    if (m_process == nullptr || WaitForSingleObject(m_process, timeoutMs) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(m_process, &code) == 0) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(code);
}

namespace {

Result<ProcessMemory> sampleMemory(HANDLE process) {
    if (process == nullptr || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
        return fail(ErrorCode::Busy, "owned process is no longer running");
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) ==
        0) {
        return fail(ErrorCode::IoError, "K32GetProcessMemoryInfo failed with error " + std::to_string(GetLastError()));
    }
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
        return fail(ErrorCode::Busy, "owned process exited during sampling");
    }
    return ProcessMemory{counters.PrivateUsage, counters.WorkingSetSize};
}

Result<std::vector<ULONG_PTR>> jobMembers(HANDLE job) {
    // Windows returns a variable-length structure. Copy out its trailing IDs from aligned storage.
    constexpr auto idsOffset = offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList);
    constexpr std::size_t maxMembers = 1024;
    alignas(ULONG_PTR) std::array<std::byte, idsOffset + maxMembers * sizeof(ULONG_PTR)> storage{};
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, storage.data(), static_cast<DWORD>(storage.size()),
                                  nullptr) == 0) {
        return fail(ErrorCode::IoError, "job member enumeration failed with error " + std::to_string(GetLastError()));
    }
    JOBOBJECT_BASIC_PROCESS_ID_LIST header{};
    std::memcpy(&header, storage.data(), idsOffset);
    if (header.NumberOfProcessIdsInList > maxMembers ||
        header.NumberOfProcessIdsInList != header.NumberOfAssignedProcesses) {
        return fail(ErrorCode::Busy, "owned job membership changed during enumeration");
    }
    std::vector<ULONG_PTR> members(header.NumberOfProcessIdsInList);
    if (!members.empty()) {
        std::memcpy(members.data(), storage.data() + idsOffset, members.size() * sizeof(ULONG_PTR));
    }
    std::ranges::sort(members);
    return members;
}

struct CloseProcessHandle {
    void operator()(void* process) const { CloseHandle(process); }
};

} // namespace

Result<ProcessMemory> ChildProcess::memory() const { return sampleMemory(m_process); }

Result<ProcessTreeMemory> ChildProcess::memoryTree() const {
    if (m_job == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no owned job to sample");
    }
    const auto members = jobMembers(m_job);
    if (!members) {
        return std::unexpected(members.error());
    }
    if (!std::ranges::binary_search(*members, m_pid)) {
        return fail(ErrorCode::Busy, "owned root missing from its job");
    }
    std::vector<std::unique_ptr<void, CloseProcessHandle>> children;
    ProcessTreeMemory memory{{0, 0}, {}, 0};
    for (const auto pid : *members) {
        HANDLE process = m_process;
        if (pid != m_pid) {
            children.emplace_back(
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
            process = children.back().get();
            BOOL sameJob = FALSE;
            if (process == nullptr || IsProcessInJob(process, m_job, &sameJob) == 0 || sameJob == FALSE) {
                return fail(ErrorCode::Busy, "could not verify a process in the owned job");
            }
        }
        const auto sample = sampleMemory(process);
        if (!sample) {
            return std::unexpected(sample.error());
        }
        memory.memberPids.push_back(static_cast<std::uint32_t>(pid));
        memory.total.privateBytes += sample->privateBytes;
        memory.total.workingSetBytes += sample->workingSetBytes;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (QueryInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr) == 0) {
        return fail(ErrorCode::IoError, "job commit peak query failed with error " + std::to_string(GetLastError()));
    }
    const auto after = jobMembers(m_job);
    if (!after) {
        return std::unexpected(after.error());
    }
    if (*after != *members || WaitForSingleObject(m_process, 0) != WAIT_TIMEOUT ||
        std::ranges::any_of(children,
                            [](const auto& child) { return WaitForSingleObject(child.get(), 0) != WAIT_TIMEOUT; })) {
        return fail(ErrorCode::Busy, "owned job membership changed during sampling");
    }
    memory.peakJobCommitBytes = limits.PeakJobMemoryUsed;
    return memory;
}

void ChildProcess::terminate() const {
    if (m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(m_process, 1);
        WaitForSingleObject(m_process, 2000);
    }
}

} // namespace frost::s1
