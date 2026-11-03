// SPDX-License-Identifier: Apache-2.0
// Workbench integration tests for FrostEngine automation.
#include "frost/build/cmake_service.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/rpc/framing.hpp"
#include "frost/rpc/json.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "../../src/app/src/engine_request_helper.hpp"
#include "workbench_rig.hpp"
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
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
using namespace frost::app::test;
QPushButton* button(QWidget* panel, const QString& text) {
    for (auto* candidate : panel->findChildren<QPushButton*>())
        if (candidate->text() == text)
            return candidate;
    return nullptr;
}
bool hasStatus(QWidget* panel, const QString& text) {
    for (auto* label : panel->findChildren<QLabel*>())
        if (label->text().contains(text))
            return true;
    return false;
}
std::vector<frost::lang::Json> engineRequests(const frost::io::ScriptedProcessLauncher& launcher,
                                              std::string_view method) {
    std::vector<frost::lang::Json> requests;
    for (const auto& launch : launcher.launches()) {
        auto request = frost::lang::parseJson(launch.stdinBytes);
        if (request && request->stringAt("method") == method)
            requests.push_back(std::move(*request));
    }
    return requests;
}
#ifdef _WIN32
using TestSocket = SOCKET;
using TestSocketLength = int;
constexpr TestSocket kInvalidSocket = INVALID_SOCKET;
void closeTestSocket(TestSocket socket) { closesocket(socket); }
#else
using TestSocket = int;
using TestSocketLength = std::size_t;
constexpr TestSocket kInvalidSocket = -1;
void closeTestSocket(TestSocket socket) { close(socket); }
#endif

// Real byte stream with controlled arm/stop timing; every wait is bounded for failed assertions.
class LuaPeer {
public:
    LuaPeer() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return;
#endif
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kInvalidSocket)
            return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0)
            return;
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            return;
        port = ntohs(address.sin_port);
        worker = std::jthread([this] { serve(); });
    }
    ~LuaPeer() {
        stopping.store(true);
        releaseArm.store(true);
        if (worker.joinable())
            worker.join();
        if (listener != kInvalidSocket)
            closeTestSocket(listener);
#ifdef _WIN32
        WSACleanup();
#endif
    }
    std::uint16_t port = 0;
    std::atomic<bool> armSeen{false};
    std::atomic<bool> releaseArm{true};
    std::atomic<bool> sendStop{false};
    std::atomic<bool> continued{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> acceptStopAndDisconnect{false};
    std::atomic<bool> acceptAndCompleteStop{false};
    std::atomic<bool> disconnected{false};
    std::atomic<std::int64_t> firstWireRequest{0};

private:
    bool writeFrame(TestSocket peer, std::string_view body) {
        const std::string wire = frost::rpc::encodeFrame(body);
        std::size_t offset = 0;
        while (offset < wire.size()) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const int count = static_cast<int>(
                send(peer, wire.data() + offset, static_cast<TestSocketLength>(wire.size() - offset), flags));
            if (count <= 0)
                return false;
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }
    void serve() {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval timeout{5, 0};
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0)
            return;
        const TestSocket peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalidSocket)
            return;
        frost::rpc::FrameDecoder decoder(2048);
        std::array<char, 2048> bytes{};
        bool authenticated = false;
        bool stoppedSent = false;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!stopping.load() && std::chrono::steady_clock::now() < deadline) {
            if (sendStop.load() && !stoppedSent) {
                stoppedSent = writeFrame(peer, R"({"event":"stopped","reason":"entry"})");
                if (!stoppedSent)
                    break;
            }
            FD_ZERO(&set);
            FD_SET(peer, &set);
            timeout = {0, 50000};
            if (select(static_cast<int>(peer + 1), &set, nullptr, nullptr, &timeout) <= 0)
                continue;
            const int count =
                static_cast<int>(recv(peer, bytes.data(), static_cast<TestSocketLength>(bytes.size()), 0));
            if (count <= 0) {
                disconnected.store(true);
                break;
            }
            decoder.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            while (auto frame = decoder.next()) {
                if (!*frame)
                    break;
                const auto request = frost::rpc::parseJson(**frame);
                if (!request)
                    break;
                if (!authenticated) {
                    authenticated = request->stringAt("token") == "0123456789abcdef0123456789abcdef";
                    if (!authenticated || !writeFrame(peer, R"({"authenticated":true,"connectionGeneration":1})"))
                        break;
                } else if (request->stringAt("command") == "reservePlay") {
                    if (!writeFrame(peer, R"({"requestId":1,"success":true,"operationId":42})"))
                        break;
                } else if (request->stringAt("command") == "armPlay" && request->integerAt("operationId") == 42) {
                    armSeen.store(true);
                    while (!releaseArm.load() && !stopping.load() && std::chrono::steady_clock::now() < deadline) {
                        FD_ZERO(&set);
                        FD_SET(peer, &set);
                        timeout = {0, 50000};
                        if (select(static_cast<int>(peer + 1), &set, nullptr, nullptr, &timeout) > 0 &&
                            recv(peer, bytes.data(), 1, MSG_PEEK) == 0) {
                            disconnected.store(true);
                            closeTestSocket(peer);
                            return;
                        }
                    }
                    if (!writeFrame(peer, frost::rpc::Json::object().set("requestId", 2).set("success", true).dump()))
                        break;
                } else if (request->stringAt("command") == "stackTrace") {
                    const auto id = request->integerAt("requestId");
                    if (id) {
                        std::int64_t expected = 0;
                        firstWireRequest.compare_exchange_strong(expected, *id);
                        auto stackFrame = frost::rpc::Json::object()
                                              .set("id", 129)
                                              .set("name", "inner")
                                              .set("line", 2)
                                              .set("sourceId", "@frost-debug:1:1")
                                              .set("sourceName", "Assets/Scripts/space.lua");
                        auto body = frost::rpc::Json::object()
                                        .set("frames", frost::rpc::Json::array({stackFrame}))
                                        .set("stopGeneration", 1)
                                        .set("hasMore", false);
                        if (!writeFrame(peer, frost::rpc::Json::object()
                                                  .set("requestId", *id)
                                                  .set("success", true)
                                                  .set("body", std::move(body))
                                                  .dump()))
                            break;
                    }
                } else if (request->stringAt("command") == "source") {
                    const auto id = request->integerAt("requestId");
                    if (id && request->stringAt("sourceId") == "@frost-debug:1:1" &&
                        request->integerAt("stopGeneration") == 1) {
                        auto body = frost::rpc::Json::object().set("content", "local a = 1\nreturn a\n");
                        if (!writeFrame(peer, frost::rpc::Json::object()
                                                  .set("requestId", *id)
                                                  .set("success", true)
                                                  .set("body", std::move(body))
                                                  .dump()))
                            break;
                    }
                } else if (request->stringAt("command") == "stopPlay") {
                    const auto id = request->integerAt("requestId");
                    if (id && request->integerAt("operationId") == 42 &&
                        writeFrame(peer,
                                   frost::rpc::Json::object()
                                       .set("requestId", *id)
                                       .set("success", acceptStopAndDisconnect.load() || acceptAndCompleteStop.load())
                                       .dump())) {
                        stopRequested.store(true);
                        if (acceptStopAndDisconnect.load()) {
                            closeTestSocket(peer);
                            return;
                        }
                        if (acceptAndCompleteStop.load() &&
                            !writeFrame(peer, R"({"event":"playStopped","operationId":42})"))
                            break;
                    }
                } else if (request->stringAt("command") == "continue") {
                    const auto id = request->integerAt("requestId");
                    if (id &&
                        writeFrame(peer,
                                   frost::rpc::Json::object().set("requestId", *id).set("success", true).dump()) &&
                        writeFrame(peer, R"({"event":"continued"})"))
                        continued.store(true);
                }
            }
        }
        closeTestSocket(peer);
    }
    TestSocket listener = kInvalidSocket;
    std::atomic<bool> stopping{false};
    std::jthread worker;
};

frost::io::ScriptedProcess luaEndpointReply(std::string_view projectFile, std::uint16_t port, char endpointTag = 'a') {
    auto endpoint = frost::lang::Json::object()
                        .set("host", "127.0.0.1")
                        .set("port", port)
                        .set("token", "0123456789abcdef0123456789abcdef")
                        .set("endpointId", std::string(64, endpointTag))
                        .set("projectFile", projectFile);
    auto reply = frost::lang::Json::object()
                     .set("ok", true)
                     .set("value", frost::lang::Json::object()
                                       .set("result", std::move(endpoint))
                                       .set("locations", frost::lang::Json::object()));
    return frost::io::ScriptedProcess{.stdoutBytes = reply.dump()};
}

TEST(WorkbenchEngine, LuaActionAttachesBeforeStartingOnlyItsEndpointAndContinues) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    WorkDir dir("engine-lua-attach");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    const auto playReply = frost::lang::Json::object()
                               .set("ok", true)
                               .set("value", frost::lang::Json::object()
                                                 .set("result", frost::lang::Json::object().set("state", "Play"))
                                                 .set("locations", frost::lang::Json::object()));
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = playReply.dump()});
    action->click();
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.play").empty(); }, 10s));
    const auto requests = engineRequests(launcher, "script.debug.play");
    ASSERT_EQ(requests.size(), 1U);
    ASSERT_NE(requests.front().find("params"), nullptr);
    EXPECT_EQ(requests.front().find("params")->stringAt("endpointId"), std::string(64, 'a'));
    EXPECT_EQ(requests.front().find("params")->integerAt("connectionGeneration"), 1);
    EXPECT_EQ(requests.front().find("params")->integerAt("operationId"), 42);
    EXPECT_TRUE(engineRequests(launcher, "play.start").empty());
    ASSERT_TRUE(peer.armSeen.load());
    peer.sendStop.store(true);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->debugController()->paused(); }, 5s));
    auto* loaded = rig.window->findChild<QPlainTextEdit*>(QStringLiteral("debugLoadedSource"));
    ASSERT_NE(loaded, nullptr);
    ASSERT_TRUE(waitFor([&] { return loaded->toPlainText() == QStringLiteral("local a = 1\nreturn a\n"); }, 5s));
    EXPECT_EQ(peer.firstWireRequest.load(), 3);
    EXPECT_TRUE(loaded->isReadOnly());
    EXPECT_TRUE(rig.workbench->debugController()->resume());
    ASSERT_TRUE(waitFor([&] { return peer.continued.load(); }, 5s));
    rig.workbench->debugController()->stop();
    ASSERT_TRUE(waitFor([&] { return peer.disconnected.load(); }, 5s));
}

TEST(WorkbenchEngine, RefusedOwnedStopLeavesPausedSessionUsable) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    WorkDir dir("engine-lua-stop-refused");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* stop = button(panel, QStringLiteral("Stop"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(stop, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    const auto playReply = frost::lang::Json::object()
                               .set("ok", true)
                               .set("value", frost::lang::Json::object()
                                                 .set("result", frost::lang::Json::object().set("state", "Play"))
                                                 .set("locations", frost::lang::Json::object()));
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = playReply.dump()});
    action->click();
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.play").empty(); }, 10s));
    peer.sendStop.store(true);
    auto* controller = rig.workbench->debugController();
    ASSERT_TRUE(waitFor([&] { return controller->paused(); }, 5s));
    stop->click();
    ASSERT_TRUE(waitFor([&] { return peer.stopRequested.load(); }, 5s));
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("stop was refused")); }, 5s));
    EXPECT_TRUE(controller->paused());
    EXPECT_TRUE(controller->resume());
    ASSERT_TRUE(waitFor([&] { return peer.continued.load(); }, 5s));
}

TEST(WorkbenchEngine, LostConnectionAfterAcceptedStopDoesNotSendGenericStop) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    peer.acceptStopAndDisconnect.store(true);
    WorkDir dir("engine-lua-stop-unknown");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* stop = button(panel, QStringLiteral("Stop"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(stop, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    const auto playReply = frost::lang::Json::object()
                               .set("ok", true)
                               .set("value", frost::lang::Json::object()
                                                 .set("result", frost::lang::Json::object().set("state", "Play"))
                                                 .set("locations", frost::lang::Json::object()));
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = playReply.dump()});
    action->click();
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.play").empty(); }, 10s));
    peer.sendStop.store(true);
    auto* controller = rig.workbench->debugController();
    ASSERT_TRUE(waitFor([&] { return controller->paused(); }, 5s));
    stop->click();
    ASSERT_TRUE(waitFor([&] { return peer.stopRequested.load(); }, 5s));
    ASSERT_TRUE(waitFor([&] { return !controller->debugging(); }, 5s));
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("outcome is unknown")); }, 5s));
    stop->click();
    EXPECT_TRUE(hasStatus(panel, QStringLiteral("outcome is unknown")));
    action->click();
    EXPECT_TRUE(hasStatus(panel, QStringLiteral("Stop the current request or debug session first")));
    EXPECT_TRUE(engineRequests(launcher, "play.stop").empty());
    EXPECT_TRUE(engineRequests(launcher, "script.debug.stop").empty());
    EXPECT_EQ(engineRequests(launcher, "script.debug.start").size(), 1U);
}

TEST(WorkbenchEngine, CompletedStopWaitsForOutstandingPlayReplyBeforeEndpointCleanup) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    peer.acceptAndCompleteStop.store(true);
    WorkDir dir("engine-lua-stop-before-play-reply");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* stop = button(panel, QStringLiteral("Stop"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(stop, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    const auto reply = frost::lang::Json::object()
                           .set("ok", true)
                           .set("value", frost::lang::Json::object()
                                             .set("result", frost::lang::Json::object().set("state", "Edit"))
                                             .set("locations", frost::lang::Json::object()))
                           .dump();
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply, .chunkBytes = reply.size(), .chunkInterval = 2s});
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply});
    action->click();
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.play").empty(); }, 10s));
    peer.sendStop.store(true);
    auto* controller = rig.workbench->debugController();
    ASSERT_TRUE(waitFor([&] { return controller->paused(); }, 5s));
    ASSERT_TRUE(hasStatus(panel, QStringLiteral("Contacting FrostEngine")));
    stop->click();
    ASSERT_TRUE(waitFor([&] { return peer.stopRequested.load(); }, 5s));
    ASSERT_TRUE(waitFor([&] { return !controller->debugging(); }, 5s));
    EXPECT_TRUE(engineRequests(launcher, "script.debug.stop").empty());
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.stop").empty(); }, 10s));
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 10s));
    const auto cleanup = engineRequests(launcher, "script.debug.stop");
    ASSERT_EQ(cleanup.size(), 1U);
    EXPECT_EQ(cleanup.front().stringAt("root"), dir.join("project"));
    ASSERT_NE(cleanup.front().find("params"), nullptr);
    EXPECT_EQ(cleanup.front().find("params")->stringAt("endpointId"), std::string(64, 'a'));
    EXPECT_TRUE(engineRequests(launcher, "play.stop").empty());
}

TEST(WorkbenchEngine, CompletedStopWaitsForOtherAutomationBeforeEndpointCleanup) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    peer.acceptAndCompleteStop.store(true);
    WorkDir dir("engine-lua-stop-during-state");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* stop = button(panel, QStringLiteral("Stop"));
    auto* state = button(panel, QStringLiteral("State"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(stop, nullptr);
    ASSERT_NE(state, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    const auto reply = frost::lang::Json::object()
                           .set("ok", true)
                           .set("value", frost::lang::Json::object()
                                             .set("result", frost::lang::Json::object().set("state", "Play"))
                                             .set("locations", frost::lang::Json::object()))
                           .dump();
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply});
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply, .chunkBytes = reply.size(), .chunkInterval = 2s});
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply});
    action->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("FrostEngine play started")); }, 10s));
    peer.sendStop.store(true);
    auto* controller = rig.workbench->debugController();
    ASSERT_TRUE(waitFor([&] { return controller->paused(); }, 5s));
    state->click();
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "play.state").empty(); }, 5s));
    ASSERT_TRUE(hasStatus(panel, QStringLiteral("Contacting FrostEngine")));
    stop->click();
    ASSERT_TRUE(waitFor([&] { return peer.stopRequested.load(); }, 5s));
    ASSERT_TRUE(waitFor([&] { return !controller->debugging(); }, 5s));
    EXPECT_TRUE(engineRequests(launcher, "script.debug.stop").empty());
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.stop").empty(); }, 10s));
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 10s));
    const auto cleanup = engineRequests(launcher, "script.debug.stop");
    ASSERT_EQ(cleanup.size(), 1U);
    EXPECT_EQ(cleanup.front().stringAt("root"), dir.join("project"));
    ASSERT_NE(cleanup.front().find("params"), nullptr);
    EXPECT_EQ(cleanup.front().find("params")->stringAt("endpointId"), std::string(64, 'a'));
    EXPECT_TRUE(engineRequests(launcher, "play.stop").empty());
}

TEST(WorkbenchEngine, CleanupAdmissionFailureRetainsExactEndpointForRetry) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    peer.acceptAndCompleteStop.store(true);
    WorkDir dir("engine-lua-stop-cleanup-retry");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* stop = button(panel, QStringLiteral("Stop"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(stop, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    const auto reply = frost::lang::Json::object()
                           .set("ok", true)
                           .set("value", frost::lang::Json::object()
                                             .set("result", frost::lang::Json::object().set("state", "Play"))
                                             .set("locations", frost::lang::Json::object()))
                           .dump();
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply});
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply});
    action->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("FrostEngine play started")); }, 10s));
    peer.sendStop.store(true);
    auto* controller = rig.workbench->debugController();
    ASSERT_TRUE(waitFor([&] { return controller->paused(); }, 5s));
    struct EnvironmentRestore {
        QByteArray old = qgetenv("FROST_ENGINE_AUTOMATION_DIR");
        bool wasSet = qEnvironmentVariableIsSet("FROST_ENGINE_AUTOMATION_DIR");
        ~EnvironmentRestore() {
            if (wasSet)
                qputenv("FROST_ENGINE_AUTOMATION_DIR", old);
            else
                qunsetenv("FROST_ENGINE_AUTOMATION_DIR");
        }
    } restore;
    qputenv("FROST_ENGINE_AUTOMATION_DIR", "relative-path");
    stop->click();
    ASSERT_TRUE(waitFor([&] { return peer.stopRequested.load(); }, 5s));
    ASSERT_TRUE(waitFor([&] { return !controller->debugging(); }, 5s));
    EXPECT_TRUE(engineRequests(launcher, "script.debug.stop").empty());
    ASSERT_TRUE(waitFor(
        [&] { return hasStatus(panel, QStringLiteral("Cannot locate the project or automation directory")); }, 5s));
    if (restore.wasSet)
        qputenv("FROST_ENGINE_AUTOMATION_DIR", restore.old);
    else
        qunsetenv("FROST_ENGINE_AUTOMATION_DIR");
    stop->click();
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.stop").empty(); }, 10s));
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 10s));
    const auto cleanup = engineRequests(launcher, "script.debug.stop");
    ASSERT_EQ(cleanup.size(), 1U);
    EXPECT_EQ(cleanup.front().stringAt("root"), dir.join("project"));
    ASSERT_NE(cleanup.front().find("params"), nullptr);
    EXPECT_EQ(cleanup.front().find("params")->stringAt("endpointId"), std::string(64, 'a'));
    EXPECT_TRUE(engineRequests(launcher, "play.stop").empty());
}

TEST(WorkbenchEngine, RevokingTrustDisconnectsTheOwnedPausedLuaSession) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    WorkDir dir("engine-lua-trust-revoked");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    launcher.expect(
        frost::io::ScriptedProcess{.stdoutBytes = R"({"ok":true,"value":{"result":{"state":"Play"},"locations":{}}})"});
    action->click();
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.play").empty(); }, 10s));
    peer.sendStop.store(true);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->debugController()->paused(); }, 5s));
    ASSERT_TRUE(rig.trust->untrust(dir.join("project")));
    ASSERT_TRUE(waitFor([&] { return peer.disconnected.load(); }, 5s));
}

TEST(WorkbenchEngine, DefiniteLuaPlayFailureDisconnectsTheOwnedSession) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    WorkDir dir("engine-lua-play-failure");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    launcher.expect(frost::io::ScriptedProcess{
        .stdoutBytes = R"({"ok":false,"code":4,"message":"Lua debug endpoint was not admitted"})"});
    launcher.expect(
        frost::io::ScriptedProcess{.stdoutBytes = R"({"ok":true,"value":{"result":{"stopped":true},"locations":{}}})"});
    action->click();
    ASSERT_TRUE(waitFor([&] { return peer.disconnected.load(); }, 5s));
    EXPECT_EQ(engineRequests(launcher, "script.debug.play").size(), 1U);
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.stop").empty(); }, 5s));
}

TEST(WorkbenchEngine, UncertainLuaPlayFailureKeepsTheSessionAvailable) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    WorkDir dir("engine-lua-play-uncertain");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = R"({"ok":false,"code":5,"message":"connection lost"})"});
    action->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("outcome is unknown")); }, 5s));
    EXPECT_TRUE(rig.workbench->debugController()->debugging());
    EXPECT_FALSE(peer.disconnected.load());
    rig.workbench->debugController()->stop();
    ASSERT_TRUE(waitFor([&] { return peer.disconnected.load(); }, 5s));
}

TEST(WorkbenchEngine, CancellingSynchronouslyDuringLuaAttachCannotStartPlay) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    WorkDir dir("engine-lua-cancel-attach");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* cancel = button(panel, QStringLiteral("Cancel Request"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(cancel, nullptr);
    bool cancelled = false;
    QObject::connect(rig.workbench->debugController(), &frost::ui::DebugController::stateChanged, panel, [&] {
        if (!cancelled && rig.workbench->debugController()->state() == frost::debug::DebugState::Starting) {
            cancelled = true;
            cancel->click();
        }
    });
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    launcher.expect(
        frost::io::ScriptedProcess{.stdoutBytes = R"({"ok":true,"value":{"result":{"stopped":true},"locations":{}}})"});
    action->click();
    ASSERT_TRUE(waitFor([&] { return cancelled; }, 5s));
    ASSERT_TRUE(waitFor([&] { return peer.disconnected.load(); }, 5s));
    EXPECT_TRUE(engineRequests(launcher, "script.debug.play").empty());
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.stop").empty(); }, 5s));
}

TEST(WorkbenchEngine, CancellingLuaHandshakeClosesPeerAndNeverStartsPlay) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    peer.releaseArm.store(false);
    WorkDir dir("engine-lua-cancel");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* cancel = button(panel, QStringLiteral("Cancel Request"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(cancel, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    launcher.expect(
        frost::io::ScriptedProcess{.stdoutBytes = R"({"ok":true,"value":{"result":{"stopped":true},"locations":{}}})"});
    action->click();
    ASSERT_TRUE(waitFor([&] { return peer.armSeen.load(); }, 5s));
    cancel->click();
    ASSERT_TRUE(waitFor([&] { return peer.disconnected.load(); }, 5s));
    EXPECT_TRUE(engineRequests(launcher, "script.debug.play").empty());
    EXPECT_TRUE(engineRequests(launcher, "play.start").empty());
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.stop").empty(); }, 5s));
    const auto cleanup = engineRequests(launcher, "script.debug.stop");
    ASSERT_NE(cleanup.front().find("params"), nullptr);
    EXPECT_EQ(cleanup.front().find("params")->stringAt("endpointId"), std::string(64, 'a'));
}

TEST(WorkbenchEngine, OldEndpointCleanupDoesNotReleaseSuccessorReservation) {
    LuaPeer first;
    LuaPeer second;
    ASSERT_NE(first.port, 0);
    ASSERT_NE(second.port, 0);
    first.releaseArm.store(false);
    second.releaseArm.store(false);
    WorkDir dir("engine-lua-successor");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* cancel = button(panel, QStringLiteral("Cancel Request"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(cancel, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), first.port));
    launcher.expect(frost::io::ScriptedProcess{.neverExits = true});
    action->click();
    ASSERT_TRUE(waitFor([&] { return first.armSeen.load(); }, 5s));
    cancel->click();
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "script.debug.stop").empty(); }, 5s));
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), second.port, 'b'));
    launcher.expect(
        frost::io::ScriptedProcess{.stdoutBytes = R"({"ok":true,"value":{"result":{"stopped":true},"locations":{}}})"});
    action->click();
    ASSERT_TRUE(waitFor([&] { return second.armSeen.load(); }, 5s));
    const auto build = rig.workbench->build().build(frost::build::BuildAction::Build);
    ASSERT_FALSE(build);
    EXPECT_EQ(build.error().code(), frost::ErrorCode::Busy);
    cancel->click();
    ASSERT_TRUE(waitFor([&] { return second.disconnected.load(); }, 5s));
    ASSERT_TRUE(waitFor([&] { return engineRequests(launcher, "script.debug.stop").size() == 2; }, 5s));
    const auto cleanup = engineRequests(launcher, "script.debug.stop");
    ASSERT_NE(cleanup[1].find("params"), nullptr);
    EXPECT_EQ(cleanup[1].find("params")->stringAt("endpointId"), std::string(64, 'b'));
}

TEST(WorkbenchEngine, CancellingLuaFromReservationRefreshNeverStartsDiscovery) {
    WorkDir dir("engine-lua-refresh-cancel", "tiny_sln");
    dir.write("Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-csharp-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* start = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* build = rig.window->findChild<QToolButton*>(QStringLiteral("toolbar.build.project"));
    ASSERT_NE(start, nullptr);
    ASSERT_NE(build, nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            rig.window->refreshActions();
            return build->isEnabled();
        },
        5s));
    struct CancelOnDisable final : QObject {
        frost::ui::ShellWindow* window = nullptr;
        bool called = false;
        bool eventFilter(QObject* object, QEvent* event) override {
            auto* widget = qobject_cast<QWidget*>(object);
            if (!called && event->type() == QEvent::EnabledChange && widget && !widget->isEnabled()) {
                called = true;
                (void)window->invoke("run.stop");
            }
            return false;
        }
    } filter;
    filter.window = rig.window.get();
    build->installEventFilter(&filter);
    start->click();
    EXPECT_TRUE(filter.called);
    EXPECT_TRUE(engineRequests(launcher, "script.debug.start").empty());
    auto reservation = rig.workbench->buildService().reserveExternalLaunch();
    ASSERT_TRUE(reservation);
    rig.workbench->buildService().releaseExternalLaunch(*reservation);
}

TEST(WorkbenchEngine, ManagedSelectionRechecksSharedOwnerBeforePreparation) {
    WorkDir dir("managed-selection-owner", "tiny_sln");
    dir.write("Managed.csproj", "<Project />");
    dir.write("dotnet.exe", "fixture");
    dir.write("guard.dll", "fixture");
    dir.write("adapter.exe", "fixture");
    frost::io::ScriptedProcessLauncher launcher;
    WorkbenchRig* activeRig = nullptr;
    std::optional<std::uint64_t> owner;
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.managedPdbGuardPath = dir.join("guard.dll");
    options.workbench.selectManagedDebug = [&](std::string_view) -> std::optional<frost::app::ManagedDebugSelection> {
        auto acquired = activeRig->workbench->buildService().reserveExternalLaunch();
        if (acquired)
            owner = *acquired;
        return frost::app::ManagedDebugSelection{.project = dir.join("Managed.csproj"),
                                                 .dotnet = dir.join("dotnet.exe"),
                                                 .configuration = "Debug",
                                                 .framework = "net10.0",
                                                 .runtimeIdentifier = "",
                                                 .adapter = dir.join("adapter.exe")};
    };
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-csharp-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-server.exe")));
    activeRig = &rig;
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto id = rig.workbench->registry().find("run.debugManaged");
    ASSERT_TRUE(id);
    frost::input::ActionContext context;
    context.trusted = true;
    ASSERT_EQ(rig.workbench->registry().tryInvoke(*id, context), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(owner);
    EXPECT_TRUE(launcher.launches().empty());
    rig.workbench->buildService().releaseExternalLaunch(*owner);
}

TEST(WorkbenchEngine, ManagedSelectionRechecksActiveDebugAfterReservationReleased) {
    WorkDir dir("managed-selection-debug", "tiny_sln");
    dir.write("Managed.csproj", "<Project />");
    dir.write("dotnet.exe", "fixture");
    dir.write("guard.dll", "fixture");
    dir.write("adapter.exe", "fixture");
    frost::io::ScriptedProcessLauncher launcher;
    frost::debug::FakeDebugAdapter adapter;
    frost::debug::scriptProgram(adapter,
                                {.sourcePath = dir.join("App/main.cpp"), .codeLines = {1}, .runsForever = true});
    WorkbenchRig* activeRig = nullptr;
    bool sessionStarted = false;
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.managedPdbGuardPath = dir.join("guard.dll");
    options.workbench.debugAdapterFactory =
        [&](frost::debug::DapHandlers handlers) -> frost::Result<std::unique_ptr<frost::debug::IDebugAdapterClient>> {
        return frost::debug::makeDapClient(adapter.transport(), std::move(handlers));
    };
    options.workbench.selectManagedDebug = [&](std::string_view) -> std::optional<frost::app::ManagedDebugSelection> {
        auto reservation = activeRig->workbench->buildService().reserveExternalLaunch();
        EXPECT_TRUE(reservation);
        if (!reservation)
            return std::nullopt;
        auto* controller = activeRig->workbench->debugController();
        auto started = controller->startTarget(
            {.name = "competing debug", .program = dir.join("adapter.exe"), .trustRoots = {dir.path()}});
        EXPECT_TRUE(started);
        activeRig->workbench->buildService().releaseExternalLaunch(*reservation);
        sessionStarted =
            started && waitFor([&] { return controller->state() == frost::debug::DebugState::Running; }, 5s);
        EXPECT_TRUE(sessionStarted);
        EXPECT_FALSE(activeRig->workbench->buildService().externalLaunchAdmissionBusy());
        return frost::app::ManagedDebugSelection{.project = dir.join("Managed.csproj"),
                                                 .dotnet = dir.join("dotnet.exe"),
                                                 .configuration = "Debug",
                                                 .framework = "net10.0",
                                                 .runtimeIdentifier = "",
                                                 .adapter = dir.join("adapter.exe")};
    };
    WorkbenchRig rig(dir, options);
    activeRig = &rig;
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-csharp-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto id = rig.workbench->registry().find("run.debugManaged");
    ASSERT_TRUE(id);
    frost::input::ActionContext context;
    context.trusted = true;
    ASSERT_EQ(rig.workbench->registry().tryInvoke(*id, context), frost::input::InvokeResult::Ran);
    EXPECT_TRUE(sessionStarted);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_TRUE(rig.workbench->debugController()->debugging());
    rig.workbench->debugController()->stop();
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->debugController()->debugging(); }, 5s));
}

TEST(WorkbenchEngine, PendingLuaHandshakeExcludesCompetingProjectOperations) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    peer.releaseArm.store(false);
    WorkDir dir("engine-lua-admission", "tiny_sln");
    dir.write("Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    launcher.expect(luaEndpointReply(dir.join("Game.frostproj"), peer.port));
    launcher.expect(
        frost::io::ScriptedProcess{.stdoutBytes = R"({"ok":true,"value":{"result":{"stopped":true},"locations":{}}})"});
    action->click();
    ASSERT_TRUE(waitFor([&] { return peer.armSeen.load(); }, 5s));
    const auto build = rig.workbench->build().build(frost::build::BuildAction::Build);
    ASSERT_FALSE(build);
    EXPECT_EQ(build.error().code(), frost::ErrorCode::Busy);
    const auto edit = rig.workbench->buildService().beginProjectEdit();
    ASSERT_FALSE(edit);
    EXPECT_EQ(edit.error().code(), frost::ErrorCode::Busy);
    const auto run = rig.workbench->runController()->run();
    ASSERT_FALSE(run);
    EXPECT_EQ(run.error().code(), frost::ErrorCode::Busy);
    const auto debug = rig.workbench->debugController()->debug();
    ASSERT_FALSE(debug);
    EXPECT_EQ(debug.error().code(), frost::ErrorCode::Busy);
    EXPECT_FALSE(rig.workbench->registry().isEnabled(*rig.workbench->registry().find("run.debugManaged"),
                                                     rig.window->actionContext()));
    EXPECT_EQ(rig.window->invoke("run.stop"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return peer.disconnected.load(); }, 5s));
    auto next = rig.workbench->buildService().reserveExternalLaunch();
    ASSERT_TRUE(next);
    rig.workbench->buildService().releaseExternalLaunch(*next);
    EXPECT_TRUE(engineRequests(launcher, "script.debug.play").empty());
}

TEST(WorkbenchEngine, LuaStartRefusesAnActiveProjectEditReservation) {
    WorkDir dir("engine-lua-reverse-admission", "tiny_sln");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-csharp-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(rig.workbench->buildService().beginProjectEdit());
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    action->click();
    const auto launches = launcher.launches();
    std::string launchDetails;
    for (const auto& record : launches)
        launchDetails += "\nexecutable=" + record.spec.executable + " launched=" + std::to_string(record.launched);
    EXPECT_TRUE(launches.empty()) << launchDetails;
    EXPECT_TRUE(hasStatus(panel, QStringLiteral("Stop the current")));
    rig.workbench->buildService().cancelProjectEdit();
}

TEST(WorkbenchEngine, LuaReservationStartsBeforeStalledDiscoveryAndStopReleasesIt) {
    WorkDir dir("engine-lua-discovery-admission", "tiny_sln");
    RigOptions options;
    options.workbench.engineDiscoveryHelperPath = FROST_DISCOVERY_STALL_CHILD;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    action->click();
    const auto edit = rig.workbench->buildService().beginProjectEdit();
    ASSERT_FALSE(edit);
    EXPECT_EQ(edit.error().code(), frost::ErrorCode::Busy);
    const auto build = rig.workbench->build().build(frost::build::BuildAction::Build);
    ASSERT_FALSE(build);
    EXPECT_EQ(build.error().code(), frost::ErrorCode::Busy);
    EXPECT_EQ(rig.window->invoke("run.stop"), frost::input::InvokeResult::Ran);
    auto next = rig.workbench->buildService().reserveExternalLaunch();
    ASSERT_TRUE(next);
    rig.workbench->buildService().releaseExternalLaunch(*next);
}

TEST(WorkbenchEngine, LuaStartRefusesActiveCMakeConfigure) {
    WorkDir dir("engine-lua-cmake-reverse");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Test C)\n");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openCMakeProject(dir.join("CMakeLists.txt")));
    auto* cmake = rig.workbench->cmakeService();
    ASSERT_NE(cmake, nullptr);
    ASSERT_TRUE(cmake->configure());
    ASSERT_EQ(cmake->state(), frost::build::BuildState::NeedsTrust);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    action->click();
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_TRUE(hasStatus(panel, QStringLiteral("Stop the current")));
    cmake->cancel();
}

TEST(WorkbenchEngine, PendingLuaHandshakeBlocksCMakeBuild) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    peer.releaseArm.store(false);
    WorkDir dir("engine-lua-cmake-admission");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Test C)\n");
    dir.write("Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openCMakeProject(dir.join("CMakeLists.txt")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    launcher.expect(luaEndpointReply(dir.join("Game.frostproj"), peer.port));
    launcher.expect(
        frost::io::ScriptedProcess{.stdoutBytes = R"({"ok":true,"value":{"result":{"stopped":true},"locations":{}}})"});
    action->click();
    ASSERT_TRUE(waitFor([&] { return peer.armSeen.load(); }, 5s));
    const auto build = rig.workbench->build().build(frost::build::BuildAction::Build);
    ASSERT_FALSE(build);
    EXPECT_EQ(build.error().code(), frost::ErrorCode::Busy);
    auto* cmake = rig.workbench->cmakeService();
    ASSERT_NE(cmake, nullptr);
    const auto direct = cmake->build(frost::build::BuildAction::Build);
    ASSERT_FALSE(direct);
    EXPECT_EQ(direct.error().code(), frost::ErrorCode::Busy);
    const auto reload = cmake->configure();
    ASSERT_FALSE(reload);
    EXPECT_EQ(reload.error().code(), frost::ErrorCode::Busy);
    const auto profile = cmake->setActiveProfile(std::size_t{0});
    ASSERT_FALSE(profile);
    EXPECT_EQ(profile.error().code(), frost::ErrorCode::Busy);
    EXPECT_EQ(rig.window->invoke("run.stop"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return peer.disconnected.load(); }, 5s));
}

TEST(WorkbenchEngine, RevokingTrustDuringLuaHandshakeClosesPeerAndNeverStartsPlay) {
    LuaPeer peer;
    ASSERT_NE(peer.port, 0);
    peer.releaseArm.store(false);
    WorkDir dir("engine-lua-revoke-handshake");
    dir.write("project/Game.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    launcher.expect(luaEndpointReply(dir.join("project/Game.frostproj"), peer.port));
    launcher.expect(
        frost::io::ScriptedProcess{.stdoutBytes = R"({"ok":true,"value":{"result":{"stopped":true},"locations":{}}})"});
    action->click();
    ASSERT_TRUE(waitFor([&] { return peer.armSeen.load(); }, 5s));
    ASSERT_TRUE(rig.trust->untrust(dir.join("project")));
    ASSERT_TRUE(waitFor([&] { return peer.disconnected.load(); }, 5s));
    EXPECT_TRUE(engineRequests(launcher, "script.debug.play").empty());
    EXPECT_TRUE(engineRequests(launcher, "play.start").empty());
    EXPECT_TRUE(engineRequests(launcher, "script.debug.stop").empty()); // No new command after trust is revoked.
}

TEST(WorkbenchEngine, RefusesUntrustedAndCancelsPendingOnProjectSwitch) {
    WorkDir dir("engine-trust-switch");
    dir.write("first/Game.frostproj", "{}");
    dir.write("second/Other.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("first")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* play = button(panel, QStringLiteral("Play"));
    ASSERT_NE(play, nullptr);
    play->click();
    EXPECT_TRUE(hasStatus(panel, QStringLiteral("Trust the project")));
    EXPECT_TRUE(launcher.launches().empty());
    ASSERT_TRUE(rig.trust->trust(dir.join("first")));
    launcher.expect(frost::io::ScriptedProcess{.neverExits = true});
    play->click();
    ASSERT_TRUE(waitFor([&] { return !engineRequests(launcher, "play.start").empty(); }, 3s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("second")));
    EXPECT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("No request running")); }, 3s));
    EXPECT_EQ(engineRequests(launcher, "play.start").size(), 1U);
}
TEST(WorkbenchEngine, LateCompletionCannotPopulateAnotherProject) {
    WorkDir dir("engine-stale-completion");
    dir.write("first/Game.frostproj", "{}");
    dir.write("second/Other.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("first")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("first")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* output = panel->findChild<QListWidget*>();
    auto* state = button(panel, QStringLiteral("State"));
    ASSERT_NE(output, nullptr);
    ASSERT_NE(state, nullptr);
    const auto reply = frost::lang::Json::object()
                           .set("ok", true)
                           .set("value", frost::lang::Json::object()
                                             .set("result", frost::lang::Json::object().set("state", "Play"))
                                             .set("locations", frost::lang::Json::object()));
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply.dump(), .chunkBytes = 2, .chunkInterval = 100ms});
    state->click();
    ASSERT_TRUE(waitFor([&] { return !launcher.launches().empty(); }, 3s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("second")));
    static_cast<void>(waitFor([] { return false; }, 400ms));
    EXPECT_EQ(output->count(), 0);
    EXPECT_EQ(rig.workbench->projectRoot(), dir.join("second"));
}
TEST(WorkbenchEngine, ShutdownCancelsStalledHelperAndPermanentlyRefusesRequests) {
    WorkDir dir("engine-shutdown");
    dir.write("project/Game.frostproj", "{}");
    RigOptions options;
    options.workbench.engineDiscoveryHelperPath = FROST_DISCOVERY_STALL_CHILD;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* play = button(panel, QStringLiteral("Play"));
    ASSERT_NE(play, nullptr);
    play->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("Contacting")); }, 2s));
    std::this_thread::sleep_for(100ms);
    const auto start = std::chrono::steady_clock::now();
    rig.workbench->shutdown();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 4s);
    play->click();
    EXPECT_FALSE(hasStatus(panel, QStringLiteral("Contacting")));
}
TEST(WorkbenchEngine, DiscoveryFindsAliasRecordInsteadOfOtherProjectFallback) {
    WorkDir dir("engine-alias-record");
    dir.write("project/Game.frostproj", "{}");
    dir.write("other/Other.frostproj", "{}");
    auto root = frost::io::toStdPath(dir.join("project"));
    auto alias = frost::io::toStdPath(dir.join("alias"));
    ASSERT_TRUE(root && alias);
    std::error_code error;
    std::filesystem::create_directory_symlink(*root, *alias, error);
    if (error)
        GTEST_SKIP() << "directory symbolic links unavailable: " << error.message();
    const auto record = [&](std::string_view path) {
        return frost::lang::Json::object()
            .set("host", "127.0.0.1")
            .set("port", 1)
            .set("tokenRequired", false)
            .set("projectPath", path)
            .dump();
    };
    dir.write("automation/Projects/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.json", record(dir.join("alias")));
    dir.write("automation/Projects/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.json", record(dir.join("other")));
    dir.write("automation/Automation.json", record(dir.join("other")));
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    auto input = frost::lang::Json::object()
                     .set("root", dir.join("project"))
                     .set("directory", dir.join("automation"))
                     .set("trustPath", rig.trust->storePath())
                     .set("method", "play.state")
                     .set("params", frost::lang::Json::object());
    int connections = 0;
    auto connector = [&](std::uint16_t port, auto,
                         std::stop_token) -> frost::Result<std::unique_ptr<frost::lang::IEngineConnection>> {
        ++connections;
        EXPECT_EQ(port, 1);
        return frost::fail(frost::ErrorCode::IoError, "fixture refused connection");
    };
    auto result = frost::app::executeEngineHelper(input, connector);
    ASSERT_FALSE(result);
    // Only the alias record reaches the connector; the other project's fallback fails validation.
    EXPECT_EQ(connections, 1);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::IoError);
}
TEST(WorkbenchEngine, RealPendingSocketDoesNotOutliveWorkbench) {
    const QString root = qEnvironmentVariable("FROST_TEST_ENGINE_ROOT");
    const QString helper = qEnvironmentVariable("FROST_TEST_ENGINE_HELPER");
    const QString marker = qEnvironmentVariable("FROST_TEST_STALL_MARKER");
    if (root.isEmpty() || helper.isEmpty() || marker.isEmpty())
        GTEST_SKIP() << "isolated stalled-socket fixture not provided";
    WorkDir dir("engine-stalled-socket");
    RigOptions options;
    options.workbench.engineDiscoveryHelperPath = helper.toUtf8().toStdString();
    WorkbenchRig rig(dir, options);
    const std::string project = root.toUtf8().toStdString();
    ASSERT_TRUE(rig.trust->trust(project));
    ASSERT_TRUE(rig.workbench->openExternalProject(project));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* state = button(panel, QStringLiteral("State"));
    ASSERT_NE(state, nullptr);
    state->click();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(marker); }, 10s));
    const auto start = std::chrono::steady_clock::now();
    rig.workbench->shutdown();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 4s);
    state->click();
    EXPECT_FALSE(hasStatus(panel, QStringLiteral("Contacting")));
    rig.window.reset();
    rig.workbench.reset();
}
TEST(WorkbenchEngine, RevokedTrustStopsLiveHelperBeforeAction) {
    const QString root = qEnvironmentVariable("FROST_TEST_ENGINE_ROOT");
    const QString helper = qEnvironmentVariable("FROST_TEST_ENGINE_HELPER");
    const QString marker = qEnvironmentVariable("FROST_TEST_REVOCATION_MARKER");
    if (root.isEmpty() || helper.isEmpty() || marker.isEmpty())
        GTEST_SKIP() << "isolated trust-revocation fixture not provided";
    WorkDir dir("engine-revocation");
    RigOptions options;
    options.workbench.engineDiscoveryHelperPath = helper.toUtf8().toStdString();
    WorkbenchRig rig(dir, options);
    const std::string project = root.toUtf8().toStdString();
    ASSERT_TRUE(rig.trust->trust(project));
    ASSERT_TRUE(rig.workbench->openExternalProject(project));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* play = button(panel, QStringLiteral("Play"));
    ASSERT_NE(play, nullptr);
    play->click();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(marker); }, 10s));
    ASSERT_TRUE(rig.trust->untrust(project));
    QFile release(marker + QStringLiteral(".release"));
    ASSERT_TRUE(release.open(QIODevice::WriteOnly));
    release.close();
    EXPECT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("no longer trusted")); }, 10s));
}
TEST(WorkbenchEngine, LogRetentionEvictsExplicitlyAndProjectSwitchClearsResults) {
    WorkDir dir("engine-log-retention");
    dir.write("first/Game.frostproj", "{}");
    dir.write("second/Other.frostproj", "{}");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("first")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("first")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* refresh = button(panel, QStringLiteral("Refresh Log"));
    auto* output = panel->findChild<QListWidget*>();
    ASSERT_NE(refresh, nullptr);
    ASSERT_NE(output, nullptr);
    auto entries = frost::lang::Json::array();
    for (int i = 0; i < 10005; ++i)
        entries.push(frost::lang::Json::object().set("message", "entry"));
    auto page =
        frost::lang::Json::object().set("entries", std::move(entries)).set("nextSince", 10005).set("truncated", false);
    auto reply = frost::lang::Json::object()
                     .set("ok", true)
                     .set("value", frost::lang::Json::object()
                                       .set("result", std::move(page))
                                       .set("locations", frost::lang::Json::object()));
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply.dump()});
    refresh->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("Older output was removed")); }, 10s));
    EXPECT_EQ(output->count(), 10000);
    auto empty = frost::lang::Json::object()
                     .set("entries", frost::lang::Json::array())
                     .set("nextSince", 10005)
                     .set("truncated", false);
    auto second = frost::lang::Json::object()
                      .set("ok", true)
                      .set("value", frost::lang::Json::object()
                                        .set("result", std::move(empty))
                                        .set("locations", frost::lang::Json::object()));
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = second.dump()});
    refresh->click();
    ASSERT_TRUE(waitFor([&] { return engineRequests(launcher, "log.get").size() == 2; }, 5s));
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("Older output was removed")); }, 5s));
    const auto requests = engineRequests(launcher, "log.get");
    ASSERT_NE(requests[1].find("params"), nullptr);
    EXPECT_EQ(requests[1].find("params")->integerAt("since"), 10005);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("second")));
    EXPECT_EQ(output->count(), 0);
}
TEST(WorkbenchEngine, ResultActivationRevalidatesLocationBeforeOpening) {
    WorkDir dir("engine-location");
    dir.write("project/Game.frostproj", "{}");
    dir.write("project/Assets/Scripts/Test.lua", "local a = 1\nlocal b = 2\n");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* refresh = button(panel, QStringLiteral("Refresh Log"));
    auto* output = panel->findChild<QListWidget*>();
    ASSERT_NE(refresh, nullptr);
    ASSERT_NE(output, nullptr);
    constexpr auto relative = "Assets/Scripts/Test.lua";
    auto entries = frost::lang::Json::array().push(
        frost::lang::Json::object()
            .set("message", "failure")
            .set("source", frost::lang::Json::object().set("path", relative).set("line", 2)));
    auto page = frost::lang::Json::object().set("entries", std::move(entries)).set("nextSince", 1);
    auto reply = frost::lang::Json::object()
                     .set("ok", true)
                     .set("value", frost::lang::Json::object()
                                       .set("result", std::move(page))
                                       .set("locations", frost::lang::Json::object().set(
                                                             relative, dir.join("project/Assets/Scripts/Test.lua"))));
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = reply.dump()});
    refresh->click();
    ASSERT_TRUE(waitFor([&] { return output->count() == 1; }, 5s));
    auto resolved =
        frost::lang::Json::object()
            .set("ok", true)
            .set("value", frost::lang::Json::object()
                              .set("result",
                                   frost::lang::Json::object().set("path", dir.join("project/Assets/Scripts/Test.lua")))
                              .set("locations", frost::lang::Json::object()));
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = resolved.dump()});
    emit output->itemDoubleClicked(output->item(0));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->currentView() != nullptr; }, 5s));
    EXPECT_EQ(rig.workbench->currentView()->status().line, 2);
    const auto requests = engineRequests(launcher, "navigation.resolve");
    ASSERT_EQ(requests.size(), 1U);
    ASSERT_NE(requests[0].find("params"), nullptr);
    EXPECT_EQ(requests[0].find("params")->stringAt("path"), relative);
}
TEST(WorkbenchEngine, RealEngineValidatesUnsavedBufferAndReportsState) {
    const QString root = qEnvironmentVariable("FROST_TEST_ENGINE_ROOT");
    const QString helper = qEnvironmentVariable("FROST_TEST_ENGINE_HELPER");
    if (root.isEmpty() || helper.isEmpty())
        GTEST_SKIP() << "isolated engine fixture not provided";
    WorkDir dir("engine-real-ui");
    RigOptions options;
    options.workbench.engineDiscoveryHelperPath = helper.toUtf8().toStdString();
    WorkbenchRig rig(dir, options);
    const std::string project = root.toUtf8().toStdString();
    ASSERT_TRUE(rig.trust->trust(project));
    ASSERT_TRUE(rig.workbench->openExternalProject(project));
    const std::string script = (root + QStringLiteral("/Assets/Scripts/AutomationTest.lua")).toUtf8().toStdString();
    ASSERT_TRUE(rig.workbench->openPath(script));
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->editor().typeText("this is invalid Lua!\n"));
    ASSERT_TRUE(view->document().dirty());
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* validate = button(panel, QStringLiteral("Validate Script"));
    ASSERT_NE(validate, nullptr);
    validate->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("Script validation failed")); }, 20s));
    auto* output = panel->findChild<QListWidget*>();
    ASSERT_NE(output, nullptr);
    EXPECT_GT(output->count(), 0);
    EXPECT_TRUE(view->document().dirty());
    auto* state = button(panel, QStringLiteral("State"));
    ASSERT_NE(state, nullptr);
    state->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 20s));
    ASSERT_GT(output->count(), 0);
    EXPECT_EQ(output->item(output->count() - 1)->text(), QStringLiteral("Edit"));
    auto* play = button(panel, QStringLiteral("Play"));
    auto* stop = button(panel, QStringLiteral("Stop"));
    ASSERT_NE(play, nullptr);
    ASSERT_NE(stop, nullptr);
    play->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 20s));
    ASSERT_GT(output->count(), 0);
    EXPECT_EQ(output->item(output->count() - 1)->text(), QStringLiteral("Play"));
    stop->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 20s));
    ASSERT_GT(output->count(), 0);
    EXPECT_EQ(output->item(output->count() - 1)->text(), QStringLiteral("Edit"));
}
TEST(WorkbenchEngine, RealEngineLuaStartupShowsCapturedSource) {
    const QString root = qEnvironmentVariable("FROST_TEST_ENGINE_ROOT");
    const QString helper = qEnvironmentVariable("FROST_TEST_ENGINE_HELPER");
    if (root.isEmpty() || helper.isEmpty())
        GTEST_SKIP() << "isolated engine fixture not provided";
    WorkDir dir("engine-real-lua");
    RigOptions options;
    options.workbench.engineDiscoveryHelperPath = helper.toUtf8().toStdString();
    WorkbenchRig rig(dir, options);
    const std::string project = root.toUtf8().toStdString();
    ASSERT_TRUE(rig.trust->trust(project));
    ASSERT_TRUE(rig.workbench->openExternalProject(project));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    ASSERT_NE(action, nullptr);
    action->click();
    auto* controller = rig.workbench->debugController();
    ASSERT_TRUE(waitFor([&] { return controller->paused(); }, 30s));
    ASSERT_TRUE(waitFor([&] { return !controller->frames().frames.empty(); }, 10s));
    const auto& frames = controller->frames().frames;
    const auto frame = std::find_if(frames.begin(), frames.end(),
                                    [](const auto& item) { return item.sourceReference > 0 && item.line > 0; });
    ASSERT_NE(frame, frames.end());
    EXPECT_GT(frame->line, 0);
    const QString sourceName = QString::fromUtf8(frame->sourceName.data(), static_cast<int>(frame->sourceName.size()));
    const QString sourcePath = QFileInfo(sourceName).isAbsolute() ? sourceName : root + QLatin1Char('/') + sourceName;
    QFile source(sourcePath);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly)) << sourcePath.toStdString();
    QString expected = QString::fromUtf8(source.readAll());
    expected.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    controller->selectFrame(frame->id);
    auto* loaded = rig.window->findChild<QPlainTextEdit*>(QStringLiteral("debugLoadedSource"));
    ASSERT_NE(loaded, nullptr);
    ASSERT_TRUE(waitFor([&] { return loaded->toPlainText() == expected; }, 10s));
    EXPECT_TRUE(loaded->isReadOnly());
    ASSERT_TRUE(rig.workbench->openPath(sourcePath.toUtf8().toStdString()));
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->editor().typeText("-- unsaved change\n"));
    ASSERT_TRUE(view->document().dirty());
    EXPECT_EQ(loaded->toPlainText(), expected);
    EXPECT_TRUE(controller->resume());
    ASSERT_TRUE(waitFor([&] { return !controller->paused(); }, 10s));
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("FrostEngine play started")); }, 20s));
    controller->stop();
    ASSERT_TRUE(waitFor([&] { return !controller->debugging(); }, 10s));
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 20s));
    auto* stop = button(panel, QStringLiteral("Stop"));
    ASSERT_NE(stop, nullptr);
    stop->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 20s));
    auto* output = panel->findChild<QListWidget*>();
    ASSERT_NE(output, nullptr);
    ASSERT_GT(output->count(), 0);
    EXPECT_EQ(output->item(output->count() - 1)->text(), QStringLiteral("Edit"));
}
TEST(WorkbenchEngine, RealEngineStopsOwnedPausedLuaPlay) {
    const QString root = qEnvironmentVariable("FROST_TEST_ENGINE_ROOT");
    const QString helper = qEnvironmentVariable("FROST_TEST_ENGINE_HELPER");
    if (root.isEmpty() || helper.isEmpty())
        GTEST_SKIP() << "isolated engine fixture not provided";
    WorkDir dir("engine-real-lua-stop");
    RigOptions options;
    options.workbench.engineDiscoveryHelperPath = helper.toUtf8().toStdString();
    WorkbenchRig rig(dir, options);
    const std::string project = root.toUtf8().toStdString();
    ASSERT_TRUE(rig.trust->trust(project));
    ASSERT_TRUE(rig.workbench->openExternalProject(project));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* action = button(panel, QStringLiteral("Debug Lua (experimental)"));
    auto* stop = button(panel, QStringLiteral("Stop"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(stop, nullptr);
    action->click();
    auto* controller = rig.workbench->debugController();
    ASSERT_TRUE(waitFor([&] { return controller->paused(); }, 30s));
    stop->click();
    ASSERT_TRUE(waitFor([&] { return !controller->debugging(); }, 20s));
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 20s));
    auto* state = button(panel, QStringLiteral("State"));
    ASSERT_NE(state, nullptr);
    auto* output = panel->findChild<QListWidget*>();
    ASSERT_NE(output, nullptr);
    const int previousCount = output->count();
    state->click();
    ASSERT_TRUE(waitFor([&] { return output->count() > previousCount; }, 20s));
    ASSERT_GT(output->count(), 0);
    EXPECT_EQ(output->item(output->count() - 1)->text(), QStringLiteral("Edit"));
}
TEST(WorkbenchEngine, RealEngineRefusesDiscoveryForAnotherProject) {
    const QString root = qEnvironmentVariable("FROST_TEST_ENGINE_WRONG_ROOT");
    const QString helper = qEnvironmentVariable("FROST_TEST_ENGINE_HELPER");
    if (root.isEmpty() || helper.isEmpty())
        GTEST_SKIP() << "isolated wrong-project engine fixture not provided";
    WorkDir dir("engine-wrong-project");
    RigOptions options;
    options.workbench.engineDiscoveryHelperPath = helper.toUtf8().toStdString();
    WorkbenchRig rig(dir, options);
    const std::string project = root.toUtf8().toStdString();
    ASSERT_TRUE(rig.trust->trust(project));
    ASSERT_TRUE(rig.workbench->openExternalProject(project));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* play = button(panel, QStringLiteral("Play"));
    ASSERT_NE(play, nullptr);
    play->click();
    EXPECT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("different project")); }, 20s));
}
TEST(WorkbenchEngine, RealEngineTestFailureAndLogLocationsNavigate) {
    const QString root = qEnvironmentVariable("FROST_TEST_ENGINE_ROOT");
    const QString helper = qEnvironmentVariable("FROST_TEST_ENGINE_HELPER");
    if (root.isEmpty() || helper.isEmpty())
        GTEST_SKIP() << "isolated engine fixture not provided";
    WorkDir dir("engine-real-results");
    RigOptions options;
    options.workbench.engineDiscoveryHelperPath = helper.toUtf8().toStdString();
    WorkbenchRig rig(dir, options);
    const std::string project = root.toUtf8().toStdString();
    ASSERT_TRUE(rig.trust->trust(project));
    ASSERT_TRUE(rig.workbench->openExternalProject(project));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* runTests = button(panel, QStringLiteral("Run Tests"));
    auto* refresh = button(panel, QStringLiteral("Refresh Log"));
    auto* output = panel->findChild<QListWidget*>();
    ASSERT_NE(runTests, nullptr);
    ASSERT_NE(refresh, nullptr);
    ASSERT_NE(output, nullptr);
    runTests->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 25s));
    const auto findNavigable = [&](const QString& fragment) -> QListWidgetItem* {
        for (int i = 0; i < output->count(); ++i) {
            auto* item = output->item(i);
            if (item->text().contains(fragment) && !item->data(Qt::UserRole).toString().isEmpty())
                return item;
        }
        return nullptr;
    };
    auto* failure = findNavigable(QStringLiteral("intentional failure"));
    ASSERT_NE(failure, nullptr);
    const int failureLine = failure->data(Qt::UserRole + 1).toInt();
    emit output->itemDoubleClicked(failure);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->currentView() != nullptr; }, 10s));
    EXPECT_EQ(rig.workbench->currentView()->status().line, failureLine);
    EXPECT_TRUE(QString::fromStdString(rig.workbench->currentView()->path()).endsWith(QStringLiteral("Failure.lua")));
    refresh->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("request complete")); }, 20s));
    auto* log = findNavigable(QStringLiteral("integration log"));
    ASSERT_NE(log, nullptr);
    const int logLine = log->data(Qt::UserRole + 1).toInt();
    emit output->itemDoubleClicked(log);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->currentView()->status().line == logLine; }, 10s));
}
} // namespace
