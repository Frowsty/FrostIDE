// SPDX-License-Identifier: Apache-2.0
//
// The Lua DAP conformance suite (LUA-11a; PLAN.md section 7 R4 & Appendix C): executes all 30 Lua
// debugging conformance scenarios against the FrostEngine Lua debug endpoint and
// luaDebugAdapterFactory with zero skipped cases, recording and verifying the exact pass/failure
// baseline before FER-8/FER-9 (R7) and LUA-11b (R12) replace the pause/continue-only adapter.

#include "frost/debug/dap_types.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/debug/lua_debug_adapter.hpp"
#include "frost/debug/lua_debug_endpoint.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/rpc/framing.hpp"
#include "frost/rpc/json.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <unistd.h>
#endif

#ifndef FROST_LUA_DAP_CONFORMANCE_BASELINE
#error "FROST_LUA_DAP_CONFORMANCE_BASELINE comes from tests/debug/CMakeLists.txt"
#endif

#ifndef FROST_CORPORA_LOCK_PATH
#error "FROST_CORPORA_LOCK_PATH comes from tests/debug/CMakeLists.txt"
#endif

namespace frost::debug {
namespace {

using namespace std::chrono_literals;

#ifdef _WIN32
using Socket = SOCKET;
using SocketLength = int;
constexpr Socket kInvalid = INVALID_SOCKET;
void closeSocket(Socket socket) { closesocket(socket); }

struct WinsockScope {
    WinsockScope() {
        WSADATA data{};
        EXPECT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
    }
    ~WinsockScope() { WSACleanup(); }
};
#else
using Socket = int;
using SocketLength = std::size_t;
constexpr Socket kInvalid = -1;
void closeSocket(Socket socket) { close(socket); }

struct WinsockScope {};
#endif

constexpr std::string_view kToken = "0123456789abcdef0123456789abcdef";
constexpr std::uint64_t kPlayOperationId = 77;
constexpr std::string_view kPlayerScriptSource = "local PlayerController = {}\n"
                                                 "function PlayerController:OnUpdate(dt)\n"
                                                 "    local speed = 10\n"
                                                 "    return speed * dt\n"
                                                 "end\n"
                                                 "return PlayerController\n";

struct ScenarioOutcome {
    std::string id;
    std::string category;
    std::string command;
    bool passed = false;
    bool skipped = false;
    std::string failureReason;

    friend bool operator==(const ScenarioOutcome&, const ScenarioOutcome&) = default;
};

// Strips the "<command>: " prefix added by dap::responseError so the outcome records the exact
// message returned by the adapter.
std::string adapterErrorMessage(const Error& error, std::string_view command) {
    const std::string_view message = error.message();
    const std::string prefix = std::format("{}: ", command);
    if (message.starts_with(prefix)) {
        return std::string(message.substr(prefix.size()));
    }
    if (const std::size_t colon = message.find(": "); colon != std::string_view::npos) {
        return std::string(message.substr(colon + 2));
    }
    return std::string(message);
}

// Loopback server speaking the current experimental FrostEngine Lua debug wire protocol.
class LoopbackLuaEngineServer {
public:
    LoopbackLuaEngineServer() : m_listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        EXPECT_NE(m_listener, kInvalid);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        EXPECT_EQ(listen(m_listener, 1), 0);
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        EXPECT_EQ(getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
        m_port = ntohs(address.sin_port);
        m_thread = std::jthread([this] { serve(); });
    }

    ~LoopbackLuaEngineServer() {
        m_stop.store(true);
        if (m_listener != kInvalid) {
            closeSocket(m_listener);
            m_listener = kInvalid;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] bool disconnected() const noexcept { return m_disconnected.load(); }

private:
    static bool sendAll(Socket peer, std::string_view wire) {
        while (!wire.empty()) {
            const int written = static_cast<int>(send(peer, wire.data(), static_cast<SocketLength>(wire.size()), 0));
            if (written <= 0) {
                return false;
            }
            wire.remove_prefix(static_cast<std::size_t>(written));
        }
        return true;
    }

    static rpc::Json engineFrames() {
        return rpc::Json::array({
            rpc::Json::object()
                .set("id", 101)
                .set("name", "PlayerController:OnUpdate")
                .set("line", 42)
                .set("sourceId", "Assets/Scripts/Gameplay/PlayerController.lua")
                .set("sourceName", "PlayerController.lua"),
            rpc::Json::object()
                .set("id", 102)
                .set("name", "StepMovement")
                .set("line", 19)
                .set("sourceId", "Assets/Scripts/Gameplay/Movement.lua")
                .set("sourceName", "Movement.lua"),
            rpc::Json::object()
                .set("id", 103)
                .set("name", "TickWorld")
                .set("line", 88)
                .set("sourceId", "Assets/Scripts/Core/WorldLoop.lua")
                .set("sourceName", "WorldLoop.lua"),
        });
    }

    void serve() {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(m_listener, &ready);
        timeval timeout{10, 0};
        if (select(static_cast<int>(m_listener + 1), &ready, nullptr, nullptr, &timeout) <= 0) {
            return;
        }
        const Socket peer = accept(m_listener, nullptr, nullptr);
        if (peer == kInvalid) {
            return;
        }
        rpc::FrameDecoder decoder(64 * 1024);
        std::array<char, 4096> buffer{};
        bool authenticated = false;
        while (!m_stop.load()) {
            FD_ZERO(&ready);
            FD_SET(peer, &ready);
            timeval readTimeout{0, 100000};
            const int sel = select(static_cast<int>(peer + 1), &ready, nullptr, nullptr, &readTimeout);
            if (sel < 0) {
                break;
            }
            if (sel == 0) {
                continue;
            }
            const int count = static_cast<int>(recv(peer, buffer.data(), static_cast<SocketLength>(buffer.size()), 0));
            if (count <= 0) {
                m_disconnected.store(true);
                break;
            }
            decoder.feed(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
            bool closed = false;
            while (auto frame = decoder.next()) {
                if (!*frame) {
                    closed = true;
                    break;
                }
                const auto message = rpc::parseJson(**frame);
                if (!message || !message->isObject()) {
                    closed = true;
                    break;
                }
                std::string wire;
                if (!authenticated) {
                    authenticated = message->stringAt("token") == kToken;
                    wire = rpc::encodeFrame(
                        rpc::Json::object().set("authenticated", authenticated).set("connectionGeneration", 1).dump());
                    if (!sendAll(peer, wire) || !authenticated) {
                        closed = true;
                        break;
                    }
                    continue;
                }
                const auto requestId = message->integerAt("requestId").value_or(0);
                const std::string_view command = message->stringAt("command");
                if (command == "reservePlay") {
                    wire = rpc::encodeFrame(rpc::Json::object()
                                                .set("requestId", requestId)
                                                .set("success", true)
                                                .set("operationId", static_cast<std::int64_t>(kPlayOperationId))
                                                .dump());
                } else if (command == "armPlay" || command == "armPause") {
                    wire =
                        rpc::encodeFrame(rpc::Json::object().set("requestId", requestId).set("success", true).dump());
                } else if (command == "pause") {
                    wire =
                        rpc::encodeFrame(rpc::Json::object().set("requestId", requestId).set("success", true).dump()) +
                        rpc::encodeFrame(rpc::Json::object().set("event", "stopped").set("reason", "pause").dump());
                } else if (command == "stackTrace") {
                    wire = rpc::encodeFrame(rpc::Json::object()
                                                .set("requestId", requestId)
                                                .set("success", true)
                                                .set("body", rpc::Json::object()
                                                                 .set("stopGeneration", 1)
                                                                 .set("hasMore", false)
                                                                 .set("frames", engineFrames()))
                                                .dump());
                } else if (command == "source") {
                    const bool valid =
                        message->stringAt("sourceId") == "Assets/Scripts/Gameplay/PlayerController.lua" &&
                        message->integerAt("stopGeneration") == 1;
                    wire = rpc::encodeFrame(rpc::Json::object()
                                                .set("requestId", requestId)
                                                .set("success", valid)
                                                .set("body", rpc::Json::object().set("content", kPlayerScriptSource))
                                                .dump());
                } else if (command == "continue") {
                    wire =
                        rpc::encodeFrame(rpc::Json::object().set("requestId", requestId).set("success", true).dump()) +
                        rpc::encodeFrame(rpc::Json::object().set("event", "continued").dump());
                } else if (command == "stopPlay") {
                    const bool matches =
                        message->integerAt("operationId") == static_cast<std::int64_t>(kPlayOperationId);
                    wire =
                        rpc::encodeFrame(
                            rpc::Json::object().set("requestId", requestId).set("success", matches).dump()) +
                        (matches ? rpc::encodeFrame(rpc::Json::object()
                                                        .set("event", "playStopped")
                                                        .set("operationId", static_cast<std::int64_t>(kPlayOperationId))
                                                        .dump())
                                 : std::string{});
                } else {
                    wire =
                        rpc::encodeFrame(rpc::Json::object().set("requestId", requestId).set("success", false).dump());
                }
                if (!sendAll(peer, wire)) {
                    closed = true;
                    break;
                }
            }
            if (closed) {
                break;
            }
        }
        closeSocket(peer);
    }

    Socket m_listener = kInvalid;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_disconnected{false};
    std::jthread m_thread;
};

struct ConformanceClientHarness {
    WinsockScope winsock;
    LoopbackLuaEngineServer server;
    mutable Mutex mutex;
    std::vector<std::pair<std::string, rpc::Json>> events FROST_GUARDED_BY(mutex);
    std::atomic<bool> closed{false};
    std::unique_ptr<IDebugAdapterClient> client;

    bool connectStandard() {
        DapHandlers handlers{
            .onEvent =
                [this](std::string_view event, const rpc::Json& body) {
                    LockGuard lock(mutex);
                    events.emplace_back(std::string(event), body);
                },
            .onClosed = [this](const Error&) { closed.store(true); },
        };
        auto factory = luaDebugAdapterFactory(server.port(), std::string(kToken), 5s);
        auto created = factory(std::move(handlers));
        if (!created) {
            ADD_FAILURE() << created.error().describe();
            return false;
        }
        client = std::move(*created);
        return true;
    }

    bool connectPlayOwned() {
        auto play = connectLuaDebugPlayEndpoint(server.port(), kToken, std::chrono::steady_clock::now() + 5s);
        if (!play) {
            ADD_FAILURE() << play.error().describe();
            return false;
        }
        DapHandlers handlers{
            .onEvent =
                [this](std::string_view event, const rpc::Json& body) {
                    LockGuard lock(mutex);
                    events.emplace_back(std::string(event), body);
                },
            .onClosed = [this](const Error&) { closed.store(true); },
        };
        auto factory = luaDebugAdapterFactory(std::move(play->stream), play->nextWireRequest, play->operationId);
        auto created = factory(std::move(handlers));
        if (!created) {
            ADD_FAILURE() << created.error().describe();
            return false;
        }
        client = std::move(*created);
        return true;
    }

    Result<rpc::Json> request(std::string command, rpc::Json arguments = rpc::Json::object()) {
        auto promise = std::make_shared<std::promise<Result<rpc::Json>>>();
        auto future = promise->get_future();
        client->request(std::move(command), std::move(arguments),
                        [promise](Result<rpc::Json> reply) { promise->set_value(std::move(reply)); });
        if (future.wait_for(5s) != std::future_status::ready) {
            return fail(ErrorCode::Timeout, "Lua debug adapter request timed out");
        }
        return future.get();
    }

    bool waitForEvent(std::string_view name, rpc::Json* outBody = nullptr) {
        return test::waitUntil(
            [&] {
                LockGuard lock(mutex);
                for (const auto& [eventName, body] : events) {
                    if (eventName == name) {
                        if (outBody != nullptr) {
                            *outBody = body;
                        }
                        return true;
                    }
                }
                return false;
            },
            5s);
    }

    void clearEvents() {
        LockGuard lock(mutex);
        events.clear();
    }
};

ScenarioOutcome makeOutcome(std::string id, std::string category, std::string command, const Result<rpc::Json>& reply,
                            bool extraCheck = true, std::string_view extraFailure = {}) {
    ScenarioOutcome outcome{
        .id = std::move(id),
        .category = std::move(category),
        .command = std::move(command),
        .passed = false,
        .skipped = false,
        .failureReason = {},
    };
    if (!reply) {
        outcome.failureReason = adapterErrorMessage(reply.error(), outcome.command);
        return outcome;
    }
    if (!extraCheck) {
        outcome.failureReason = std::string(extraFailure);
        return outcome;
    }
    outcome.passed = true;
    return outcome;
}

// Executes all 30 conformance scenarios against the live endpoint + luaDebugAdapterFactory.
std::vector<ScenarioOutcome> runAllConformanceScenarios() {
    std::vector<ScenarioOutcome> outcomes;
    outcomes.reserve(30);

    ConformanceClientHarness harness;
    if (!harness.connectStandard()) {
        return outcomes;
    }

    // 1. handshake.initialize
    {
        const auto reply = harness.request("initialize", dap::initializeArguments("frost-lua"));
        const bool ok = reply && dap::parseCapabilities(*reply).supportsConfigurationDoneRequest;
        outcomes.push_back(makeOutcome("handshake.initialize", "handshake", "initialize", reply, ok,
                                       "initialize did not report supportsConfigurationDoneRequest"));
    }

    // 2. handshake.attach_and_configuration_done
    {
        const auto attachReply = harness.request("attach", rpc::Json::object());
        const bool initializedArrived = attachReply && harness.waitForEvent("initialized");
        const auto configReply = initializedArrived ? harness.request("configurationDone", rpc::Json::object())
                                                    : fail(ErrorCode::IoError, "attach did not emit initialized");
        outcomes.push_back(makeOutcome("handshake.attach_and_configuration_done", "handshake", "configurationDone",
                                       configReply, initializedArrived, "initialized event was not emitted"));
    }

    // 3. threads.single_lua_thread
    {
        const auto reply = harness.request("threads");
        bool ok = false;
        if (reply) {
            if (const rpc::Json* list = reply->find("threads"); list != nullptr && list->size() == 1) {
                const dap::Thread thread = dap::parseThread(list->items().front());
                ok = thread.id == 1 && thread.name == "Lua";
            }
        }
        outcomes.push_back(makeOutcome("threads.single_lua_thread", "threads", "threads", reply, ok,
                                       "threads did not return thread 1 'Lua'"));
    }

    // 4. execution.pause_and_stopped_event
    {
        harness.clearEvents();
        const auto reply = harness.request("pause", rpc::Json::object().set("threadId", 1));
        rpc::Json stoppedBody;
        const bool stopped = reply && harness.waitForEvent("stopped", &stoppedBody);
        const dap::StoppedEvent event = dap::parseStoppedEvent(stoppedBody);
        const bool ok = stopped && event.reason == "pause" && event.threadId == 1 && event.allThreadsStopped;
        outcomes.push_back(makeOutcome("execution.pause_and_stopped_event", "execution", "pause", reply, ok,
                                       "pause did not produce a stopped event on thread 1"));
    }

    // 5. stack.stack_trace_while_stopped
    std::int64_t topSourceReference = 0;
    std::int64_t topFrameId = 0;
    {
        const auto reply =
            harness.request("stackTrace", rpc::Json::object().set("threadId", 1).set("startFrame", 0).set("levels", 0));
        bool ok = false;
        if (reply) {
            const rpc::Json* frames = reply->find("stackFrames");
            const auto total = reply->integerAt("totalFrames");
            if (frames != nullptr && frames->size() == 3 && total == 3) {
                const dap::StackFrame top = dap::parseStackFrame(frames->items().front());
                topFrameId = top.id;
                topSourceReference = top.source ? top.source->sourceReference : 0;
                ok = top.id == 101 && top.name == "PlayerController:OnUpdate" && top.line == 42 && top.column == 1 &&
                     top.source && top.source->name == "PlayerController.lua" && topSourceReference > 0;
            }
        }
        outcomes.push_back(makeOutcome("stack.stack_trace_while_stopped", "stack", "stackTrace", reply, ok,
                                       "stackTrace did not return the expected stopped Lua frames"));
    }

    // 6. stack.stack_trace_paging
    {
        const auto reply =
            harness.request("stackTrace", rpc::Json::object().set("threadId", 1).set("startFrame", 1).set("levels", 1));
        bool ok = false;
        if (reply) {
            const rpc::Json* frames = reply->find("stackFrames");
            const auto total = reply->integerAt("totalFrames");
            if (frames != nullptr && frames->size() == 1 && total == 3) {
                const dap::StackFrame middle = dap::parseStackFrame(frames->items().front());
                ok = middle.id == 102 && middle.name == "StepMovement" && middle.line == 19;
            }
        }
        outcomes.push_back(makeOutcome("stack.stack_trace_paging", "stack", "stackTrace", reply, ok,
                                       "stackTrace paging did not slice frames with totalFrames"));
    }

    // 7. source.loaded_source_retrieval
    {
        const auto reply = harness.request("source", rpc::Json::object().set("sourceReference", topSourceReference));
        const bool ok =
            reply && reply->stringAt("content") == kPlayerScriptSource && reply->stringAt("mimeType") == "text/x-lua";
        outcomes.push_back(makeOutcome("source.loaded_source_retrieval", "source", "source", reply, ok,
                                       "source did not return the loaded Lua source and mimeType"));
    }

    // 8. execution.continue_and_continued_event
    {
        harness.clearEvents();
        const auto reply = harness.request("continue", rpc::Json::object().set("threadId", 1));
        rpc::Json continuedBody;
        const bool continued = reply && reply->boolAt("allThreadsContinued") == true &&
                               harness.waitForEvent("continued", &continuedBody) &&
                               continuedBody.integerAt("threadId") == 1 &&
                               continuedBody.boolAt("allThreadsContinued") == true;
        outcomes.push_back(makeOutcome("execution.continue_and_continued_event", "execution", "continue", reply,
                                       continued, "continue did not emit continued for thread 1"));
    }

    // 9. lifecycle.owned_play_stop
    {
        ConformanceClientHarness playHarness;
        if (playHarness.connectPlayOwned()) {
            const auto reply = playHarness.request("frost.stopPlay", rpc::Json::object());
            const bool terminated = reply && playHarness.waitForEvent("terminated");
            outcomes.push_back(makeOutcome("lifecycle.owned_play_stop", "lifecycle", "frost.stopPlay", reply,
                                           terminated, "frost.stopPlay did not emit terminated on playStopped"));
            playHarness.client->close();
        }
    }

    // 10. lifecycle.disconnect
    {
        ConformanceClientHarness disconnectHarness;
        if (disconnectHarness.connectStandard()) {
            const auto reply = disconnectHarness.request(
                "disconnect", rpc::Json::object().set("terminateDebuggee", false).set("restart", false));
            const bool cleanClose =
                reply && test::waitUntil([&] { return disconnectHarness.server.disconnected(); }, 5s);
            outcomes.push_back(makeOutcome("lifecycle.disconnect", "lifecycle", "disconnect", reply, cleanClose,
                                           "disconnect did not close the endpoint stream cleanly"));
            disconnectHarness.client->close();
        }
    }

    const std::string scriptPath = test::workPath("Assets/Scripts/Gameplay/PlayerController.lua");

    // 11. breakpoints.line
    {
        const auto reply = harness.request("setBreakpoints", dap::setBreakpointsArguments(scriptPath, {42}));
        outcomes.push_back(makeOutcome("breakpoints.line", "breakpoints", "setBreakpoints", reply));
    }

    // 12. breakpoints.multiple_lines
    {
        const auto reply = harness.request("setBreakpoints", dap::setBreakpointsArguments(scriptPath, {10, 25, 42}));
        outcomes.push_back(makeOutcome("breakpoints.multiple_lines", "breakpoints", "setBreakpoints", reply));
    }

    // 13. breakpoints.conditional
    {
        const std::vector<dap::SourceBreakpoint> bps{{.line = 42, .condition = "speed > 5"}};
        const auto reply = harness.request("setBreakpoints", dap::setBreakpointsArguments(scriptPath, bps));
        outcomes.push_back(makeOutcome("breakpoints.conditional", "breakpoints", "setBreakpoints", reply));
    }

    // 14. breakpoints.hit_condition
    {
        const std::vector<dap::SourceBreakpoint> bps{{.line = 42, .hitCondition = "3"}};
        const auto reply = harness.request("setBreakpoints", dap::setBreakpointsArguments(scriptPath, bps));
        outcomes.push_back(makeOutcome("breakpoints.hit_condition", "breakpoints", "setBreakpoints", reply));
    }

    // 15. breakpoints.logpoint
    {
        const std::vector<dap::SourceBreakpoint> bps{{.line = 42, .logMessage = "speed={speed}"}};
        const auto reply = harness.request("setBreakpoints", dap::setBreakpointsArguments(scriptPath, bps));
        outcomes.push_back(makeOutcome("breakpoints.logpoint", "breakpoints", "setBreakpoints", reply));
    }

    // 16. breakpoints.pausepoint
    {
        rpc::Json args =
            rpc::Json::object()
                .set("source", rpc::Json::object().set("path", scriptPath))
                .set("breakpoints", rpc::Json::array({rpc::Json::object().set("line", 42).set("mode", "pausepoint")}))
                .set("sourceModified", false);
        const auto reply = harness.request("setBreakpoints", std::move(args));
        outcomes.push_back(makeOutcome("breakpoints.pausepoint", "breakpoints", "setBreakpoints", reply));
    }

    // 17. breakpoints.function
    {
        const std::vector<dap::FunctionBreakpoint> bps{{.name = "PlayerController:OnUpdate"}};
        const auto reply = harness.request("setFunctionBreakpoints", dap::setFunctionBreakpointsArguments(bps));
        outcomes.push_back(makeOutcome("breakpoints.function", "breakpoints", "setFunctionBreakpoints", reply));
    }

    // 18. breakpoints.exception
    {
        const auto reply =
            harness.request("setExceptionBreakpoints", dap::setExceptionBreakpointsArguments({"lua_error"}));
        outcomes.push_back(makeOutcome("breakpoints.exception", "breakpoints", "setExceptionBreakpoints", reply));
    }

    // Re-pause the session so stepping, variables, and evaluate scenarios execute in a stopped state.
    harness.clearEvents();
    const auto repause = harness.request("pause", rpc::Json::object().set("threadId", 1));
    EXPECT_TRUE(repause.has_value());
    EXPECT_TRUE(harness.waitForEvent("stopped"));

    // 19. stepping.step_over
    {
        const auto reply = harness.request("next", rpc::Json::object().set("threadId", 1));
        outcomes.push_back(makeOutcome("stepping.step_over", "stepping", "next", reply));
    }

    // 20. stepping.step_in
    {
        const auto reply = harness.request("stepIn", rpc::Json::object().set("threadId", 1));
        outcomes.push_back(makeOutcome("stepping.step_in", "stepping", "stepIn", reply));
    }

    // 21. stepping.step_out
    {
        const auto reply = harness.request("stepOut", rpc::Json::object().set("threadId", 1));
        outcomes.push_back(makeOutcome("stepping.step_out", "stepping", "stepOut", reply));
    }

    // 22. variables.scopes
    {
        const auto reply = harness.request("scopes", rpc::Json::object().set("frameId", topFrameId));
        outcomes.push_back(makeOutcome("variables.scopes", "variables", "scopes", reply));
    }

    // 23. variables.locals
    {
        const auto reply = harness.request("variables", rpc::Json::object().set("variablesReference", 1));
        outcomes.push_back(makeOutcome("variables.locals", "variables", "variables", reply));
    }

    // 24. variables.upvalues
    {
        const auto reply = harness.request("variables", rpc::Json::object().set("variablesReference", 2));
        outcomes.push_back(makeOutcome("variables.upvalues", "variables", "variables", reply));
    }

    // 25. variables.tables
    {
        const auto reply = harness.request("variables", rpc::Json::object().set("variablesReference", 3));
        outcomes.push_back(makeOutcome("variables.tables", "variables", "variables", reply));
    }

    // 26. variables.set_variable
    {
        const auto reply = harness.request("setVariable", dap::setVariableArguments(1, "speed", "25"));
        outcomes.push_back(makeOutcome("variables.set_variable", "variables", "setVariable", reply));
    }

    // 27. evaluate.watch
    {
        const auto reply = harness.request(
            "evaluate",
            rpc::Json::object().set("expression", "speed * dt").set("frameId", topFrameId).set("context", "watch"));
        outcomes.push_back(makeOutcome("evaluate.watch", "evaluate", "evaluate", reply));
    }

    // 28. evaluate.repl
    {
        const auto reply = harness.request(
            "evaluate",
            rpc::Json::object().set("expression", "1 + 2").set("frameId", topFrameId).set("context", "repl"));
        outcomes.push_back(makeOutcome("evaluate.repl", "evaluate", "evaluate", reply));
    }

    // 29. coroutines.multi_coroutine_threads
    // Sets a breakpoint in a child coroutine script and checks that threads reports the child coroutine.
    {
        const std::string coroScript = test::workPath("Assets/Scripts/Gameplay/WorkerCoroutine.lua");
        const auto bpReply = harness.request("setBreakpoints", dap::setBreakpointsArguments(coroScript, {14}));
        if (!bpReply) {
            outcomes.push_back(makeOutcome("coroutines.multi_coroutine_threads", "coroutines", "threads", bpReply));
        } else {
            const auto threadsReply = harness.request("threads");
            const rpc::Json* list = threadsReply ? threadsReply->find("threads") : nullptr;
            const bool hasChildCoroutine = list != nullptr && list->size() >= 2;
            outcomes.push_back(makeOutcome("coroutines.multi_coroutine_threads", "coroutines", "threads", threadsReply,
                                           hasChildCoroutine, "threads did not report child coroutine threads"));
        }
    }

    // 30. lifecycle.terminate
    {
        const auto reply = harness.request("terminate", rpc::Json::object().set("restart", false));
        outcomes.push_back(makeOutcome("lifecycle.terminate", "lifecycle", "terminate", reply));
    }

    harness.client->close();
    return outcomes;
}

TEST(LuaDapConformance, RunsAllScenariosWithZeroSkippedAndMatchesBaseline) {
    const auto baselineBytes = io::realFileSystem().readFile(FROST_LUA_DAP_CONFORMANCE_BASELINE);
    ASSERT_TRUE(baselineBytes.has_value()) << baselineBytes.error().describe();
    const auto baselineJson = rpc::parseJson(*baselineBytes);
    ASSERT_TRUE(baselineJson.has_value()) << baselineJson.error().describe();
    ASSERT_TRUE(baselineJson->isObject());

    EXPECT_EQ(baselineJson->stringAt("schema"), "frost.lua-dap-conformance-baseline/1");
    EXPECT_EQ(baselineJson->stringAt("adapter"), "frost.lua-debug-adapter");
    EXPECT_EQ(baselineJson->stringAt("endpoint"), "frostengine.lua-debug-endpoint/experimental");

    const rpc::Json* baselineScenarios = baselineJson->find("scenarios");
    ASSERT_NE(baselineScenarios, nullptr);
    ASSERT_TRUE(baselineScenarios->isArray());

    const std::vector<ScenarioOutcome> live = runAllConformanceScenarios();
    ASSERT_EQ(live.size(), 30U);
    ASSERT_EQ(baselineScenarios->size(), live.size());

    std::int64_t skippedCount = 0;
    std::int64_t passedCount = 0;
    std::int64_t failedCount = 0;
    for (const ScenarioOutcome& item : live) {
        if (item.skipped) {
            ++skippedCount;
        } else if (item.passed) {
            ++passedCount;
        } else {
            ++failedCount;
        }
    }

    EXPECT_EQ(skippedCount, 0) << "LUA-11a requires zero skipped conformance scenarios";
    EXPECT_EQ(passedCount, 10);
    EXPECT_EQ(failedCount, 20);
    EXPECT_EQ(baselineJson->integerAt("totalCount"), static_cast<std::int64_t>(live.size()));
    EXPECT_EQ(baselineJson->integerAt("skippedCount"), skippedCount);
    EXPECT_EQ(baselineJson->integerAt("passedCount"), passedCount);
    EXPECT_EQ(baselineJson->integerAt("failedCount"), failedCount);

    for (std::size_t index = 0; index < live.size(); ++index) {
        const rpc::Json& expectedJson = baselineScenarios->items()[index];
        const ScenarioOutcome expected{
            .id = std::string(expectedJson.stringAt("id")),
            .category = std::string(expectedJson.stringAt("category")),
            .command = std::string(expectedJson.stringAt("command")),
            .passed = expectedJson.boolAt("passed").value_or(false),
            .skipped = false,
            .failureReason = std::string(expectedJson.stringAt("failureReason")),
        };
        const ScenarioOutcome& actual = live[index];
        EXPECT_EQ(actual, expected) << "Scenario mismatch at index " << index << " (" << actual.id << "): live={"
                                    << "category=" << actual.category << ", command=" << actual.command
                                    << ", passed=" << actual.passed << ", failureReason='" << actual.failureReason
                                    << "'} vs baseline={"
                                    << "category=" << expected.category << ", command=" << expected.command
                                    << ", passed=" << expected.passed << ", failureReason='" << expected.failureReason
                                    << "'}";
    }
}

TEST(LuaDapConformance, IndividualScenariosAreExecutableThroughDebugSession) {
    [[maybe_unused]] WinsockScope winsock;
    LoopbackLuaEngineServer server;
    test::SessionRecorder recorder;

    const std::string scriptPath = test::workPath("Assets/Scripts/Gameplay/PlayerController.lua");
    std::map<std::string, std::vector<BreakpointLine>> initialBreakpoints;
    initialBreakpoints.emplace(scriptPath, std::vector<BreakpointLine>{
                                               BreakpointLine{.id = 1, .line = 42},
                                               BreakpointLine{.id = 2, .line = 42, .condition = "speed > 5"},
                                               BreakpointLine{.id = 3, .line = 42, .logMessage = "speed={speed}"},
                                           });

    DebugSessionOptions options{
        .adapterId = "frost-lua",
        .targetMode = DebugTargetMode::Attach,
        .functionBreakpoints = {FunctionBreakpointLine{.id = 4, .name = "PlayerController:OnUpdate"}},
        .exceptionFilters = {"lua_error"},
    };

    auto factory = luaDebugAdapterFactory(server.port(), std::string(kToken), 5s);
    auto session = DebugSession::start(std::move(factory), rpc::Json::object(), std::move(initialBreakpoints),
                                       recorder.callbacks(), std::move(options));

    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Running; }, 5s));
    EXPECT_TRUE(session->capabilities().supportsConfigurationDoneRequest);
    EXPECT_FALSE(session->capabilities().supportsFunctionBreakpoints);

    // Line breakpoints and function breakpoints are reported unverified rather than hanging startup.
    ASSERT_TRUE(test::waitUntil(
        [&] {
            LockGuard lock(recorder.mutex);
            return recorder.breakpoints.size() >= 4 &&
                   recorder.output.find("Exception breakpoints could not be set:") != std::string::npos;
        },
        5s));
    {
        LockGuard lock(recorder.mutex);
        for (const BreakpointResult& bp : recorder.breakpoints) {
            EXPECT_FALSE(bp.verified) << bp.id;
            if (bp.id <= 3) {
                EXPECT_NE(bp.message.find("operation is not implemented by the Lua debugger"), std::string::npos)
                    << bp.message;
            } else {
                EXPECT_NE(bp.message.find("does not support function breakpoints"), std::string::npos) << bp.message;
            }
        }
    }

    // Pause -> Stopped, threads + stack frames + loaded Lua source retrieval work through DebugSession.
    ASSERT_TRUE(session->pause());
    ASSERT_TRUE(test::waitUntil(
        [&] { return session->state() == DebugState::Stopped && session->selectedFrame().has_value(); }, 5s));
    ASSERT_TRUE(session->stopInfo().has_value());
    EXPECT_EQ(session->stopInfo()->reason, "pause");

    const std::vector<DebugThread> threads = session->threads();
    ASSERT_EQ(threads.size(), 1U);
    EXPECT_EQ(threads.front(), (DebugThread{.id = 1, .name = "Lua"}));

    const FrameList frameList = session->frames(1);
    ASSERT_EQ(frameList.frames.size(), 3U);
    EXPECT_EQ(frameList.totalFrames, 3);
    const DebugFrame selected = *session->selectedFrame();
    EXPECT_EQ(selected.id, 101);
    EXPECT_EQ(selected.name, "PlayerController:OnUpdate");
    EXPECT_EQ(selected.sourceName, "PlayerController.lua");
    EXPECT_EQ(selected.line, 42);
    ASSERT_GT(selected.sourceReference, 0);

    {
        auto sourcePromise = std::make_shared<std::promise<Result<std::string>>>();
        auto sourceFuture = sourcePromise->get_future();
        session->source(selected.sourceReference,
                        [sourcePromise](Result<std::string> content) { sourcePromise->set_value(std::move(content)); });
        ASSERT_EQ(sourceFuture.wait_for(5s), std::future_status::ready);
        const Result<std::string> sourceContent = sourceFuture.get();
        ASSERT_TRUE(sourceContent.has_value()) << sourceContent.error().describe();
        EXPECT_EQ(*sourceContent, kPlayerScriptSource);
    }

    // Stepping requests fail cleanly and restore DebugSession to Stopped with the previous frame intact.
    ASSERT_TRUE(session->next());
    ASSERT_TRUE(test::waitUntil(
        [&] {
            LockGuard lock(recorder.mutex);
            return session->state() == DebugState::Stopped &&
                   recorder.output.find("next failed: next: operation is not implemented by the Lua debugger") !=
                       std::string::npos;
        },
        5s));
    ASSERT_TRUE(session->selectedFrame().has_value());
    EXPECT_EQ(session->selectedFrame()->id, 101);

    // Scopes, variables, setVariable, and evaluate report the adapter's exact failure message.
    {
        auto scopesPromise = std::make_shared<std::promise<Result<ScopeList>>>();
        auto scopesFuture = scopesPromise->get_future();
        session->scopes(selected.id,
                        [scopesPromise](Result<ScopeList> result) { scopesPromise->set_value(std::move(result)); });
        ASSERT_EQ(scopesFuture.wait_for(5s), std::future_status::ready);
        const Result<ScopeList> scopesResult = scopesFuture.get();
        ASSERT_FALSE(scopesResult.has_value());
        EXPECT_EQ(adapterErrorMessage(scopesResult.error(), "scopes"),
                  "operation is not implemented by the Lua debugger");
    }
    {
        auto varsPromise = std::make_shared<std::promise<Result<VariableList>>>();
        auto varsFuture = varsPromise->get_future();
        session->variables(1, 0, 0,
                           [varsPromise](Result<VariableList> result) { varsPromise->set_value(std::move(result)); });
        ASSERT_EQ(varsFuture.wait_for(5s), std::future_status::ready);
        const Result<VariableList> varsResult = varsFuture.get();
        ASSERT_FALSE(varsResult.has_value());
        EXPECT_EQ(adapterErrorMessage(varsResult.error(), "variables"),
                  "operation is not implemented by the Lua debugger");
    }
    {
        auto setVarPromise = std::make_shared<std::promise<Result<dap::Variable>>>();
        auto setVarFuture = setVarPromise->get_future();
        session->setVariable(1, "speed", "25", [setVarPromise](Result<dap::Variable> result) {
            setVarPromise->set_value(std::move(result));
        });
        ASSERT_EQ(setVarFuture.wait_for(5s), std::future_status::ready);
        const Result<dap::Variable> setVarResult = setVarFuture.get();
        ASSERT_FALSE(setVarResult.has_value());
        EXPECT_EQ(adapterErrorMessage(setVarResult.error(), "setVariable"),
                  "operation is not implemented by the Lua debugger");
    }
    {
        auto evalPromise = std::make_shared<std::promise<Result<dap::Variable>>>();
        auto evalFuture = evalPromise->get_future();
        session->evaluate("speed * dt", selected.id, "watch",
                          [evalPromise](Result<dap::Variable> result) { evalPromise->set_value(std::move(result)); });
        ASSERT_EQ(evalFuture.wait_for(5s), std::future_status::ready);
        const Result<dap::Variable> evalResult = evalFuture.get();
        ASSERT_FALSE(evalResult.has_value());
        EXPECT_EQ(adapterErrorMessage(evalResult.error(), "evaluate"),
                  "operation is not implemented by the Lua debugger");
    }

    ASSERT_TRUE(session->continueRun());
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Running; }, 5s));
    session->stop();
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Ended; }, 5s));
    ASSERT_TRUE(session->outcome().has_value());
    EXPECT_TRUE(session->outcome()->disconnectConfirmed);
}

TEST(LuaDapConformance, BaselineAndCorporaLockAreValidOnEveryPlatform) {
    const auto baselineBytes = io::realFileSystem().readFile(FROST_LUA_DAP_CONFORMANCE_BASELINE);
    ASSERT_TRUE(baselineBytes.has_value()) << baselineBytes.error().describe();
    const auto baseline = rpc::parseJson(*baselineBytes);
    ASSERT_TRUE(baseline.has_value()) << baseline.error().describe();

    const rpc::Json* scenarios = baseline->find("scenarios");
    ASSERT_NE(scenarios, nullptr);
    ASSERT_TRUE(scenarios->isArray());
    EXPECT_EQ(scenarios->size(), 30U);

    std::set<std::string> ids;
    std::set<std::string> categories;
    for (const rpc::Json& item : scenarios->items()) {
        const std::string id(item.stringAt("id"));
        const std::string category(item.stringAt("category"));
        const std::string command(item.stringAt("command"));
        const auto passed = item.boolAt("passed");
        const std::string failureReason(item.stringAt("failureReason"));
        EXPECT_FALSE(id.empty());
        EXPECT_FALSE(category.empty());
        EXPECT_FALSE(command.empty());
        ASSERT_TRUE(passed.has_value()) << id;
        EXPECT_TRUE(ids.insert(id).second) << "duplicate scenario id: " << id;
        categories.insert(category);
        if (*passed) {
            EXPECT_TRUE(failureReason.empty()) << id;
        } else {
            EXPECT_FALSE(failureReason.empty()) << id;
        }
    }
    EXPECT_EQ(categories, (std::set<std::string>{"breakpoints", "coroutines", "evaluate", "execution", "handshake",
                                                 "lifecycle", "source", "stack", "stepping", "threads", "variables"}));

    const auto lockBytes = io::realFileSystem().readFile(FROST_CORPORA_LOCK_PATH);
    ASSERT_TRUE(lockBytes.has_value()) << lockBytes.error().describe();
    const auto lockJson = rpc::parseJson(*lockBytes);
    ASSERT_TRUE(lockJson.has_value()) << lockJson.error().describe();
    const rpc::Json* corpora = lockJson->find("corpora");
    ASSERT_NE(corpora, nullptr);
    ASSERT_TRUE(corpora->isArray());

    std::string engineSha;
    for (const rpc::Json& entry : corpora->items()) {
        if (entry.stringAt("id") == "frostengine") {
            engineSha = std::string(entry.stringAt("sha"));
        }
    }
    ASSERT_EQ(engineSha.size(), 40U);
}

} // namespace
} // namespace frost::debug
