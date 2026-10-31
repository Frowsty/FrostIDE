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
    ChildProcess child;
    child.m_startTicks = perf::win::qpcNow();
    if (CreateProcessW(options.executable.c_str(), mutableLine.data(), nullptr, nullptr, FALSE,
                       CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &info) == 0) {
        return fail(ErrorCode::IoError, "CreateProcessW failed with error " + std::to_string(GetLastError()));
    }
    CloseHandle(info.hThread);
    child.m_process = info.hProcess;
    child.m_pid = info.dwProcessId;
    return child;
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : m_process(std::exchange(other.m_process, nullptr)), m_pid(other.m_pid), m_startTicks(other.m_startTicks) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        if (m_process != nullptr) {
            CloseHandle(m_process);
        }
        m_process = std::exchange(other.m_process, nullptr);
        m_pid = other.m_pid;
        m_startTicks = other.m_startTicks;
    }
    return *this;
}

ChildProcess::~ChildProcess() {
    if (m_process != nullptr) {
        CloseHandle(m_process);
    }
}

std::optional<std::uint32_t> ChildProcess::wait(std::uint32_t timeoutMs) const {
    if (m_process == nullptr || WaitForSingleObject(m_process, timeoutMs) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(m_process, &code);
    return static_cast<std::uint32_t>(code);
}

void ChildProcess::terminate() const {
    if (m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(m_process, 1);
        WaitForSingleObject(m_process, 2000);
    }
}

} // namespace frost::s1
