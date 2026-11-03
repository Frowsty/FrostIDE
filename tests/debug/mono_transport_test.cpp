// SPDX-License-Identifier: Apache-2.0
// Test Mono loopback transport and selected-process verification.
#include "frost/debug/mono_transport.hpp"
#include "frost/debug/mono_windows_peer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>

#include <unistd.h>
#endif
namespace frost::debug::mono {
namespace {
using namespace std::chrono_literals;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket invalid = -1;
void closeSocket(Socket s) { ::close(s); }
#endif
class Listener {
public:
    Listener() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return;
        m_winsock = true;
#endif
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == invalid)
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
        if (m_socket != invalid)
            closeSocket(m_socket);
#ifdef _WIN32
        if (m_winsock)
            WSACleanup();
#endif
    }
    Socket acceptOne() {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(m_socket, &set);
        timeval timeout{3, 0};
        if (select(static_cast<int>(m_socket + 1), &set, nullptr, nullptr, &timeout) <= 0)
            return invalid;
        return accept(m_socket, nullptr, nullptr);
    }
    std::uint16_t port = 0;

private:
    Socket m_socket = invalid;
#ifdef _WIN32
    bool m_winsock = false;
#endif
};
TEST(MonoTransport, RejectsPeerBeforeAnyProtocolBytes) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::atomic<int> received{-2};
    std::jthread server([&] {
        const auto peer = listener.acceptOne();
        if (peer == invalid)
            return;
        char c = 0;
        received = static_cast<int>(recv(peer, &c, 1, 0));
        closeSocket(peer);
    });
    bool called = false;
    auto stream = BinaryLoopback::connect(
        listener.port, std::chrono::steady_clock::now() + 3s, {},
        [&](std::uint16_t local, std::uint16_t remote, MonoDeadline, std::stop_token) -> Result<void> {
            called = local != 0 && remote == listener.port;
            return fail(ErrorCode::PermissionDenied, "unverified peer");
        });
    EXPECT_FALSE(stream);
    EXPECT_TRUE(called);
    server.join();
    EXPECT_EQ(received.load(), 0);
}
TEST(MonoTransport, RechecksPeerBeforeWritingAndClosesOnFailure) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::atomic<int> received{-2};
    std::jthread server([&] {
        const auto peer = listener.acceptOne();
        if (peer == invalid)
            return;
        char c = 0;
        received = static_cast<int>(recv(peer, &c, 1, 0));
        closeSocket(peer);
    });
    int calls = 0;
    auto stream =
        BinaryLoopback::connect(listener.port, std::chrono::steady_clock::now() + 3s, {},
                                [&](std::uint16_t, std::uint16_t, MonoDeadline, std::stop_token) -> Result<void> {
                                    if (++calls > 1)
                                        return fail(ErrorCode::PermissionDenied, "peer changed");
                                    return {};
                                });
    ASSERT_TRUE(stream);
    const std::array<std::uint8_t, 1> byte{42};
    EXPECT_FALSE((*stream)->write(byte));
    server.join();
    EXPECT_EQ(received.load(), 0);
}
TEST(MonoTransport, IdlePollKeepsConnectionUsableAcrossDeadlineRenewal) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::atomic<bool> sendNow{false};
    std::jthread server([&] {
        const auto peer = listener.acceptOne();
        if (peer == invalid)
            return;
        const auto limit = std::chrono::steady_clock::now() + 3s;
        while (!sendNow.load() && std::chrono::steady_clock::now() < limit)
            std::this_thread::yield();
        if (!sendNow.load()) {
            closeSocket(peer);
            return;
        }
        const char c = 42;
        (void)send(peer, &c, 1, 0);
        char ack = 0;
        (void)recv(peer, &ack, 1, 0);
        closeSocket(peer);
    });
    auto stream = BinaryLoopback::connect(listener.port, std::chrono::steady_clock::now() + 3s, {},
                                          [](auto, auto, auto, auto) -> Result<void> { return {}; });
    ASSERT_TRUE(stream);
    auto idle = (*stream)->pollReadable(std::chrono::steady_clock::now() + 20ms);
    ASSERT_TRUE(idle);
    EXPECT_FALSE(*idle);
    ASSERT_TRUE((*stream)->renewDeadline(std::chrono::steady_clock::now() + 3s));
    sendNow = true;
    auto ready = (*stream)->pollReadable(std::chrono::steady_clock::now() + 2s);
    ASSERT_TRUE(ready);
    ASSERT_TRUE(*ready);
    std::array<std::uint8_t, 1> received{};
    ASSERT_TRUE((*stream)->read(received));
    EXPECT_EQ(received[0], 42);
    ASSERT_TRUE((*stream)->write(received));
}
TEST(MonoTransport, IdlePollRevocationClosesConnection) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::jthread server([&] {
        const auto peer = listener.acceptOne();
        if (peer == invalid)
            return;
        char c = 0;
        (void)recv(peer, &c, 1, 0);
        closeSocket(peer);
    });
    bool valid = true;
    auto stream = BinaryLoopback::connect(listener.port, std::chrono::steady_clock::now() + 3s, {},
                                          [&](auto, auto, auto, auto) -> Result<void> {
                                              if (!valid)
                                                  return fail(ErrorCode::PermissionDenied, "revoked");
                                              return {};
                                          });
    ASSERT_TRUE(stream);
    valid = false;
    EXPECT_FALSE((*stream)->pollReadable(std::chrono::steady_clock::now() + 10ms));
    EXPECT_FALSE((*stream)->renewDeadline(std::chrono::steady_clock::now() + 3s));
}
TEST(MonoTransport, IdleVerificationTimeoutDoesNotCloseStream) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::jthread server([&] {
        const auto peer = listener.acceptOne();
        if (peer == invalid)
            return;
        char c = 0;
        (void)recv(peer, &c, 1, 0);
        closeSocket(peer);
    });
    int calls = 0;
    auto stream = BinaryLoopback::connect(listener.port, std::chrono::steady_clock::now() + 3s, {},
                                          [&](auto, auto, auto deadline, auto) -> Result<void> {
                                              if (++calls == 2) {
                                                  std::this_thread::sleep_until(deadline);
                                                  return fail(ErrorCode::Timeout, "idle budget expired");
                                              }
                                              return {};
                                          });
    ASSERT_TRUE(stream);
    auto idle = (*stream)->pollReadable(std::chrono::steady_clock::now() + 20ms);
    ASSERT_TRUE(idle);
    EXPECT_FALSE(*idle);
    EXPECT_TRUE((*stream)->renewDeadline(std::chrono::steady_clock::now() + 3s));
    const std::array<std::uint8_t, 1> byte{42};
    EXPECT_TRUE((*stream)->write(byte));
}
TEST(MonoTransport, RejectsMissingVerifierAndInvalidPort) {
    EXPECT_FALSE(BinaryLoopback::connect(0, std::chrono::steady_clock::now() + 1s, {}, {}));
    EXPECT_FALSE(BinaryLoopback::connect(1234, std::chrono::steady_clock::now() + 1s, {}, {}));
}
TEST(MonoTransport, ExpiredDeadlineRejectsBeforeConnect) {
    bool called = false;
    auto result =
        BinaryLoopback::connect(1234, std::chrono::steady_clock::now() - 1s, {},
                                [&](std::uint16_t, std::uint16_t, MonoDeadline, std::stop_token) -> Result<void> {
                                    called = true;
                                    return {};
                                });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);
    EXPECT_FALSE(called);
}
TEST(MonoTransport, PeerVerifierRejectsUnboundIdentity) {
    EXPECT_FALSE(WindowsPeer::open(0, 1, [] { return true; }));
    EXPECT_FALSE(WindowsPeer::open(1, 0, [] { return true; }));
    EXPECT_FALSE(WindowsPeer::open(1, 1, {}));
}
#ifdef _WIN32
TEST(MonoTransport, WindowsPeerBindsEstablishedTupleToHeldProcess) {
    FILETIME created{}, exited{}, kernel{}, user{};
    ASSERT_TRUE(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
    const auto creation = std::uint64_t(created.dwLowDateTime) | (std::uint64_t(created.dwHighDateTime) << 32);
    EXPECT_FALSE(
        WindowsPeer::open(static_cast<std::uint32_t>(GetCurrentProcessId()), creation ^ 1, [] { return true; }));
    auto peerVerifier =
        WindowsPeer::open(static_cast<std::uint32_t>(GetCurrentProcessId()), creation, [] { return true; });
    ASSERT_TRUE(peerVerifier);
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto cancelledScan =
        (*peerVerifier)->verify(1, 2, std::chrono::steady_clock::now() + 1s, cancelled.get_token());
    ASSERT_FALSE(cancelledScan);
    EXPECT_EQ(cancelledScan.error().code(), ErrorCode::Cancelled);
    std::stop_source duringIdentity;
    int identityChecks = 0;
    auto interrupted = WindowsPeer::open(static_cast<std::uint32_t>(GetCurrentProcessId()), creation, [&] {
        if (++identityChecks == 2)
            duringIdentity.request_stop();
        return true;
    });
    ASSERT_TRUE(interrupted);
    const auto stoppedScan =
        (*interrupted)->verify(1, 2, std::chrono::steady_clock::now() + 1s, duringIdentity.get_token());
    ASSERT_FALSE(stoppedScan);
    EXPECT_EQ(stoppedScan.error().code(), ErrorCode::Cancelled);
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::atomic_bool accepted{false};
    std::atomic_bool allowClose{false};
    std::jthread server([&] {
        const auto peer = listener.acceptOne();
        if (peer == invalid)
            return;
        accepted = true;
        char c = 0;
        static_cast<void>(recv(peer, &c, 1, 0));
        const auto end = std::chrono::steady_clock::now() + 3s;
        while (!allowClose && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        closeSocket(peer);
    });
    auto stream = BinaryLoopback::connect(
        listener.port, std::chrono::steady_clock::now() + 3s, {},
        [&](std::uint16_t local, std::uint16_t remote, MonoDeadline deadline, std::stop_token stop) -> Result<void> {
            const auto end = std::chrono::steady_clock::now() + 2s;
            while (!accepted && std::chrono::steady_clock::now() < end)
                std::this_thread::yield();
            return (*peerVerifier)->verify(local, remote, deadline, stop);
        });
    ASSERT_TRUE(stream);
    const std::array<std::uint8_t, 1> sent{42};
    const auto written = (*stream)->write(sent);
    allowClose = true;
    EXPECT_TRUE(written);
}
#endif
TEST(MonoTransport, ExchangesBinaryBytesWithVerifiedLoopbackPeer) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::jthread server([&] {
        const auto peer = listener.acceptOne();
        if (peer == invalid)
            return;
        char c = 0;
        if (recv(peer, &c, 1, 0) == 1)
            static_cast<void>(send(peer, &c, 1, 0));
        closeSocket(peer);
    });
    auto stream = BinaryLoopback::connect(
        listener.port, std::chrono::steady_clock::now() + 3s, {},
        [](std::uint16_t, std::uint16_t, MonoDeadline, std::stop_token) -> Result<void> { return {}; });
    ASSERT_TRUE(stream);
    const std::array<std::uint8_t, 1> sent{0};
    ASSERT_TRUE((*stream)->write(sent));
    std::array<std::uint8_t, 1> received{};
    const auto count = (*stream)->read(received);
    ASSERT_TRUE(count);
    EXPECT_EQ(*count, 1u);
    EXPECT_EQ(received, sent);
}
TEST(MonoTransport, CancellationDuringPostSendVerificationDoesNotReportSuccess) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::jthread server([&] {
        const auto peer = listener.acceptOne();
        if (peer == invalid)
            return;
        char c = 0;
        static_cast<void>(recv(peer, &c, 1, 0));
        closeSocket(peer);
    });
    std::stop_source stop;
    int calls = 0;
    auto stream =
        BinaryLoopback::connect(listener.port, std::chrono::steady_clock::now() + 3s, stop.get_token(),
                                [&](std::uint16_t, std::uint16_t, MonoDeadline, std::stop_token) -> Result<void> {
                                    if (++calls == 4)
                                        stop.request_stop();
                                    return {};
                                });
    ASSERT_TRUE(stream);
    const std::array<std::uint8_t, 1> byte{42};
    const auto result = (*stream)->write(byte);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
}
TEST(MonoTransport, CancellationDuringPostReceiveVerificationDiscardsResult) {
    Listener listener;
    ASSERT_NE(listener.port, 0);
    std::jthread server([&] {
        const auto peer = listener.acceptOne();
        if (peer == invalid)
            return;
        char c = 42;
        static_cast<void>(send(peer, &c, 1, 0));
        closeSocket(peer);
    });
    std::stop_source stop;
    int calls = 0;
    auto stream =
        BinaryLoopback::connect(listener.port, std::chrono::steady_clock::now() + 3s, stop.get_token(),
                                [&](std::uint16_t, std::uint16_t, MonoDeadline, std::stop_token) -> Result<void> {
                                    if (++calls == 3)
                                        stop.request_stop();
                                    return {};
                                });
    ASSERT_TRUE(stream);
    std::array<std::uint8_t, 1> buffer{};
    const auto result = (*stream)->read(buffer);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
}
} // namespace
} // namespace frost::debug::mono
