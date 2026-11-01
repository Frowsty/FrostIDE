// SPDX-License-Identifier: Apache-2.0
//
/*
 * Helpers shared by the real-process tests (process_test.cpp, async_process_test.cpp and the pipe fault
 * suite async_process_fault_test.cpp): the io_test_child spec, ProcessWatch (observes a process's exit
 * without being its parent) and AsyncEnv (a Reactor and a ThreadPool torn down in the exit order of
 * async_process.hpp, with helpers that drive IAsyncProcess coroutines from the test thread).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"
#include "frost/sched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>

#include <csignal>

#include <poll.h>
#include <unistd.h>
#endif

namespace frost::test {

// io_test_child's absolute path, normalised (CMake hands it over with forward slashes).
[[nodiscard]] inline std::string childPath() { return io::normalizePath(FROST_IO_TEST_CHILD).value(); }

[[nodiscard]] inline io::ProcessSpec childSpec(std::vector<std::string> arguments) {
    io::ProcessSpec spec;
    spec.executable = childPath();
    spec.arguments = std::move(arguments);
    return spec;
}

// The bytes io_test_child's flood and stream modes write: byte i is i % 251.
[[nodiscard]] inline std::string pattern(std::size_t size) {
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<char>(i % 251);
    }
    return out;
}

// Observes a process's exit without being its parent (a SYNCHRONIZE handle or a pidfd).
// Opened while the process is known to be alive, so the id cannot have been reused.
class ProcessWatch {
public:
#ifdef _WIN32
    explicit ProcessWatch(io::ProcessId pid) : m_handle(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid)) {}
#else
    explicit ProcessWatch(io::ProcessId pid)
        : m_fd(static_cast<int>(syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0))) {}
#endif
    ~ProcessWatch() {
#ifdef _WIN32
        if (m_handle != nullptr) {
            CloseHandle(m_handle);
        }
#else
        if (m_fd >= 0) {
            close(m_fd);
        }
#endif
    }
    ProcessWatch(const ProcessWatch&) = delete ("owns a handle");
    ProcessWatch& operator=(const ProcessWatch&) = delete ("owns a handle");

    [[nodiscard]] bool valid() const noexcept {
#ifdef _WIN32
        return m_handle != nullptr;
#else
        return m_fd >= 0;
#endif
    }

    // Whether the process has ended, or ends within `timeout`.
    [[nodiscard]] bool exitsWithin(std::chrono::milliseconds timeout) const {
#ifdef _WIN32
        return WaitForSingleObject(m_handle, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
#else
        pollfd entry{m_fd, POLLIN, 0};
        return poll(&entry, 1, static_cast<int>(timeout.count())) == 1;
#endif
    }

    // Cleanup for failed expectations and for processes the test owns.
    void kill() const {
#ifdef _WIN32
        TerminateProcess(m_handle, 1);
#else
        syscall(SYS_pidfd_send_signal, m_fd, SIGKILL, nullptr, 0);
#endif
    }

private:
#ifdef _WIN32
    HANDLE m_handle = nullptr;
#else
    int m_fd = -1;
#endif
};

/*
 * A reactor and a two-worker pool for IAsyncProcess tests. Declare it before the processes: they are
 * destroyed first (their release is posted), then the reactor shuts down (draining every handler), then
 * the pool (draining the coroutines the drain woke), as async_process.hpp prescribes.
 */
class AsyncEnv {
public:
    explicit AsyncEnv(unsigned reactorThreads = 1) {
        auto reactor = io::Reactor::create(io::ReactorOptions{reactorThreads, "frost-test-proc"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<sched::ThreadPool>(sched::ThreadPoolOptions{.workerCount = 2});
    }
    ~AsyncEnv() {
        if (m_reactor) {
            m_reactor->shutdown();
        }
        if (m_pool) {
            m_pool->shutdown();
        }
        m_pool.reset();
        m_reactor.reset();
    }
    AsyncEnv(const AsyncEnv&) = delete ("owns the reactor");
    AsyncEnv& operator=(const AsyncEnv&) = delete ("owns the reactor");

    [[nodiscard]] io::Reactor& reactor() { return *m_reactor; }
    [[nodiscard]] sched::ThreadPool& pool() { return *m_pool; }
    [[nodiscard]] io::AsyncContext context() { return io::AsyncContext{*m_reactor, *m_pool}; }

    [[nodiscard]] Result<std::unique_ptr<io::IAsyncProcess>> launch(const io::ProcessSpec& spec) {
        return io::realProcessLauncher().launchAsync(spec, context());
    }

    // Runs `task` on a pool worker (as production callers do) and waits for it on the test thread.
    template<class T>
    T run(sched::Task<T> task) {
        return m_pool->spawn(sched::Lane::UserVisible, std::move(task)).join();
    }

    // Reads `stdout` (or stderr) to EOF; an error ends the read and is returned.
    [[nodiscard]] Result<std::string> drain(io::IAsyncProcess& process, bool errStream = false,
                                            std::size_t chunk = 64 * 1024) {
        return run(drainTask(process, errStream, chunk));
    }

    [[nodiscard]] std::int64_t boundObjects() const { return m_reactor->stats().boundObjects; }

    // Waits up to `timeout` for every bound state to be gone (release and reaping run asynchronously).
    [[nodiscard]] bool settlesWithin(std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (boundObjects() != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

private:
    static sched::Task<Result<std::string>> drainTask(io::IAsyncProcess& process, bool errStream, std::size_t chunk) {
        std::string collected;
        std::vector<char> buffer(chunk);
        for (;;) {
            auto got = co_await (errStream ? process.readStderr(buffer) : process.readStdout(buffer));
            if (!got) {
                co_return std::unexpected(std::move(got.error()));
            }
            if (*got == 0) {
                co_return collected;
            }
            if (*got > buffer.size()) {
                co_return fail(ErrorCode::Internal, "a read returned more than the buffer holds");
            }
            collected.append(buffer.data(), *got);
        }
    }

    std::unique_ptr<io::Reactor> m_reactor;
    std::unique_ptr<sched::ThreadPool> m_pool;
};

} // namespace frost::test
