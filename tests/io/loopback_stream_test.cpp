// SPDX-License-Identifier: Apache-2.0
// Tests for loopback TCP stream connections.
#include "frost/io/loopback_stream.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <stop_token>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>

#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
void closeSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalid = -1;
void closeSocket(Socket socket) { close(socket); }
#endif
class Listener {
public:
    Listener() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return;
        m_initialized = true;
#endif
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == kInvalid)
            return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(m_socket, 1) != 0)
            return;
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &length) == 0)
            port = ntohs(address.sin_port);
    }
    ~Listener() {
        if (m_socket != kInvalid)
            closeSocket(m_socket);
#ifdef _WIN32
        if (m_initialized)
            WSACleanup();
#endif
    }
    Socket acceptOne() {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(m_socket, &set);
        timeval timeout{3, 0};
        if (select(static_cast<int>(m_socket + 1), &set, nullptr, nullptr, &timeout) <= 0)
            return kInvalid;
        return accept(m_socket, nullptr, nullptr);
    }
    std::uint16_t port = 0;

private:
    Socket m_socket = kInvalid;
#ifdef _WIN32
    bool m_initialized = false;
#endif
};

TEST(LoopbackStream, ReadsFragmentedLinesAcrossRequests) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::jthread server([&] {
        Socket peer = listener.acceptOne();
        if (peer == kInvalid)
            return;
        char buffer[64];
        if (recv(peer, buffer, sizeof(buffer), 0) > 0) {
            static_cast<void>(send(peer, "first", 5, 0));
            static_cast<void>(send(peer, "\n", 1, 0));
            if (recv(peer, buffer, sizeof(buffer), 0) > 0)
                static_cast<void>(send(peer, "second\n", 7, 0));
        }
        closeSocket(peer);
    });
    auto stream = frost::io::LoopbackStream::connect(listener.port, std::chrono::steady_clock::now() + 3s);
    ASSERT_TRUE(stream);
    EXPECT_EQ((*stream)->exchange("one").value_or(""), "first");
    EXPECT_EQ((*stream)->exchange("two").value_or(""), "second");
}

TEST(LoopbackStream, EnforcesCallerReplyLimit) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::jthread server([&] {
        Socket peer = listener.acceptOne();
        if (peer == kInvalid)
            return;
        char buffer[16]{};
        (void)recv(peer, buffer, sizeof(buffer), 0);
        const std::string message = std::string(129, 'x') + "\n";
#ifdef _WIN32
        (void)send(peer, message.data(), static_cast<int>(message.size()), 0);
#else
        (void)send(peer, message.data(), message.size(), MSG_NOSIGNAL);
#endif
        closeSocket(peer);
    });
    auto stream = frost::io::LoopbackStream::connect(listener.port, std::chrono::steady_clock::now() + 3s);
    ASSERT_TRUE(stream);
    auto reply = (*stream)->exchange("hello", 128);
    ASSERT_FALSE(reply);
    EXPECT_EQ(reply.error().code(), frost::ErrorCode::OutOfRange);
}

TEST(LoopbackStream, DuplexBytesAndCloseWakeAReader) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::jthread server([&] {
        const Socket peer = listener.acceptOne();
        if (peer == kInvalid)
            return;
        std::array<char, 16> bytes{};
        if (recv(peer, bytes.data(), static_cast<int>(bytes.size()), 0) > 0) {
            static_cast<void>(send(peer, "world", 5, 0));
            static_cast<void>(recv(peer, bytes.data(), static_cast<int>(bytes.size()), 0));
        }
        closeSocket(peer);
    });
    auto stream = frost::io::LoopbackStream::connectDuplex(listener.port, std::chrono::steady_clock::now() + 3s);
    ASSERT_TRUE(stream);
    ASSERT_TRUE((*stream)->write("hello"));
    std::array<char, 16> bytes{};
    std::size_t count = 0;
    while (count < 5) {
        auto read = (*stream)->read(std::span<char>(bytes).subspan(count));
        ASSERT_TRUE(read);
        ASSERT_GT(*read, 0U);
        count += *read;
    }
    EXPECT_EQ(std::string_view(bytes.data(), count), "world");
    auto blocked = std::async(std::launch::async, [&] { return (*stream)->read(bytes); });
    (*stream)->close();
    ASSERT_EQ(blocked.wait_for(3s), std::future_status::ready);
    const auto result = blocked.get();
    EXPECT_TRUE(!result || *result == 0);
    (*stream)->close();
}

TEST(LoopbackStream, CancelsPendingReadWithoutWaitingForServer) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::jthread server([&] {
        Socket peer = listener.acceptOne();
        if (peer == kInvalid)
            return;
        char buffer[64];
        static_cast<void>(recv(peer, buffer, sizeof(buffer), 0));
        std::this_thread::sleep_for(500ms);
        closeSocket(peer);
    });
    std::stop_source stop;
    auto stream =
        frost::io::LoopbackStream::connect(listener.port, std::chrono::steady_clock::now() + 3s, stop.get_token());
    ASSERT_TRUE(stream);
    std::jthread cancel([&] {
        std::this_thread::sleep_for(50ms);
        stop.request_stop();
    });
    const auto start = std::chrono::steady_clock::now();
    auto result = (*stream)->exchange("waiting");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Cancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 400ms);
}
} // namespace
