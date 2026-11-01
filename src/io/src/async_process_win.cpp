// SPDX-License-Identifier: Apache-2.0
//
/*
 * IAsyncProcess on Windows (ADR 0020): the parent's overlapped named-pipe ends (process_win.cpp creates
 * them with FILE_FLAG_OVERLAPPED) are associated with the reactor's completion port through
 * asio::windows::stream_handle, and the exit is watched through asio::windows::object_handle on a
 * duplicate of the process handle. These handles are never used by the blocking WindowsProcess, so no
 * completion packet ever meets one of its stack OVERLAPPEDs.
 */
/*
 * The process and job handles stay open until the state is destroyed, so terminate() may use them from
 * any thread while the public object lives. Release (the object destroyed) closes the pipes, ends the job
 * at once with killOnClose (TerminateJobObject; closing the job handle would end it too, with
 * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, but only once the last reference to the state drops), and stops
 * the exit wait. Windows needs no reaping.
 */

#include "frost/core/assert.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/reactor.hpp"

#include "async_process_impl.hpp" // asio first.
#include "process_internal.hpp"
#include <asio/windows/object_handle.hpp>

#include <format>
#include <memory>
#include <optional>
#include <utility>

#include <windows.h>

namespace frost::io::detail {
namespace {

class WindowsAsyncProcess final : public AsyncProcessState {
public:
    WindowsAsyncProcess(Reactor::Impl& reactor, sched::IScheduler& scheduler, SpawnedChild& child)
        : AsyncProcessState(reactor, scheduler, child.pid, child.in.valid(), child.out.valid(), child.err.valid()),
          m_process(std::move(child.process)), m_job(std::move(child.job)) {}

    Result<void> terminateChild() override {
        // The job form also ends descendants that outlived the child itself (as WindowsProcess::terminate).
        if (m_job.valid()) {
            if (TerminateJobObject(m_job.get(), static_cast<UINT>(kTerminatedExitCode)) == FALSE) {
                return std::unexpected(lastWin32Error("TerminateJobObject"));
            }
            return {};
        }
        if (WaitForSingleObject(m_process.get(), 0) == WAIT_OBJECT_0) {
            return {};
        }
        if (TerminateProcess(m_process.get(), static_cast<UINT>(kTerminatedExitCode)) == FALSE) {
            const DWORD error = GetLastError();
            // ERROR_ACCESS_DENIED also means "already exiting"; only report it if the process lives on.
            if (WaitForSingleObject(m_process.get(), 0) == WAIT_OBJECT_0) {
                return {};
            }
            return std::unexpected(win32Error(error, "TerminateProcess"));
        }
        return {};
    }

protected:
    void startExitWatch() override {
        HANDLE duplicate = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), m_process.get(), GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE,
                            0) == FALSE) {
            exitObserved(std::unexpected(lastWin32Error("DuplicateHandle(process)")));
            return;
        }
        m_exitWait.emplace(reactor().context);
        std::error_code ec;
        static_cast<void>(m_exitWait->assign(duplicate, ec));
        if (ec) {
            CloseHandle(duplicate);
            exitObserved(std::unexpected(win32Error(static_cast<std::uint32_t>(ec.value()), "watch the process exit")));
            return;
        }
        m_exitWait->async_wait(bindHandler([this](const std::error_code& waited) {
            if (waited == asio::error::operation_aborted) {
                return; // Release or shutdown.
            }
            if (waited) {
                exitObserved(
                    std::unexpected(win32Error(static_cast<std::uint32_t>(waited.value()), "wait for the process")));
                return;
            }
            DWORD code = 0;
            if (GetExitCodeProcess(m_process.get(), &code) == FALSE) {
                exitObserved(std::unexpected(lastWin32Error("GetExitCodeProcess")));
                return;
            }
            exitObserved(ExitStatus{static_cast<int>(code), 0});
        }));
    }

    void stopExitWatch() noexcept override {
        if (m_exitWait && m_exitWait->is_open()) {
            std::error_code ignored;
            static_cast<void>(m_exitWait->close(ignored));
        }
    }

    void releaseChild() noexcept override {
        if (m_job.valid()) {
            static_cast<void>(TerminateJobObject(m_job.get(), static_cast<UINT>(kTerminatedExitCode)));
        }
        stopExitWatch();
    }

private:
    // Open until destruction (see the file comment); the job ends the tree when it closes.
    UniqueHandle m_process;
    UniqueHandle m_job;
    std::optional<asio::windows::object_handle> m_exitWait; // Strand only.
};

// Associates the parent's pipe end with the completion port; an invalid handle gives no stream.
Result<std::optional<PipeStream>> associate(Reactor::Impl& reactor, UniqueHandle& handle, std::string_view what) {
    if (!handle.valid()) {
        return std::optional<PipeStream>{};
    }
    PipeStream stream(reactor.context);
    std::error_code ec;
    static_cast<void>(stream.assign(handle.get(), ec));
    if (ec) {
        return std::unexpected(
            win32Error(static_cast<std::uint32_t>(ec.value()), std::format("associate {} with the reactor", what)));
    }
    static_cast<void>(handle.release()); // The stream owns it now.
    return std::optional<PipeStream>(std::move(stream));
}

} // namespace

Result<std::unique_ptr<IAsyncProcess>> makeAsyncProcess(SpawnedChild child, const AsyncContext& context) {
    Reactor::Impl& reactor = context.reactor.impl();
    auto state = std::make_shared<WindowsAsyncProcess>(reactor, context.scheduler, child);
    auto in = associate(reactor, child.in, "stdin");
    auto out =
        in ? associate(reactor, child.out, "stdout") : Result<std::optional<PipeStream>>(std::unexpected(in.error()));
    auto err =
        out ? associate(reactor, child.err, "stderr") : Result<std::optional<PipeStream>>(std::unexpected(out.error()));
    if (!err) {
        static_cast<void>(state->terminateChild()); // Never handed out: end it.
        return std::unexpected(std::move(err.error()));
    }
    // Adopted before the state is bound: until then no other thread (shutdown included) can see it.
    state->adoptPipes(std::move(*in), std::move(*out), std::move(*err));
    if (auto bound = reactor.bind(state); !bound) {
        static_cast<void>(state->terminateChild());
        return std::unexpected(std::move(bound.error()));
    }
    if (!state->post([state] { state->startOnStrand(); })) {
        static_cast<void>(state->terminateChild());
        return fail(ErrorCode::Cancelled, "launchAsync: the reactor is shut down");
    }
    return std::unique_ptr<IAsyncProcess>(std::make_unique<AsyncProcess>(std::move(state)));
}

} // namespace frost::io::detail
