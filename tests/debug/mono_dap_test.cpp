// SPDX-License-Identifier: Apache-2.0
// Exercise Mono adapter lifecycle, control, source mapping and failures.
#include "frost/debug/mono_dap.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>

#include <unistd.h>
#endif
using namespace std::chrono_literals;
namespace frost::debug::mono {
namespace {

#ifdef _WIN32
using TestSocket = SOCKET;
constexpr TestSocket badSocket = INVALID_SOCKET;
void closeTestSocket(TestSocket s) { closesocket(s); }
#else
using TestSocket = int;
constexpr TestSocket badSocket = -1;
void closeTestSocket(TestSocket s) { close(s); }
#endif
void put32(std::vector<std::uint8_t>& data, std::uint32_t n) {
    for (int shift = 24; shift >= 0; shift -= 8)
        data.push_back(static_cast<std::uint8_t>(n >> shift));
}
std::uint32_t get32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
bool receiveAll(TestSocket s, std::uint8_t* data, std::size_t n) {
    while (n) {
#ifdef _WIN32
        const auto length = static_cast<int>(std::min(n, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#else
        const auto length = n;
#endif
        auto count = recv(s, reinterpret_cast<char*>(data), length, 0);
        if (count <= 0)
            return false;
        data += count;
        n -= static_cast<std::size_t>(count);
    }
    return true;
}
bool sendAll(TestSocket s, const std::vector<std::uint8_t>& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
#ifdef _WIN32
        const auto length =
            static_cast<int>(std::min(data.size() - offset, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#else
        const auto length = data.size() - offset;
#endif
        auto count = send(s, reinterpret_cast<const char*>(data.data() + offset), length, 0);
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}
// A real loopback peer with deterministic Mono responses. It never contacts an Editor.
class ScriptedMono {
public:
    explicit ScriptedMono(bool badImage = false, bool failResume = false, bool failSecondClear = false,
                          bool stopAfterResume = false)
        : badImage(badImage), failResume(failResume), failSecondClear(failSecondClear),
          stopAfterResume(stopAfterResume) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return;
        winsock = true;
#endif
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == badSocket)
            return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 1))
            return;
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size))
            return;
        port = ntohs(address.sin_port);
        server = std::jthread([this] { run(); });
    }
    ~ScriptedMono() {
        if (listener != badSocket)
            closeTestSocket(listener);
        if (server.joinable())
            server.join();
#ifdef _WIN32
        if (winsock)
            WSACleanup();
#endif
    }
    AdapterTarget target() {
        auto module = std::make_shared<VerifiedModule>();
        module->assemblyPath = "/owned/a.dll";
        module->moduleId = "module";
        module->image = "image";
        module->pdb = "pdb";
        module->sources = {"/owned/a.cs"};
        module->points = {{0x06000001, 0, 10, 4}, {0x06000001, 0, 11, 8}};
        return {module, [this](MonoDeadline deadline, std::stop_token stop) {
                    return BinaryLoopback::connect(port, deadline, stop,
                                                   [this](auto, auto, auto, auto) -> Result<void> {
                                                       ++verifications;
                                                       return {};
                                                   });
                }};
    }
    std::atomic<int> verifications{0};
    std::atomic<bool> disposed{false};
    std::atomic<int> clears{0};
    std::uint16_t port = 0;

private:
    void run() {
        auto peer = accept(listener, nullptr, nullptr);
        if (peer == badSocket)
            return;
#ifdef _WIN32
        DWORD timeout = 15000;
        setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{15, 0};
        setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
        std::vector<std::uint8_t> handshake(13);
        if (!receiveAll(peer, handshake.data(), handshake.size()) || !sendAll(peer, handshake)) {
            closeTestSocket(peer);
            return;
        }
        for (;;) {
            std::vector<std::uint8_t> header(11);
            if (!receiveAll(peer, header.data(), header.size()))
                break;
            auto length = get32(header.data());
            if (length < 11 || length > 1024)
                break;
            std::vector<std::uint8_t> body(length - 11);
            if (!receiveAll(peer, body.data(), body.size()))
                break;
            const auto set = header[9], command = header[10];
            std::vector<std::uint8_t> payload;
            std::uint16_t error = 0;
            if (set == 1 && command == 1) {
                put32(payload, 1);
                payload.push_back('x');
                put32(payload, 2);
                put32(payload, 58);
            } else if (set == 1 && command == 8) {
                const std::vector<std::uint8_t> startup{0, 0, 0, 29, 0, 0, 0, 1, 0, 64, 100, 2, 0, 0, 0,
                                                        1, 0, 0, 0,  0, 0, 0, 0, 0, 1,  0,   0, 0, 1};
                if (!sendAll(peer, startup))
                    break;
            } else if (set == 20 && command == 1) {
                put32(payload, 1);
            } else if (set == 20 && command == 3) {
                put32(payload, 1);
                put32(payload, 2);
            } else if (set == 21 && (command == 1 || command == 8 || command == 10)) {
                std::string value = command == 1   ? "/owned/a.dll"
                                    : command == 8 ? (badImage ? "other" : "image")
                                                   : "pdb";
                put32(payload, static_cast<std::uint32_t>(value.size()));
                payload.insert(payload.end(), value.begin(), value.end());
            } else if (set == 21 && command == 12) {
                put32(payload, 3);
            } else if (set == 15 && command == 1) {
                put32(payload, nextBreakpoint++);
            } else if (set == 11 && command == 1) {
                put32(payload, 1);
                put32(payload, 5);
                put32(payload, 3);
                put32(payload, 4);
                payload.push_back(0);
            } else if (set == 15 && command == 2) {
                if (++clears == 2 && failSecondClear)
                    error = 1;
            } else if (set == 1 && command == 4 && failResume && !stopAfterResume) {
                error = 1;
            } else if (set == 1 && command == 6) {
                disposed = true;
            } else if (!(set == 1 && (command == 3 || command == 4))) {
                break;
            }
            std::vector<std::uint8_t> reply;
            put32(reply, static_cast<std::uint32_t>(11 + payload.size()));
            reply.insert(reply.end(), header.begin() + 4, header.begin() + 8);
            reply.insert(reply.end(), {0x80, static_cast<std::uint8_t>(error >> 8), static_cast<std::uint8_t>(error)});
            reply.insert(reply.end(), payload.begin(), payload.end());
            if (!sendAll(peer, reply))
                break;
            if (set == 1 && command == 4 && stopAfterResume) {
                const std::vector<std::uint8_t> event{0, 0, 0, 37, 0, 0, 0, 7, 0, 64, 100, 2, 0, 0, 0, 1, 10, 0, 0,
                                                      0, 9, 0, 0,  0, 1, 0, 0, 0, 3,  0,   0, 0, 0, 0, 0, 0,  4};
                if (!sendAll(peer, event))
                    break;
                stopAfterResume = false;
            }
            if (disposed)
                break;
        }
        closeTestSocket(peer);
    }
    TestSocket listener = badSocket;
    std::jthread server;
    bool badImage, failResume, failSecondClear, stopAfterResume;
    std::uint32_t nextBreakpoint = 9;
#ifdef _WIN32
    bool winsock = false;
#endif
};
Result<rpc::Json> requestSync(IDebugAdapterClient& client, std::string command, rpc::Json args = {}) {
    auto promise = std::make_shared<std::promise<Result<rpc::Json>>>();
    auto future = promise->get_future();
    (void)client.request(std::move(command), std::move(args),
                         [promise](Result<rpc::Json> result) { promise->set_value(std::move(result)); });
    if (future.wait_for(5s) != std::future_status::ready)
        return fail(ErrorCode::Timeout, "test request timed out");
    return future.get();
}
TEST(MonoDap, RunningBreakpointRejectionDoesNotEndSession) {
    ScriptedMono runtime;
    ASSERT_NE(runtime.port, 0);
    auto client = monoAdapterFactory(runtime.target())({});
    ASSERT_TRUE(client);
    ASSERT_TRUE(requestSync(**client, "attach"));
    ASSERT_TRUE(requestSync(**client, "continue"));
    const auto args = rpc::Json::object()
                          .set("source", rpc::Json::object().set("path", "/owned/a.cs"))
                          .set("breakpoints", rpc::Json::array({rpc::Json::object().set("line", 10)}));
    auto rejected = requestSync(**client, "setBreakpoints", args);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::Busy);
    EXPECT_FALSE((*client)->closed());
    EXPECT_TRUE(requestSync(**client, "disconnect"));
    EXPECT_TRUE(runtime.disposed);
}
TEST(MonoDap, HashMismatchDisposesSuspendedSession) {
    ScriptedMono runtime(true);
    ASSERT_NE(runtime.port, 0);
    std::promise<void> closed;
    auto future = closed.get_future();
    DapHandlers handlers;
    handlers.onClosed = [&](const auto&) { closed.set_value(); };
    auto client = monoAdapterFactory(runtime.target())(std::move(handlers));
    ASSERT_TRUE(client);
    EXPECT_FALSE(requestSync(**client, "attach"));
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_TRUE(runtime.disposed);
    EXPECT_TRUE((*client)->closed());
}
TEST(MonoDap, SuspendedIdleRenewsAndAllowsResume) {
    ScriptedMono runtime;
    ASSERT_NE(runtime.port, 0);
    auto client = monoAdapterFactory(runtime.target())({});
    ASSERT_TRUE(client);
    ASSERT_TRUE(requestSync(**client, "attach"));
    const auto before = runtime.verifications.load();
    std::this_thread::sleep_for(11s);
    EXPECT_GT(runtime.verifications.load(), before);
    EXPECT_TRUE(requestSync(**client, "continue"));
    EXPECT_TRUE(requestSync(**client, "disconnect"));
    EXPECT_TRUE(runtime.disposed);
}
TEST(MonoDap, PartialBreakpointClearFailureClosesSession) {
    ScriptedMono runtime(false, false, true);
    ASSERT_NE(runtime.port, 0);
    std::promise<void> closed;
    auto future = closed.get_future();
    DapHandlers handlers;
    handlers.onClosed = [&](const auto&) { closed.set_value(); };
    auto client = monoAdapterFactory(runtime.target())(std::move(handlers));
    ASSERT_TRUE(client);
    ASSERT_TRUE(requestSync(**client, "attach"));
    const auto args =
        rpc::Json::object()
            .set("source", rpc::Json::object().set("path", "/owned/a.cs"))
            .set("breakpoints",
                 rpc::Json::array({rpc::Json::object().set("line", 10), rpc::Json::object().set("line", 11)}));
    ASSERT_TRUE(requestSync(**client, "setBreakpoints", args));
    EXPECT_FALSE(requestSync(**client, "setBreakpoints", args));
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(runtime.clears, 2);
    EXPECT_FALSE(runtime.disposed);
    EXPECT_TRUE((*client)->closed());
}
TEST(MonoDap, StackMappingSurvivesBreakpointRemoval) {
    ScriptedMono runtime(false, false, false, true);
    ASSERT_NE(runtime.port, 0);
    std::promise<void> stopped;
    auto future = stopped.get_future();
    DapHandlers handlers;
    handlers.onEvent = [&](std::string_view name, const rpc::Json&) {
        if (name == "stopped")
            stopped.set_value();
    };
    auto client = monoAdapterFactory(runtime.target())(std::move(handlers));
    ASSERT_TRUE(client);
    ASSERT_TRUE(requestSync(**client, "attach"));
    auto args = rpc::Json::object()
                    .set("source", rpc::Json::object().set("path", "/owned/a.cs"))
                    .set("breakpoints", rpc::Json::array({rpc::Json::object().set("line", 10)}));
    ASSERT_TRUE(requestSync(**client, "setBreakpoints", args));
    ASSERT_TRUE(requestSync(**client, "continue"));
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    args.set("breakpoints", rpc::Json::array());
    ASSERT_TRUE(requestSync(**client, "setBreakpoints", args));
    auto stack = requestSync(**client, "stackTrace", rpc::Json::object().set("threadId", 1));
    ASSERT_TRUE(stack);
    const auto* frames = stack->find("stackFrames");
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames->size(), 1u);
    EXPECT_EQ(frames->items().front().integerAt("line"), 10);
    const auto* source = frames->items().front().find("source");
    ASSERT_TRUE(source);
    EXPECT_EQ(source->stringAt("path"), "/owned/a.cs");
    EXPECT_TRUE(requestSync(**client, "disconnect"));
}
TEST(MonoDap, FailedResumeRejectsPreviouslyPublishedFrame) {
    ScriptedMono runtime(false, true, false, true);
    ASSERT_NE(runtime.port, 0);
    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();
    std::promise<void> closed;
    auto closedFuture = closed.get_future();
    DapHandlers handlers;
    handlers.onEvent = [&](std::string_view name, const rpc::Json&) {
        if (name == "stopped")
            stopped.set_value();
    };
    handlers.onClosed = [&](const auto&) { closed.set_value(); };
    auto client = monoAdapterFactory(runtime.target())(std::move(handlers));
    ASSERT_TRUE(client);
    ASSERT_TRUE(requestSync(**client, "attach"));
    auto args = rpc::Json::object()
                    .set("source", rpc::Json::object().set("path", "/owned/a.cs"))
                    .set("breakpoints", rpc::Json::array({rpc::Json::object().set("line", 10)}));
    ASSERT_TRUE(requestSync(**client, "setBreakpoints", args));
    ASSERT_TRUE(requestSync(**client, "continue"));
    ASSERT_EQ(stoppedFuture.wait_for(5s), std::future_status::ready);
    auto stack = requestSync(**client, "stackTrace", rpc::Json::object().set("threadId", 1));
    ASSERT_TRUE(stack);
    const auto* frames = stack->find("stackFrames");
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames->size(), 1u);
    auto frame = frames->items().front().integerAt("id");
    ASSERT_TRUE(frame);
    EXPECT_FALSE(requestSync(**client, "continue"));
    ASSERT_EQ(closedFuture.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(requestSync(**client, "scopes", rpc::Json::object().set("frameId", *frame)));
}
TEST(MonoDap, FailedResumeClosesAndReportsUncertainCleanup) {
    ScriptedMono runtime(false, true);
    ASSERT_NE(runtime.port, 0);
    std::promise<Error> closed;
    auto future = closed.get_future();
    DapHandlers handlers;
    handlers.onClosed = [&](const Error& error) { closed.set_value(error); };
    auto client = monoAdapterFactory(runtime.target())(std::move(handlers));
    ASSERT_TRUE(client);
    ASSERT_TRUE(requestSync(**client, "attach"));
    EXPECT_FALSE(requestSync(**client, "continue"));
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_TRUE((*client)->closed());
    EXPECT_FALSE(runtime.disposed);
    EXPECT_NE(future.get().message().find("runtime state is unknown"), std::string::npos);
}
TEST(MonoDap, RejectsUnboundTarget) {
    auto factory = monoAdapterFactory({});
    EXPECT_FALSE(factory({}));
}
TEST(MonoDap, InitializesAndReportsConnectorFailureWithoutHanging) {
    auto module = std::make_shared<VerifiedModule>();
    module->assemblyPath = "/owned/a.dll";
    module->moduleId = "module";
    module->image = "image";
    module->pdb = "pdb";
    AdapterTarget target{module, [](MonoDeadline, std::stop_token) -> Result<std::unique_ptr<BinaryLoopback>> {
                             return fail(ErrorCode::PermissionDenied, "identity denied");
                         }};
    auto factory = monoAdapterFactory(std::move(target));
    std::promise<void> closed;
    auto closedFuture = closed.get_future();
    DapHandlers handlers;
    handlers.onClosed = [&](const auto&) { closed.set_value(); };
    auto client = factory(std::move(handlers));
    ASSERT_TRUE(client);
    std::promise<Result<rpc::Json>> initialized;
    auto init = initialized.get_future();
    ASSERT_NE((*client)->request("initialize", {},
                                 [&](Result<rpc::Json> result) { initialized.set_value(std::move(result)); }),
              0);
    ASSERT_EQ(init.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(init.get());
    std::promise<Result<rpc::Json>> attached;
    auto attach = attached.get_future();
    ASSERT_NE(
        (*client)->request("attach", {}, [&](Result<rpc::Json> result) { attached.set_value(std::move(result)); }), 0);
    ASSERT_EQ(attach.wait_for(3s), std::future_status::ready);
    EXPECT_FALSE(attach.get());
    ASSERT_EQ(closedFuture.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE((*client)->closed());
    std::promise<Result<rpc::Json>> resumed;
    auto resume = resumed.get_future();
    EXPECT_EQ(
        (*client)->request("continue", {}, [&](Result<rpc::Json> result) { resumed.set_value(std::move(result)); }), 0);
    ASSERT_EQ(resume.wait_for(3s), std::future_status::ready);
    EXPECT_FALSE(resume.get());
}
} // namespace
} // namespace frost::debug::mono
