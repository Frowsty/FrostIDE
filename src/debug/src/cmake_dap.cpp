// SPDX-License-Identifier: Apache-2.0
#include "frost/debug/cmake_dap.hpp"
#include "frost/io/utf.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
namespace frost::debug {
namespace {
bool validEndpoint(std::string_view endpoint) {
    if (endpoint.find('\0') != std::string_view::npos) return false;
#ifdef _WIN32
    return endpoint.starts_with("\\\\.\\pipe\\FrostIDE-CMake-");
#else
    return !endpoint.empty() && endpoint.front() == '/' && endpoint.size() < sizeof(sockaddr_un{}.sun_path);
#endif
}
class CMakeTransport final : public IDapTransport {
public:
#ifdef _WIN32
    using Handle = HANDLE;
#else
    using Handle = int;
#endif
    CMakeTransport(Handle handle, std::function<void()> cancel) : m_handle(handle), m_cancel(std::move(cancel)) {}
    ~CMakeTransport() override {
        close();
#ifdef _WIN32
        CloseHandle(m_handle);
#else
        ::close(m_handle);
#endif
    }
    Result<std::size_t> read(std::span<char> bytes) override {
#ifdef _WIN32
        return transfer(bytes.data(), bytes.size(), false);
#else
        ssize_t count;
        do { count = ::recv(m_handle, bytes.data(), bytes.size(), 0); } while (count < 0 && errno == EINTR);
        if (count < 0 && !m_closed) return fail(ErrorCode::IoError, "CMake debugger socket read failed");
        return count < 0 ? 0 : static_cast<std::size_t>(count);
#endif
    }
    Result<void> write(std::string_view bytes) override {
        while (!bytes.empty()) {
#ifdef _WIN32
            auto count = transfer(const_cast<char*>(bytes.data()), bytes.size(), true);
            if (!count) return std::unexpected(count.error());
            if (*count == 0) return fail(ErrorCode::IoError, "CMake debugger pipe closed");
            bytes.remove_prefix(*count);
#else
            ssize_t count;
            do { count = ::send(m_handle, bytes.data(), bytes.size(), MSG_NOSIGNAL); } while (count < 0 && errno == EINTR);
            if (count <= 0) return fail(ErrorCode::IoError, "CMake debugger socket write failed");
            bytes.remove_prefix(static_cast<std::size_t>(count));
#endif
        }
        return {};
    }
    void close() noexcept override {
        if (m_closed.exchange(true)) return;
#ifdef _WIN32
        CancelIoEx(m_handle, nullptr);
#else
        ::shutdown(m_handle, SHUT_RDWR);
#endif
        if (m_cancel) m_cancel();
    }
private:
#ifdef _WIN32
    Result<std::size_t> transfer(char* bytes, std::size_t size, bool writing) {
        if (m_closed) return writing ? Result<std::size_t>(fail(ErrorCode::IoError, "CMake debugger pipe closed"))
                                     : Result<std::size_t>(0);
        OVERLAPPED op{};
        op.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!op.hEvent) return fail(ErrorCode::IoError, "CMake debugger I/O event failed");
        DWORD count = 0;
        const DWORD length = static_cast<DWORD>(std::min<std::size_t>(size, 1024 * 1024));
        const bool immediate = writing ? WriteFile(m_handle, bytes, length, &count, &op)
                                       : ReadFile(m_handle, bytes, length, &count, &op);
        DWORD error = immediate ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_IO_PENDING) {
            // Covers close racing with submission after CancelIoEx ran.
            if (m_closed) CancelIoEx(m_handle, &op);
            if (!GetOverlappedResult(m_handle, &op, &count, TRUE)) error = GetLastError();
            else error = ERROR_SUCCESS;
        }
        CloseHandle(op.hEvent);
        if (!writing && (error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED)) return 0;
        if (error != ERROR_SUCCESS) return fail(ErrorCode::IoError, "CMake debugger pipe transfer failed");
        return static_cast<std::size_t>(count);
    }
#endif
    Handle m_handle;
    std::function<void()> m_cancel;
    std::atomic<bool> m_closed{false};
};
Result<std::unique_ptr<IDapTransport>> connect(std::string_view endpoint, std::function<void()> cancel,
                                              std::chrono::milliseconds timeout, std::stop_token stop) {
    if (endpoint.find('\0') != std::string_view::npos || timeout <= std::chrono::milliseconds::zero())
        return fail(ErrorCode::InvalidArgument, "Invalid CMake debugger endpoint or deadline");
#ifdef _WIN32
    if (!endpoint.starts_with("\\\\.\\pipe\\FrostIDE-CMake-"))
        return fail(ErrorCode::InvalidArgument, "CMake debugger requires a private FrostIDE local pipe");
    auto wide = io::toWide(endpoint);
    if (!wide) return std::unexpected(wide.error());
#else
    if (endpoint.empty() || endpoint.front() != '/' || endpoint.size() >= sizeof(sockaddr_un{}.sun_path))
        return fail(ErrorCode::InvalidArgument, "CMake debugger requires an absolute local socket path");
#endif
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "CMake debugger connection cancelled");
#ifdef _WIN32
        auto handle = CreateFileW(wide->c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
            return std::unique_ptr<IDapTransport>(new CMakeTransport(handle, std::move(cancel)));
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PIPE_BUSY)
            return fail(ErrorCode::IoError, "Cannot open CMake debugger pipe");
#else
        int handle = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (handle < 0) return fail(ErrorCode::IoError, "Cannot create CMake debugger socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
        int status = ::connect(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (status < 0 && errno == EINPROGRESS) {
            pollfd ready{handle, POLLOUT, 0};
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                if (::poll(&ready, 1, 10) > 0) {
                    int error = 0;
                    socklen_t length = sizeof(error);
                    if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) status = 0;
                    else errno = error;
                    break;
                }
            }
        }
        if (status == 0 && ::fcntl(handle, F_SETFL, 0) == 0)
            return std::unique_ptr<IDapTransport>(new CMakeTransport(handle, std::move(cancel)));
        const int error = errno;
        ::close(handle);
        if (error != ENOENT && error != ECONNREFUSED && error != EINTR && error != EAGAIN && error != EINPROGRESS)
            return fail(ErrorCode::IoError, "Cannot connect to CMake debugger socket");
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return fail(ErrorCode::Timeout, "CMake debugger did not open its endpoint before the deadline");
}
// Factory construction never waits on the GUI thread. The owned connector publishes the
// transport to the DAP reader/writer, while close cancels startup and wakes both waiters.
class ConnectingTransport final : public IDapTransport {
public:
    ConnectingTransport(std::string endpoint, std::function<void()> cancel, std::chrono::milliseconds timeout)
        : m_cancel(std::move(cancel)), m_worker([this, endpoint = std::move(endpoint), timeout](std::stop_token stop) {
            auto connected = connect(endpoint, {}, timeout, stop);
            {
                std::lock_guard lock(m_mutex);
                if (connected) {
                    m_transport = std::move(*connected);
                    if (m_closed) m_transport->close();
                } else m_error = connected.error();
                m_ready = true;
            }
            m_changed.notify_all();
        }) {}
    ~ConnectingTransport() override { close(); if (m_worker.joinable()) m_worker.join(); }
    Result<std::size_t> read(std::span<char> bytes) override {
        auto stream = wait();
        if (!stream) return std::unexpected(stream.error());
        return (*stream)->read(bytes);
    }
    Result<void> write(std::string_view bytes) override {
        auto stream = wait();
        if (!stream) return std::unexpected(stream.error());
        return (*stream)->write(bytes);
    }
    void close() noexcept override {
        IDapTransport* stream = nullptr;
        {
            std::lock_guard lock(m_mutex);
            if (m_closed) return;
            m_closed = true;
            stream = m_transport.get();
        }
        m_worker.request_stop();
        m_changed.notify_all();
        if (stream) stream->close();
        if (m_cancel) m_cancel();
    }
private:
    Result<IDapTransport*> wait() {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [this] { return m_ready || m_closed; });
        if (m_closed) return fail(ErrorCode::Cancelled, "CMake debugger connection closed");
        if (m_error) return std::unexpected(*m_error);
        return m_transport.get();
    }
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::unique_ptr<IDapTransport> m_transport;
    std::optional<Error> m_error;
    bool m_ready = false;
    bool m_closed = false;
    std::function<void()> m_cancel;
    std::jthread m_worker;
};
} // namespace
AdapterFactory cmakeDapFactory(std::string endpoint, std::function<void()> cancelConfigure,
                              std::chrono::milliseconds timeout) {
    return [endpoint = std::move(endpoint), cancel = std::move(cancelConfigure), timeout](DapHandlers handlers)
        -> Result<std::unique_ptr<IDebugAdapterClient>> {
        if (!validEndpoint(endpoint) || timeout <= std::chrono::milliseconds::zero()) {
            if (cancel) cancel();
            return fail(ErrorCode::InvalidArgument, "Invalid CMake debugger endpoint or deadline");
        }
        return makeDapClient(std::make_unique<ConnectingTransport>(endpoint, cancel, timeout), std::move(handlers));
    };
}
} // namespace frost::debug
