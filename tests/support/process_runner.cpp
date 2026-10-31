// SPDX-License-Identifier: Apache-2.0
//
// Implementation of frost::test::runProcess (see process_runner.hpp) for
// Windows (CreateProcessW with an inherited pipe) and POSIX (fork/exec).

#include "process_runner.hpp"

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>

#include <unistd.h>
#endif

namespace frost::test {

#ifdef _WIN32

namespace {

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
    return wide;
}

// Quotes one argument following the rules CommandLineToArgvW uses to parse it:
// backslashes are literal except directly before a quote.
void appendQuoted(std::wstring& commandLine, const std::wstring& arg) {
    commandLine += L'"';
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            commandLine.append(backslashes * 2 + 1, L'\\');
        } else {
            commandLine.append(backslashes, L'\\');
        }
        backslashes = 0;
        commandLine += c;
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine += L'"';
}

} // namespace

ProcessResult runProcess(const std::filesystem::path& program, const std::vector<std::string>& args) {
    ProcessResult result;
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (CreatePipe(&readEnd, &writeEnd, &inherit, 0) == FALSE) {
        result.startError = "CreatePipe failed";
        return result;
    }
    // Only the write end is inherited by the child.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine;
    appendQuoted(commandLine, program.wstring());
    for (const auto& arg : args) {
        commandLine += L' ';
        appendQuoted(commandLine, toWide(arg));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &info);
    CloseHandle(writeEnd);
    if (started == FALSE) {
        result.startError = "CreateProcessW failed with error " + std::to_string(GetLastError());
        CloseHandle(readEnd);
        return result;
    }

    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) != FALSE && read > 0) {
        result.output.append(buffer, read);
    }
    CloseHandle(readEnd);
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return result;
}

#else

ProcessResult runProcess(const std::filesystem::path& program, const std::vector<std::string>& args) {
    ProcessResult result;
    int fds[2];
    if (pipe(fds) != 0) {
        result.startError = "pipe failed";
        return result;
    }
    // argv must be built before fork: only async-signal-safe calls are allowed in the child.
    std::string programPath = program.string();
    std::vector<char*> argv;
    argv.push_back(programPath.data());
    std::vector<std::string> copies(args);
    for (auto& arg : copies) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        result.startError = "fork failed";
        close(fds[0]);
        close(fds[1]);
        return result;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execv(programPath.c_str(), argv.data());
        _exit(127);
    }
    close(fds[1]);
    char buffer[4096];
    ssize_t count = 0;
    while ((count = read(fds[0], buffer, sizeof(buffer))) > 0) {
        result.output.append(buffer, static_cast<std::size_t>(count));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result.exitCode == 127 && result.output.empty()) {
        result.startError = "exec failed";
    }
    return result;
}

#endif

} // namespace frost::test
