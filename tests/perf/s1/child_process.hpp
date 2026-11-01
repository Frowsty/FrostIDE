// SPDX-License-Identifier: Apache-2.0
//
// A minimal child-process launcher for the S1 first-frame runs (Windows only).
// It is the only place the spike starts processes: CreateProcessW with an
// absolute executable path and the wide API, as CLAUDE.md requires. The child
// runs in its executable's directory and in a kill-on-close job, so a crashed
// harness never leaves a topmost frost.exe window behind.
/* TODO(M1): port to frost::io::IProcessLauncher (frost/io/process.hpp) once m0/io is
   merged. ProcessSpec covers executable, arguments, workingDirectory and
   killOnClose; before the port it needs the first window's placement
   (STARTF_USEPOSITION: x, y) and show command (STARTF_USESHOWWINDOW:
   SW_SHOWNOACTIVATE), and a process-start timestamp taken just before creation. */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace frost::s1 {

struct LaunchOptions {
    std::wstring executable; // Absolute path. Precondition: not relative.
    // Appended verbatim after the quoted executable path: the caller quotes any
    // argument that contains spaces (CommandLineToArgvW rules).
    std::wstring arguments;
    // The first overlapped window's position (STARTF_USEPOSITION), which Windows
    // applies when the child creates it with CW_USEDEFAULT.
    std::optional<std::pair<int, int>> windowPosition;
    // Show the first window without activating it (STARTF_USESHOWWINDOW with
    // SW_SHOWNOACTIVATE), so a run never takes the user's focus.
    bool noActivate = true;
};

// A running or finished child. Move-only; the destructor closes the handles, and
// closing the job handle terminates the child if it is still running. Use from
// one thread.
class ChildProcess {
public:
    // Errors: InvalidArgument (relative path), IoError (CreateProcessW or the job failed).
    [[nodiscard]] static Result<ChildProcess> launch(const LaunchOptions& options);

    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete ("owns process handles");
    ChildProcess& operator=(const ChildProcess&) = delete ("owns process handles");

    [[nodiscard]] std::uint32_t pid() const noexcept { return m_pid; }
    // QueryPerformanceCounter immediately before CreateProcessW: B1's "process start".
    [[nodiscard]] std::int64_t startTicks() const noexcept { return m_startTicks; }

    // Waits up to `timeoutMs` for the process to exit; returns its exit code, or
    // nullopt on timeout or when the exit code cannot be read.
    [[nodiscard]] std::optional<std::uint32_t> wait(std::uint32_t timeoutMs) const;
    // Terminates the process (only this child). Idempotent.
    void terminate() const;

private:
    ChildProcess() = default;
    void close() noexcept;

    void* m_process = nullptr; // HANDLE.
    void* m_job = nullptr;     // HANDLE of the kill-on-close job.
    std::uint32_t m_pid = 0;
    std::int64_t m_startTicks = 0;
};

} // namespace frost::s1
