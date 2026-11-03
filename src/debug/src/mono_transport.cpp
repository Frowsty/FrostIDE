// SPDX-License-Identifier: Apache-2.0
// Transport bounded Mono protocol bytes over verified loopback connections.
#include "frost/debug/mono_transport.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace frost::debug::mono {
namespace {
constexpr std::size_t maxTransfer = 1024 * 1024;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid = INVALID_SOCKET;
int errorCode() { return WSAGetLastError(); }
bool pending(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
void closeSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket invalid = -1;
int errorCode() { return errno; }
bool pending(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EALREADY; }
void closeSocket(Socket s) { ::close(s); }
#endif
} // namespace
struct BinaryLoopback::Impl {
    Socket socket = invalid;
    Deadline deadline;
    std::stop_token stop;
    VerifyPeer verify;
    std::uint16_t localPort = 0;
    std::uint16_t remotePort = 0;
#ifdef _WIN32
    bool winsock = false;
#endif
    ~Impl() {
        if (socket != invalid)
            closeSocket(socket);
#ifdef _WIN32
        if (winsock)
            WSACleanup();
#endif
    }
    Result<void> check() const {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Mono transport cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            return fail(ErrorCode::Timeout, "Mono transport deadline reached");
        return {};
    }
    Result<void> ready(bool writable) {
        for (;;) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Mono transport cancelled");
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return fail(ErrorCode::Timeout, "Mono transport deadline reached");
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            const auto slice = std::min(left, std::chrono::microseconds(50000));
            timeval timeout{static_cast<long>(slice.count() / 1000000), static_cast<long>(slice.count() % 1000000)};
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            const int result = select(static_cast<int>(socket + 1), writable ? nullptr : &set,
                                      writable ? &set : nullptr, nullptr, &timeout);
            if (result > 0)
                return {};
            if (result < 0) {
#ifndef _WIN32
                if (errno == EINTR)
                    continue;
#endif
                return fail(ErrorCode::IoError, "Mono socket wait failed");
            }
        }
    }
};
BinaryLoopback::BinaryLoopback(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
BinaryLoopback::~BinaryLoopback() = default;
void BinaryLoopback::close() noexcept { m_impl.reset(); }

Result<std::unique_ptr<BinaryLoopback>> BinaryLoopback::connect(std::uint16_t port, Deadline deadline,
                                                                std::stop_token stop, VerifyPeer verify) {
    if (!port || !verify)
        return fail(ErrorCode::InvalidArgument, "Mono endpoint and verifier required");
    auto impl = std::make_unique<Impl>();
    impl->deadline = deadline;
    impl->stop = stop;
    impl->verify = std::move(verify);
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Mono transport cancelled");
    if (std::chrono::steady_clock::now() >= deadline)
        return fail(ErrorCode::Timeout, "Mono transport deadline reached before connect");
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return fail(ErrorCode::IoError, "Winsock startup failed");
    impl->winsock = true;
#endif
    impl->socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl->socket == invalid)
        return fail(ErrorCode::IoError, "Mono socket creation failed");
#ifndef _WIN32
    if (impl->socket >= FD_SETSIZE)
        return fail(ErrorCode::OutOfRange, "Mono socket exceeds select limit");
#endif
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(impl->socket, static_cast<long>(FIONBIO), &mode) != 0)
        return fail(ErrorCode::IoError, "Mono socket setup failed");
#else
    const int flags = fcntl(impl->socket, F_GETFL, 0);
    if (flags < 0 || fcntl(impl->socket, F_SETFL, flags | O_NONBLOCK) < 0)
        return fail(ErrorCode::IoError, "Mono socket setup failed");
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (auto valid = impl->check(); !valid)
        return std::unexpected(valid.error());
    if (::connect(impl->socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (!pending(errorCode()))
            return fail(ErrorCode::IoError, "Mono connection failed");
        auto ready = impl->ready(true);
        if (!ready)
            return std::unexpected(ready.error());
        int error = 0;
#ifdef _WIN32
        int length = sizeof(error);
        if (getsockopt(impl->socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 || error)
#else
        socklen_t length = sizeof(error);
        if (getsockopt(impl->socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error)
#endif
            return fail(ErrorCode::IoError, "Mono connection failed");
    }
    sockaddr_in local{}, peer{};
#ifdef _WIN32
    int localSize = sizeof(local), peerSize = sizeof(peer);
#else
    socklen_t localSize = sizeof(local), peerSize = sizeof(peer);
#endif
    if (getsockname(impl->socket, reinterpret_cast<sockaddr*>(&local), &localSize) != 0 ||
        getpeername(impl->socket, reinterpret_cast<sockaddr*>(&peer), &peerSize) != 0 || local.sin_family != AF_INET ||
        peer.sin_family != AF_INET || local.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
        peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || ntohs(peer.sin_port) != port || ntohs(local.sin_port) == 0)
        return fail(ErrorCode::IoError, "Mono connected tuple is invalid");
    impl->localPort = ntohs(local.sin_port);
    impl->remotePort = port;
    auto verified = impl->verify(impl->localPort, impl->remotePort, deadline, stop);
    if (!verified)
        return std::unexpected(verified.error());
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Mono connection cancelled during verification");
    if (std::chrono::steady_clock::now() >= deadline)
        return fail(ErrorCode::Timeout, "Mono connection expired during verification");
    return std::unique_ptr<BinaryLoopback>(new BinaryLoopback(std::move(impl)));
}
Result<void> BinaryLoopback::renewDeadline(Deadline deadline) {
    if (!m_impl)
        return fail(ErrorCode::Cancelled, "Mono transport closed");
    if (auto valid = m_impl->check(); !valid) {
        close();
        return std::unexpected(valid.error());
    }
    auto verified = m_impl->verify(m_impl->localPort, m_impl->remotePort, m_impl->deadline, m_impl->stop);
    if (!verified) {
        close();
        return std::unexpected(verified.error());
    }
    if (auto valid = m_impl->check(); !valid) {
        close();
        return std::unexpected(valid.error());
    }
    if (deadline <= std::chrono::steady_clock::now()) {
        close();
        return fail(ErrorCode::Timeout, "Mono renewal deadline already passed");
    }
    m_impl->deadline = deadline;
    return {};
}
Result<bool> BinaryLoopback::pollReadable(Deadline until) {
    if (!m_impl)
        return fail(ErrorCode::Cancelled, "Mono transport closed");
    for (;;) {
        if (auto valid = m_impl->check(); !valid) {
            close();
            return std::unexpected(valid.error());
        }
        if (std::chrono::steady_clock::now() >= until)
            return false;
        auto verified =
            m_impl->verify(m_impl->localPort, m_impl->remotePort, std::min(until, m_impl->deadline), m_impl->stop);
        if (!verified) {
            if (verified.error().code() == ErrorCode::Timeout && std::chrono::steady_clock::now() >= until &&
                std::chrono::steady_clock::now() < m_impl->deadline && !m_impl->stop.stop_requested())
                return false;
            close();
            return std::unexpected(verified.error());
        }
        if (auto valid = m_impl->check(); !valid) {
            close();
            return std::unexpected(valid.error());
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= until)
            return false;
        const auto slice = std::min({std::chrono::duration_cast<std::chrono::microseconds>(until - now),
                                     std::chrono::duration_cast<std::chrono::microseconds>(m_impl->deadline - now),
                                     std::chrono::microseconds(50000)});
        timeval timeout{static_cast<long>(slice.count() / 1000000), static_cast<long>(slice.count() % 1000000)};
        fd_set set;
        FD_ZERO(&set);
        FD_SET(m_impl->socket, &set);
        const int result = select(static_cast<int>(m_impl->socket + 1), &set, nullptr, nullptr, &timeout);
        if (result < 0) {
#ifndef _WIN32
            if (errno == EINTR)
                continue;
#endif
            close();
            return fail(ErrorCode::IoError, "Mono socket wait failed");
        }
        if (result > 0) {
            auto current =
                m_impl->verify(m_impl->localPort, m_impl->remotePort, std::min(until, m_impl->deadline), m_impl->stop);
            if (!current) {
                if (current.error().code() == ErrorCode::Timeout && std::chrono::steady_clock::now() >= until &&
                    std::chrono::steady_clock::now() < m_impl->deadline && !m_impl->stop.stop_requested())
                    return false;
                close();
                return std::unexpected(current.error());
            }
            if (auto valid = m_impl->check(); !valid) {
                close();
                return std::unexpected(valid.error());
            }
            if (std::chrono::steady_clock::now() >= until)
                return false;
            return true;
        }
    }
}
Result<void> BinaryLoopback::write(std::span<const std::uint8_t> bytes) {
    if (!m_impl)
        return fail(ErrorCode::Cancelled, "Mono transport closed");
    if (bytes.empty() || bytes.size() > maxTransfer)
        return fail(ErrorCode::OutOfRange, "invalid Mono write size");
    if (auto valid = m_impl->check(); !valid) {
        close();
        return std::unexpected(valid.error());
    }
    auto verified = m_impl->verify(m_impl->localPort, m_impl->remotePort, m_impl->deadline, m_impl->stop);
    if (!verified) {
        close();
        return std::unexpected(verified.error());
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto ready = m_impl->ready(true);
        if (!ready) {
            close();
            return std::unexpected(ready.error());
        }
        verified = m_impl->verify(m_impl->localPort, m_impl->remotePort, m_impl->deadline, m_impl->stop);
        if (!verified) {
            close();
            return std::unexpected(verified.error());
        }
        if (auto valid = m_impl->check(); !valid) {
            close();
            return std::unexpected(valid.error());
        }
#ifdef _WIN32
        const int n = ::send(m_impl->socket, reinterpret_cast<const char*>(bytes.data() + offset),
                             static_cast<int>(bytes.size() - offset), 0);
#else
        const auto n = ::send(m_impl->socket, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            if (n < 0 && pending(errorCode()))
                continue;
            close();
            return fail(ErrorCode::IoError, "Mono socket write failed");
        }
        offset += static_cast<std::size_t>(n);
        verified = m_impl->verify(m_impl->localPort, m_impl->remotePort, m_impl->deadline, m_impl->stop);
        if (!verified) {
            close();
            return std::unexpected(verified.error());
        }
        if (auto valid = m_impl->check(); !valid) {
            close();
            return std::unexpected(valid.error());
        }
    }
    return {};
}
Result<std::size_t> BinaryLoopback::read(std::span<std::uint8_t> buffer) {
    if (!m_impl)
        return fail(ErrorCode::Cancelled, "Mono transport closed");
    if (buffer.empty() || buffer.size() > maxTransfer)
        return fail(ErrorCode::OutOfRange, "invalid Mono read size");
    for (;;) {
        auto ready = m_impl->ready(false);
        if (!ready) {
            close();
            return std::unexpected(ready.error());
        }
        auto verified = m_impl->verify(m_impl->localPort, m_impl->remotePort, m_impl->deadline, m_impl->stop);
        if (!verified) {
            close();
            return std::unexpected(verified.error());
        }
        if (auto valid = m_impl->check(); !valid) {
            close();
            return std::unexpected(valid.error());
        }
#ifdef _WIN32
        const int n =
            ::recv(m_impl->socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
        const auto n = ::recv(m_impl->socket, buffer.data(), buffer.size(), 0);
#endif
        if (n > 0) {
            verified = m_impl->verify(m_impl->localPort, m_impl->remotePort, m_impl->deadline, m_impl->stop);
            if (!verified) {
                close();
                return std::unexpected(verified.error());
            }
            if (auto valid = m_impl->check(); !valid) {
                close();
                return std::unexpected(valid.error());
            }
            return static_cast<std::size_t>(n);
        }
        if (n < 0 && pending(errorCode()))
            continue;
        close();
        return fail(ErrorCode::IoError, "Mono socket closed or read failed");
    }
}
} // namespace frost::debug::mono
