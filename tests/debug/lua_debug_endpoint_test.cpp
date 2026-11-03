// SPDX-License-Identifier: Apache-2.0
// Authentication and byte preservation at the experimental Lua debug endpoint.
#include "frost/debug/lua_debug_adapter.hpp"
#include "frost/debug/lua_debug_endpoint.hpp"
#include "frost/rpc/framing.hpp"
#include "frost/rpc/json.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

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
using SocketLength = int;
constexpr Socket kInvalid = INVALID_SOCKET;
void closeSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
using SocketLength = std::size_t;
constexpr Socket kInvalid = -1;
void closeSocket(Socket socket) { close(socket); }
#endif

void exerciseReservedPlayStop(bool preAdmission, bool refuse = false) {
#ifdef _WIN32
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listener, kInvalid);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    std::atomic<bool> correct{false};
    std::jthread server([&] {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener, &ready);
        timeval timeout{5, 0};
        if (select(static_cast<int>(listener + 1), &ready, nullptr, nullptr, &timeout) <= 0)
            return;
        const Socket peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalid)
            return;
        frost::rpc::FrameDecoder decoder(2048);
        std::array<char, 1024> buffer{};
        int step = 0;
        while (step < (refuse ? 5 : 4)) {
            FD_ZERO(&ready);
            FD_SET(peer, &ready);
            timeval readTimeout{5, 0};
            if (select(static_cast<int>(peer + 1), &ready, nullptr, nullptr, &readTimeout) <= 0)
                break;
            const int count = static_cast<int>(recv(peer, buffer.data(), static_cast<SocketLength>(buffer.size()), 0));
            if (count <= 0)
                break;
            decoder.feed(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
            while (auto frame = decoder.next()) {
                if (!*frame)
                    break;
                const auto request = frost::rpc::parseJson(**frame);
                if (!request)
                    break;
                std::string response;
                if (step == 0 && request->stringAt("token") == "0123456789abcdef0123456789abcdef")
                    response = R"({"authenticated":true,"connectionGeneration":9})";
                else if (step == 1 && request->stringAt("command") == "reservePlay" &&
                         request->integerAt("requestId") == 1)
                    response = R"({"requestId":1,"success":true,"operationId":42})";
                else if (step == 2 && request->stringAt("command") == "armPlay" &&
                         request->integerAt("requestId") == 2 && request->integerAt("operationId") == 42) {
                    response = R"({"requestId":2,"success":true})";
                } else if (step == 3 && request->stringAt("command") == "stopPlay" &&
                           request->integerAt("requestId") == 3 && request->integerAt("operationId") == 42) {
                    response = refuse         ? R"({"requestId":3,"success":false})"
                               : preAdmission ? R"({"requestId":3,"success":true,"completed":true})"
                                              : R"({"requestId":3,"success":true})";
                    correct.store(true);
                } else if (step == 4 && refuse && request->stringAt("command") == "continue" &&
                           request->integerAt("requestId") == 4) {
                    response = R"({"requestId":4,"success":true})";
                } else
                    break;
                auto wire = frost::rpc::encodeFrame(response);
                if (step == 3 && refuse)
                    wire = frost::rpc::encodeFrame(R"({"event":"stopped","reason":"pause"})") + wire;
                else if (step == 3 && !preAdmission)
                    wire += frost::rpc::encodeFrame(R"({"event":"playStopped","operationId":41})") +
                            frost::rpc::encodeFrame(R"({"event":"playStopped","operationId":42})") +
                            frost::rpc::encodeFrame(R"({"event":"playStopped","operationId":42})");
                else if (step == 4 && refuse)
                    wire += frost::rpc::encodeFrame(R"({"event":"continued"})");
                std::size_t offset = 0;
                while (offset < wire.size()) {
                    const int sent = static_cast<int>(
                        send(peer, wire.data() + offset, static_cast<SocketLength>(wire.size() - offset), 0));
                    if (sent <= 0)
                        break;
                    offset += static_cast<std::size_t>(sent);
                }
                if (offset != wire.size())
                    break;
                ++step;
            }
        }
        closeSocket(peer);
    });
    auto result = frost::debug::connectLuaDebugPlayEndpoint(ntohs(address.sin_port), "0123456789abcdef0123456789abcdef",
                                                            std::chrono::steady_clock::now() + 3s);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->connectionGeneration, 9U);
    EXPECT_EQ(result->operationId, 42U);
    EXPECT_EQ(result->nextWireRequest, 3);
    std::atomic<int> terminated{0};
    std::atomic<int> pausedEvents{0};
    std::atomic<int> continuedEvents{0};
    std::atomic<bool> closed{false};
    frost::debug::DapHandlers handlers;
    handlers.onEvent = [&](std::string_view event, const auto&) {
        if (event == "terminated")
            ++terminated;
        if (event == "stopped")
            ++pausedEvents;
        if (event == "continued")
            ++continuedEvents;
    };
    handlers.onClosed = [&](const auto&) { closed.store(true); };
    auto factory =
        frost::debug::luaDebugAdapterFactory(std::move(result->stream), result->nextWireRequest, result->operationId);
    auto client = factory(std::move(handlers));
    ASSERT_TRUE(client);
    auto stopped = std::make_shared<std::promise<std::pair<bool, bool>>>();
    auto stopResult = stopped->get_future();
    (*client)->request("frost.stopPlay", frost::rpc::Json::object(), [stopped](auto reply) {
        stopped->set_value({reply.has_value(), reply && reply->boolAt("completed").value_or(false)});
    });
    ASSERT_EQ(stopResult.wait_for(5s), std::future_status::ready);
    const auto outcome = stopResult.get();
    EXPECT_EQ(outcome.first, !refuse);
    EXPECT_EQ(outcome.second, preAdmission);
    if (refuse) {
        ASSERT_TRUE(frost::debug::test::waitUntil([&] { return pausedEvents.load() == 1; }));
        auto resumed = std::make_shared<std::promise<bool>>();
        auto resumeResult = resumed->get_future();
        (*client)->request("continue", frost::rpc::Json::object(),
                           [resumed](auto reply) { resumed->set_value(reply.has_value()); });
        ASSERT_EQ(resumeResult.wait_for(5s), std::future_status::ready);
        EXPECT_TRUE(resumeResult.get());
    }
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return closed.load(); }));
    EXPECT_EQ(terminated.load(), refuse ? 0 : 1);
    EXPECT_EQ(pausedEvents.load(), refuse ? 1 : 0);
    EXPECT_EQ(continuedEvents.load(), refuse ? 1 : 0);
    EXPECT_TRUE(correct.load());
    (*client)->close();
    client->reset();
    server.join();
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}

TEST(LuaDebugEndpoint, ReservesAndArmsExactPlayBeforeHandingOffWireIds) { exerciseReservedPlayStop(false); }

TEST(LuaDebugEndpoint, PreAdmissionStopTerminatesWithoutAnEngineEvent) { exerciseReservedPlayStop(true); }

TEST(LuaDebugEndpoint, RefusedPlayStopAllowsContinueWithoutTermination) { exerciseReservedPlayStop(false, true); }

TEST(LuaDebugEndpoint, MapsReorderedWireRepliesToTheirDapRequests) {
#ifdef _WIN32
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listener, kInvalid);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    std::atomic<std::int64_t> pauseWire{0};
    std::atomic<std::int64_t> continueWire{0};
    std::jthread server([&] {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener, &ready);
        timeval timeout{5, 0};
        if (select(static_cast<int>(listener + 1), &ready, nullptr, nullptr, &timeout) <= 0)
            return;
        const Socket peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalid)
            return;
        const auto sendAll = [peer](std::string_view wire) {
            while (!wire.empty()) {
                const int written =
                    static_cast<int>(send(peer, wire.data(), static_cast<SocketLength>(wire.size()), 0));
                if (written <= 0)
                    return false;
                wire.remove_prefix(static_cast<std::size_t>(written));
            }
            return true;
        };
        frost::rpc::FrameDecoder decoder(2048);
        std::array<char, 1024> buffer{};
        bool authenticated = false;
        while (continueWire.load() == 0) {
            FD_ZERO(&ready);
            FD_SET(peer, &ready);
            timeval readTimeout{5, 0};
            if (select(static_cast<int>(peer + 1), &ready, nullptr, nullptr, &readTimeout) <= 0)
                break;
            const int count = static_cast<int>(recv(peer, buffer.data(), static_cast<SocketLength>(buffer.size()), 0));
            if (count <= 0)
                break;
            decoder.feed(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
            while (auto frame = decoder.next()) {
                if (!*frame)
                    break;
                const auto message = frost::rpc::parseJson(**frame);
                if (!message)
                    break;
                if (!authenticated) {
                    authenticated = true;
                    const auto reply = frost::rpc::encodeFrame(R"({"authenticated":true})");
                    if (!sendAll(reply))
                        break;
                } else if (message->stringAt("command") == "pause") {
                    pauseWire.store(message->integerAt("requestId").value_or(0));
                } else if (message->stringAt("command") == "continue") {
                    continueWire.store(message->integerAt("requestId").value_or(0));
                }
            }
        }
        if (pauseWire.load() > 0 && continueWire.load() > 0) {
            const auto failure = frost::rpc::Json::object().set("requestId", continueWire.load()).set("success", false);
            const auto success = frost::rpc::Json::object().set("requestId", pauseWire.load()).set("success", true);
            const auto wire = frost::rpc::encodeFrame(failure.dump()) + frost::rpc::encodeFrame(success.dump());
            static_cast<void>(sendAll(wire));
        }
        closeSocket(peer);
    });
    auto factory =
        frost::debug::luaDebugAdapterFactory(ntohs(address.sin_port), "0123456789abcdef0123456789abcdef", 3s);
    auto client = factory({});
    ASSERT_TRUE(client);
    for (int i = 0; i < 3; ++i)
        (*client)->request("threads", frost::rpc::Json::object(), [](auto) {});
    auto paused = std::make_shared<std::promise<bool>>();
    auto continued = std::make_shared<std::promise<bool>>();
    auto pauseResult = paused->get_future();
    auto continueResult = continued->get_future();
    const auto pauseDap = (*client)->request("pause", frost::rpc::Json::object(),
                                             [paused](auto result) { paused->set_value(result.has_value()); });
    const auto continueDap = (*client)->request("continue", frost::rpc::Json::object(),
                                                [continued](auto result) { continued->set_value(result.has_value()); });
    EXPECT_GT(pauseDap, 2);
    EXPECT_GT(continueDap, pauseDap);
    ASSERT_EQ(pauseResult.wait_for(5s), std::future_status::ready);
    ASSERT_EQ(continueResult.wait_for(5s), std::future_status::ready);
    EXPECT_TRUE(pauseResult.get());
    EXPECT_FALSE(continueResult.get());
    EXPECT_EQ(pauseWire.load(), 2);
    EXPECT_EQ(continueWire.load(), 3);
    (*client)->close();
    client->reset();
    server.join();
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}

TEST(LuaDebugEndpoint, AuthenticatesAndLeavesTheNextFrameUnread) {
#ifdef _WIN32
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listener, kInvalid);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    std::string received;
    std::jthread server([&] {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval timeout{3, 0};
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0)
            return;
        const Socket peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalid)
            return;
        frost::rpc::FrameDecoder decoder(2048);
        std::array<char, 2048> bytes{};
        while (received.empty()) {
            const int count = static_cast<int>(recv(peer, bytes.data(), static_cast<SocketLength>(bytes.size()), 0));
            if (count <= 0)
                break;
            decoder.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            if (auto frame = decoder.next(); frame && *frame)
                received = **frame;
        }
        if (!received.empty()) {
            const std::string reply =
                frost::rpc::encodeFrame(R"({"authenticated":true})") + frost::rpc::encodeFrame(R"({"event":"ready"})");
            std::size_t sent = 0;
            while (sent < reply.size()) {
                const int count = static_cast<int>(
                    send(peer, reply.data() + sent, static_cast<SocketLength>(reply.size() - sent), 0));
                if (count <= 0)
                    break;
                sent += static_cast<std::size_t>(count);
            }
        }
        closeSocket(peer);
    });
    auto stream = frost::debug::connectLuaDebugEndpoint(ntohs(address.sin_port), "0123456789abcdef0123456789abcdef",
                                                        std::chrono::steady_clock::now() + 3s);
    ASSERT_TRUE(stream);
    frost::rpc::FrameDecoder decoder(1024);
    std::array<char, 128> bytes{};
    std::optional<frost::Result<std::string>> frame;
    while (!frame) {
        auto count = (*stream)->read(bytes);
        ASSERT_TRUE(count);
        ASSERT_GT(*count, 0U);
        decoder.feed(std::string_view(bytes.data(), *count));
        frame = decoder.next();
    }
    ASSERT_TRUE(*frame);
    EXPECT_EQ(**frame, R"({"event":"ready"})");
    server.join();
    auto request = frost::rpc::parseJson(received);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->stringAt("token"), "0123456789abcdef0123456789abcdef");
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}

TEST(LuaDebugEndpoint, AdapterTranslatesSessionControlOverTheSocket) {
#ifdef _WIN32
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listener, kInvalid);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    std::atomic<bool> continued{false};
    std::atomic<std::int64_t> pauseWireId{0};
    std::jthread server([&] {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval timeout{3, 0};
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0)
            return;
        const Socket peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalid)
            return;
        frost::rpc::FrameDecoder decoder(2048);
        std::array<char, 1024> bytes{};
        bool authenticated = false;
        for (;;) {
            const int count = static_cast<int>(recv(peer, bytes.data(), static_cast<SocketLength>(bytes.size()), 0));
            if (count <= 0)
                break;
            decoder.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            while (auto frame = decoder.next()) {
                if (!*frame)
                    break;
                auto message = frost::rpc::parseJson(**frame);
                if (!message)
                    break;
                std::string response;
                std::string extraEvent;
                if (!authenticated) {
                    authenticated = message->stringAt("token") == "0123456789abcdef0123456789abcdef";
                    if (!authenticated)
                        break;
                    response = R"({"authenticated":true})";
                } else if (message->stringAt("command") == "pause") {
                    pauseWireId.store(message->integerAt("requestId").value_or(0));
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", true)
                                   .dump();
                    extraEvent = R"({"event":"stopped","reason":"pause"})";
                } else if (message->stringAt("command") == "stackTrace") {
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", false)
                                   .dump();
                } else if (message->stringAt("command") == "continue") {
                    continued.store(true);
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", true)
                                   .dump();
                    extraEvent = R"({"event":"continued"})";
                }
                const std::string wire = frost::rpc::encodeFrame(response) +
                                         (extraEvent.empty() ? std::string() : frost::rpc::encodeFrame(extraEvent));
                std::size_t sent = 0;
                while (sent < wire.size()) {
                    const int written = static_cast<int>(
                        send(peer, wire.data() + sent, static_cast<SocketLength>(wire.size() - sent), 0));
                    if (written <= 0)
                        break;
                    sent += static_cast<std::size_t>(written);
                }
            }
        }
        closeSocket(peer);
    });
    auto factory =
        frost::debug::luaDebugAdapterFactory(ntohs(address.sin_port), "0123456789abcdef0123456789abcdef", 3s);
    auto session = frost::debug::DebugSession::start(std::move(factory), frost::rpc::Json::object(), {}, {}, false,
                                                     frost::debug::DebugTargetMode::Attach);
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Running; }));
    ASSERT_TRUE(session->pause());
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Stopped; }));
    EXPECT_EQ(pauseWireId.load(), 2);
    auto rejected = std::make_shared<std::promise<bool>>();
    auto rejectedResult = rejected->get_future();
    session->evaluate("1 + 1", {}, "watch", [rejected](auto result) { rejected->set_value(!result); });
    ASSERT_EQ(rejectedResult.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(rejectedResult.get());
    ASSERT_TRUE(session->continueRun());
    ASSERT_TRUE(frost::debug::test::waitUntil(
        [&] { return continued.load() && session->state() == frost::debug::DebugState::Running; }));
    session->stop();
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Ended; }));
    session.reset();
    server.join();
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}

TEST(LuaDebugEndpoint, PrearmedPauseIsDeliveredAsAnEntryStop) {
#ifdef _WIN32
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listener, kInvalid);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    std::atomic<bool> continued{false};
    std::atomic<bool> latePauseSeen{false};
    std::atomic<std::int64_t> prearmedPauseId{0};
    std::jthread server([&] {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval timeout{3, 0};
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0)
            return;
        const Socket peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalid)
            return;
        frost::rpc::FrameDecoder decoder(2048);
        std::array<char, 1024> bytes{};
        bool authenticated = false;
        for (;;) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(peer, &ready);
            timeval readTimeout{5, 0};
            if (select(static_cast<int>(peer + 1), &ready, nullptr, nullptr, &readTimeout) <= 0)
                break;
            const int count = static_cast<int>(recv(peer, bytes.data(), static_cast<SocketLength>(bytes.size()), 0));
            if (count <= 0)
                break;
            decoder.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            while (auto frame = decoder.next()) {
                if (!*frame)
                    break;
                auto message = frost::rpc::parseJson(**frame);
                if (!message)
                    break;
                std::string response;
                std::string extraEvent;
                if (!authenticated) {
                    authenticated = message->stringAt("token") == "0123456789abcdef0123456789abcdef";
                    if (!authenticated)
                        break;
                    response = R"({"authenticated":true})";
                } else if (message->stringAt("command") == "armPause") {
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", true)
                                   .dump();
                } else if (message->stringAt("command") == "pause") {
                    prearmedPauseId.store(message->integerAt("requestId").value_or(0));
                    const std::string stopped = frost::rpc::encodeFrame(R"({"event":"stopped","reason":"entry"})");
                    std::size_t offset = 0;
                    while (offset < stopped.size()) {
                        const int written = static_cast<int>(
                            send(peer, stopped.data() + offset, static_cast<SocketLength>(stopped.size() - offset), 0));
                        if (written <= 0)
                            break;
                        offset += static_cast<std::size_t>(written);
                    }
                    latePauseSeen.store(true);
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", false)
                                   .dump();
                } else if (message->stringAt("command") == "stackTrace") {
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", false)
                                   .dump();
                } else if (message->stringAt("command") == "continue") {
                    continued.store(true);
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", true)
                                   .dump();
                    extraEvent = R"({"event":"continued"})";
                }
                const std::string wire = frost::rpc::encodeFrame(response) +
                                         (extraEvent.empty() ? std::string() : frost::rpc::encodeFrame(extraEvent));
                std::size_t sent = 0;
                while (sent < wire.size()) {
                    const int written = static_cast<int>(
                        send(peer, wire.data() + sent, static_cast<SocketLength>(wire.size() - sent), 0));
                    if (written <= 0)
                        break;
                    sent += static_cast<std::size_t>(written);
                }
            }
        }
        closeSocket(peer);
    });
    auto stream = frost::debug::connectLuaDebugEndpoint(ntohs(address.sin_port), "0123456789abcdef0123456789abcdef",
                                                        std::chrono::steady_clock::now() + 3s, true);
    ASSERT_TRUE(stream);
    auto factory = frost::debug::luaDebugAdapterFactory(std::move(*stream));
    auto duplicate = factory;
    auto session = frost::debug::DebugSession::start(std::move(factory), frost::rpc::Json::object(), {}, {}, false,
                                                     frost::debug::DebugTargetMode::Attach);
    EXPECT_FALSE(duplicate({}));
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Running; }));
    ASSERT_TRUE(session->pause());
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Stopped; }));
    EXPECT_EQ(prearmedPauseId.load(), 2);
    ASSERT_TRUE(session->stopInfo().has_value());
    EXPECT_EQ(session->stopInfo()->reason, "entry");
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return latePauseSeen.load(); }));
    auto rejected = std::make_shared<std::promise<bool>>();
    auto rejectedResult = rejected->get_future();
    session->evaluate("1 + 1", {}, "watch", [rejected](auto result) { rejected->set_value(!result); });
    ASSERT_EQ(rejectedResult.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(rejectedResult.get());
    ASSERT_TRUE(session->continueRun());
    ASSERT_TRUE(frost::debug::test::waitUntil(
        [&] { return continued.load() && session->state() == frost::debug::DebugState::Running; }));
    session->stop();
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Ended; }));
    session.reset();
    server.join();
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}

TEST(LuaDebugEndpoint, ImmediatePrearmedStopSurvivesHandshakeAndSessionStartup) {
#ifdef _WIN32
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listener, kInvalid);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    std::atomic<bool> continued{false};
    std::jthread server([&] {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval timeout{3, 0};
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0)
            return;
        const Socket peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalid)
            return;
        frost::rpc::FrameDecoder decoder(2048);
        std::array<char, 1024> bytes{};
        bool authenticated = false;
        for (;;) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(peer, &ready);
            timeval readTimeout{5, 0};
            if (select(static_cast<int>(peer + 1), &ready, nullptr, nullptr, &readTimeout) <= 0)
                break;
            const int count = static_cast<int>(recv(peer, bytes.data(), static_cast<SocketLength>(bytes.size()), 0));
            if (count <= 0)
                break;
            decoder.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            while (auto frame = decoder.next()) {
                if (!*frame)
                    break;
                auto message = frost::rpc::parseJson(**frame);
                if (!message)
                    break;
                std::string response;
                std::string extraEvent;
                if (!authenticated) {
                    authenticated = message->stringAt("token") == "0123456789abcdef0123456789abcdef";
                    if (!authenticated)
                        break;
                    response = R"({"authenticated":true})";
                } else if (message->stringAt("command") == "armPause") {
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", true)
                                   .dump();
                    extraEvent = R"({"event":"stopped","reason":"entry"})";
                } else if (message->stringAt("command") == "stackTrace") {
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", false)
                                   .dump();
                } else if (message->stringAt("command") == "continue") {
                    continued.store(true);
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", true)
                                   .dump();
                    extraEvent = R"({"event":"continued"})";
                }
                const std::string wire = frost::rpc::encodeFrame(response) +
                                         (extraEvent.empty() ? std::string() : frost::rpc::encodeFrame(extraEvent));
                std::size_t sent = 0;
                while (sent < wire.size()) {
                    const int written = static_cast<int>(
                        send(peer, wire.data() + sent, static_cast<SocketLength>(wire.size() - sent), 0));
                    if (written <= 0)
                        break;
                    sent += static_cast<std::size_t>(written);
                }
            }
        }
        closeSocket(peer);
    });
    auto stream = frost::debug::connectLuaDebugEndpoint(ntohs(address.sin_port), "0123456789abcdef0123456789abcdef",
                                                        std::chrono::steady_clock::now() + 3s, true);
    ASSERT_TRUE(stream);
    auto factory = frost::debug::luaDebugAdapterFactory(std::move(*stream));
    auto duplicate = factory;
    auto session = frost::debug::DebugSession::start(std::move(factory), frost::rpc::Json::object(), {}, {}, false,
                                                     frost::debug::DebugTargetMode::Attach);
    EXPECT_FALSE(duplicate({}));
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Stopped; }));
    ASSERT_TRUE(session->stopInfo().has_value());
    EXPECT_EQ(session->stopInfo()->reason, "entry");
    ASSERT_TRUE(session->continueRun());
    ASSERT_TRUE(frost::debug::test::waitUntil(
        [&] { return continued.load() && session->state() == frost::debug::DebugState::Running; }));
    session->stop();
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Ended; }));
    session.reset();
    server.join();
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}

TEST(LuaDebugEndpoint, DiscardingUnusedPreconnectedFactoryClosesThePeer) {
#ifdef _WIN32
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listener, kInvalid);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    std::atomic<bool> disconnected{false};
    std::jthread server([&] {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval timeout{3, 0};
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0)
            return;
        const Socket peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalid)
            return;
        frost::rpc::FrameDecoder decoder(2048);
        std::array<char, 1024> bytes{};
        while (true) {
            FD_ZERO(&set);
            FD_SET(peer, &set);
            timeout = {5, 0};
            if (select(static_cast<int>(peer + 1), &set, nullptr, nullptr, &timeout) <= 0)
                break;
            const int count = static_cast<int>(recv(peer, bytes.data(), static_cast<SocketLength>(bytes.size()), 0));
            if (count <= 0) {
                disconnected.store(true);
                break;
            }
            decoder.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            if (auto frame = decoder.next(); frame && *frame) {
                const std::string reply = frost::rpc::encodeFrame(R"({"authenticated":true})");
                std::size_t sent = 0;
                while (sent < reply.size()) {
                    const int written = static_cast<int>(
                        send(peer, reply.data() + sent, static_cast<SocketLength>(reply.size() - sent), 0));
                    if (written <= 0)
                        break;
                    sent += static_cast<std::size_t>(written);
                }
            }
        }
        closeSocket(peer);
    });
    auto stream = frost::debug::connectLuaDebugEndpoint(ntohs(address.sin_port), "0123456789abcdef0123456789abcdef",
                                                        std::chrono::steady_clock::now() + 3s);
    ASSERT_TRUE(stream);
    {
        auto factory = frost::debug::luaDebugAdapterFactory(std::move(*stream));
        auto copy = factory;
        factory = {};
        EXPECT_TRUE(static_cast<bool>(copy));
        EXPECT_FALSE(disconnected.load());
    }
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return disconnected.load(); }));
    server.join();
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}

TEST(LuaDebugEndpoint, MalformedEngineFrameDisconnectsWhileStoppedEvenWhenSessionIsRetained) {
#ifdef _WIN32
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listener, kInvalid);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    std::atomic<bool> inject{false};
    std::atomic<bool> disconnected{false};
    std::jthread server([&] {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval timeout{3, 0};
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0)
            return;
        const Socket peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalid)
            return;
        frost::rpc::FrameDecoder decoder(2048);
        std::array<char, 1024> bytes{};
        bool authenticated = false;
        bool paused = false;
        while (!paused) {
            const int count = static_cast<int>(recv(peer, bytes.data(), static_cast<SocketLength>(bytes.size()), 0));
            if (count <= 0)
                break;
            decoder.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            while (auto frame = decoder.next()) {
                if (!*frame)
                    break;
                auto message = frost::rpc::parseJson(**frame);
                if (!message)
                    break;
                std::string response;
                std::string extraEvent;
                if (!authenticated) {
                    authenticated = message->stringAt("token") == "0123456789abcdef0123456789abcdef";
                    response = R"({"authenticated":true})";
                } else if (message->stringAt("command") == "pause") {
                    paused = true;
                    response = frost::rpc::Json::object()
                                   .set("requestId", *message->integerAt("requestId"))
                                   .set("success", true)
                                   .dump();
                    extraEvent = R"({"event":"stopped","reason":"pause"})";
                }
                const std::string wire = frost::rpc::encodeFrame(response) +
                                         (extraEvent.empty() ? std::string() : frost::rpc::encodeFrame(extraEvent));
                std::size_t sent = 0;
                while (sent < wire.size()) {
                    const int written = static_cast<int>(
                        send(peer, wire.data() + sent, static_cast<SocketLength>(wire.size() - sent), 0));
                    if (written <= 0)
                        break;
                    sent += static_cast<std::size_t>(written);
                }
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (paused && !inject.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (paused && inject.load()) {
            const std::string invalid = frost::rpc::encodeFrame("not-json");
            std::size_t sent = 0;
            while (sent < invalid.size()) {
                const int written = static_cast<int>(
                    send(peer, invalid.data() + sent, static_cast<SocketLength>(invalid.size() - sent), 0));
                if (written <= 0)
                    break;
                sent += static_cast<std::size_t>(written);
            }
            const auto closeDeadline = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < closeDeadline) {
                FD_ZERO(&set);
                FD_SET(peer, &set);
                timeout = timeval{0, 50000};
                if (select(static_cast<int>(peer + 1), &set, nullptr, nullptr, &timeout) <= 0)
                    continue;
                if (recv(peer, bytes.data(), static_cast<SocketLength>(bytes.size()), 0) <= 0) {
                    disconnected.store(true);
                    break;
                }
                // A stack request can already be queued when malformed input arrives; drain it before EOF.
            }
        }
        closeSocket(peer);
    });
    auto factory =
        frost::debug::luaDebugAdapterFactory(ntohs(address.sin_port), "0123456789abcdef0123456789abcdef", 3s);
    auto session = frost::debug::DebugSession::start(std::move(factory), frost::rpc::Json::object(), {}, {}, false,
                                                     frost::debug::DebugTargetMode::Attach);
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Running; }));
    ASSERT_TRUE(session->pause());
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Stopped; }));
    inject.store(true);
    ASSERT_TRUE(frost::debug::test::waitUntil([&] { return disconnected.load(); }));
    EXPECT_TRUE(frost::debug::test::waitUntil([&] { return session->state() == frost::debug::DebugState::Ended; }));
    server.join();
    session.reset();
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}
} // namespace
