// SPDX-License-Identifier: Apache-2.0
//
// The corpus build lock for C++ tests (FND-6b): the same lock tools/corpora.py's BuildLock takes on
// <corpora root>/<id>-<sha12>.build.lock while it configures or builds the shared engine clone in
// place, so a corpus test that configures the clone never runs beside `corpora.py engine
// --configure` or a journey (tests/journeys) on the same pin. Windows locks the file's first byte
// (LockFileEx; msvcrt.locking takes the same byte), Linux flock()s the whole file. Test code only.
#pragma once

#include <chrono>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/file.h>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace frost::test {

/*
 * Holds the lock from construction (waiting up to `timeout`, polling once a second: the holder is
 * another process, which signals nothing) until destruction. held() says whether it was taken; the
 * file is created when missing. `path` is UTF-8. Not copyable; one thread.
 */
class EngineBuildLock {
public:
    explicit EngineBuildLock(const std::string& path, std::chrono::seconds timeout = std::chrono::hours(2)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
#ifdef _WIN32
        const int length = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(), length);
        m_handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE)
            return;
        for (;;) {
            OVERLAPPED at{};
            if (LockFileEx(m_handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at) != 0) {
                m_held = true;
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return;
            std::this_thread::sleep_for(std::chrono::seconds(1)); // Another process holds it; it signals nothing.
        }
#else
        m_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (m_fd < 0)
            return;
        for (;;) {
            if (::flock(m_fd, LOCK_EX | LOCK_NB) == 0) {
                m_held = true;
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return;
            std::this_thread::sleep_for(std::chrono::seconds(1)); // Another process holds it; it signals nothing.
        }
#endif
    }
    ~EngineBuildLock() {
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE) {
            if (m_held) {
                OVERLAPPED at{};
                UnlockFileEx(m_handle, 0, 1, 0, &at);
            }
            CloseHandle(m_handle);
        }
#else
        if (m_fd >= 0)
            ::close(m_fd); // Closing the descriptor releases the flock.
#endif
    }
    EngineBuildLock(const EngineBuildLock&) = delete ("owns a file lock");
    EngineBuildLock& operator=(const EngineBuildLock&) = delete ("owns a file lock");

    [[nodiscard]] bool held() const noexcept { return m_held; }

private:
#ifdef _WIN32
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
    bool m_held = false;
};

} // namespace frost::test
