// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the S1 child-process launcher (see child_process.hpp).

#include "child_process.hpp"

#include "frost/perf/win/timing.hpp"

#include <string>
#include <utility>
#include <vector>

#include <windows.h>

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
    if (CreateProcessW(options.executable.c_str(), mutableLine.data(), nullptr, nullptr, FALSE,
                       CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, nullptr,
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

void ChildProcess::terminate() const {
    if (m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(m_process, 1);
        WaitForSingleObject(m_process, 2000);
    }
}

} // namespace frost::s1
