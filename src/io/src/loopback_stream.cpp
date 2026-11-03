// SPDX-License-Identifier: Apache-2.0
// Cross-platform loopback TCP streams for local automation connections.
#include "frost/io/loopback_stream.hpp"

#include <algorithm>
#include <array>
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

namespace frost::io {
namespace {
constexpr std::size_t kMaxLine = 16 * 1024 * 1024;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
int lastError() { return WSAGetLastError(); }
bool pending(int code) { return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS || code == WSAEALREADY; }
void closeSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalid = -1;
int lastError() { return errno; }
bool pending(int code) { return code == EAGAIN || code == EWOULDBLOCK || code == EINPROGRESS || code == EALREADY; }
void closeSocket(Socket socket) { close(socket); }
#endif
} // namespace

struct LoopbackStream::Impl {
    Socket socket = kInvalid;
    Deadline deadline;
    std::stop_token stop;
    std::string buffered;
#ifdef _WIN32
    bool winsock = false;
#endif
    ~Impl() {
        if (socket != kInvalid)
            closeSocket(socket);
#ifdef _WIN32
        if (winsock)
            WSACleanup();
#endif
    }
    Result<void> ready(bool write) {
        for (;;) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "loopback request cancelled");
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return fail(ErrorCode::Timeout, "loopback request timed out");
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            const auto slice = std::min(remaining, std::chrono::microseconds(50000));
            timeval timeout{static_cast<long>(slice.count() / 1000000), static_cast<long>(slice.count() % 1000000)};
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            const int result =
                select(static_cast<int>(socket + 1), write ? nullptr : &set, write ? &set : nullptr, nullptr, &timeout);
            if (result > 0)
                return {};
            if (result < 0) {
#ifndef _WIN32
                if (errno == EINTR)
                    continue;
#endif
                return fail(ErrorCode::IoError, "loopback socket wait failed");
            }
        }
    }
};

LoopbackStream::LoopbackStream(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
LoopbackStream::~LoopbackStream() = default;

Result<std::unique_ptr<LoopbackStream>> LoopbackStream::connect(std::uint16_t port, Deadline deadline,
                                                                std::stop_token stop) {
    if (!port)
        return fail(ErrorCode::InvalidArgument, "invalid loopback port");
    auto impl = std::make_unique<Impl>();
    impl->deadline = deadline;
    impl->stop = stop;
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "loopback request cancelled");
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return fail(ErrorCode::IoError, "cannot initialize loopback socket");
    impl->winsock = true;
#endif
    impl->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl->socket == kInvalid)
        return fail(ErrorCode::IoError, "cannot create loopback socket");
#ifndef _WIN32
    if (impl->socket >= FD_SETSIZE)
        return fail(ErrorCode::OutOfRange, "loopback socket exceeds select limit");
#endif
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(impl->socket, static_cast<long>(FIONBIO), &nonblocking) != 0)
#else
    if (fcntl(impl->socket, F_SETFL, O_NONBLOCK) < 0)
#endif
        return fail(ErrorCode::IoError, "cannot set nonblocking loopback socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(impl->socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (!pending(lastError()))
            return fail(ErrorCode::IoError, "cannot connect to loopback server");
        if (auto ready = impl->ready(true); !ready)
            return std::unexpected(std::move(ready.error()));
        int error = 0;
#ifdef _WIN32
        int length = sizeof(error);
        if (getsockopt(impl->socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 || error)
#else
        socklen_t length = sizeof(error);
        if (getsockopt(impl->socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error)
#endif
            return fail(ErrorCode::IoError, "cannot connect to loopback server");
    }
    return std::unique_ptr<LoopbackStream>(new LoopbackStream(std::move(impl)));
}

Result<std::string> LoopbackStream::exchange(std::string_view line, std::size_t maxReplyBytes) {
    if (line.size() > kMaxLine || maxReplyBytes == 0 || maxReplyBytes > kMaxLine ||
        line.find('\n') != std::string_view::npos)
        return fail(ErrorCode::InvalidArgument, "invalid loopback message");
    std::string wire(line);
    wire.push_back('\n');
    std::size_t sent = 0;
    while (sent < wire.size()) {
        if (auto ready = m_impl->ready(true); !ready)
            return std::unexpected(std::move(ready.error()));
        const int size = static_cast<int>(std::min<std::size_t>(wire.size() - sent, 65536));
#ifdef _WIN32
        const int count = send(m_impl->socket, wire.data() + sent, size, 0);
#else
        const auto count = send(m_impl->socket, wire.data() + sent, static_cast<std::size_t>(size), MSG_NOSIGNAL);
#endif
        if (count > 0)
            sent += static_cast<std::size_t>(count);
        else if (count == 0 || !pending(lastError()))
            return fail(ErrorCode::IoError, "loopback send failed");
    }
    for (;;) {
        const auto newline = m_impl->buffered.find('\n');
        if (newline != std::string::npos) {
            if (newline > maxReplyBytes)
                return fail(ErrorCode::OutOfRange, "loopback response exceeds limit");
            std::string response = m_impl->buffered.substr(0, newline);
            m_impl->buffered.erase(0, newline + 1);
            return response;
        }
        if (m_impl->buffered.size() > maxReplyBytes)
            return fail(ErrorCode::OutOfRange, "loopback response exceeds limit");
        if (auto ready = m_impl->ready(false); !ready)
            return std::unexpected(std::move(ready.error()));
        std::array<char, 4096> bytes{};
        const auto count = recv(m_impl->socket, bytes.data(), static_cast<int>(bytes.size()), 0);
        if (count > 0)
            m_impl->buffered.append(bytes.data(), static_cast<std::size_t>(count));
        else if (count == 0 || !pending(lastError()))
            return fail(ErrorCode::IoError, "loopback connection closed");
    }
}
} // namespace frost::io
