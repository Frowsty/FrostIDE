// SPDX-License-Identifier: Apache-2.0
// DBG-11 acceptance through the real Workbench, pinned engine and pinned native/Lua DAPs.
// The long native all-stop preserves an already captured real Lua pause beyond R6's heartbeat
// timeout. Every frame, process identity, pause generation and write count comes from its tool.
#include "frost/debug/breakpoints.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/lang/uri.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/mixed_debug_window.hpp"
#include "frost/ui/run_controller.hpp"

#include "../app/workbench_rig.hpp"
#include "support/real_engine_session.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>

#include <unistd.h>
#endif

namespace frost::app {
namespace {
using namespace std::chrono_literals;
using rpc::Json;

constexpr std::string_view kScript = R"(-- SPDX-License-Identifier: Apache-2.0
local Mixed = {}

local function Capture(probe)
    local serial = probe.serial -- FROST_MIXED_LUA_PAUSE
    return serial + 1
end

function Mixed:OnCreate()
    self.serial = 17
end

function Mixed:OnUpdate(dt)
    self.answer = Capture(self)
    self.elapsed = (self.elapsed or 0) + dt
end

return Mixed
)";
constexpr auto kStartTimeout = 20min; // The pinned Debug engine's native symbols can exceed 1 GiB.
constexpr auto kOperationTimeout = 2min;

bool pumpUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

bool processAlive(std::int64_t pid) {
#ifdef _WIN32
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

using FrameKey = std::tuple<std::int64_t, std::string, std::string, std::int64_t, std::int64_t, std::int64_t>;
std::vector<FrameKey> frameKeys(const debug::FrameList& list) {
    std::vector<FrameKey> result;
    for (const auto& frame : list.frames)
        result.emplace_back(frame.id, frame.name, frame.path, frame.line, frame.column, frame.sourceReference);
    return result;
}

std::int64_t markerLine() {
    const auto offset = kScript.find("-- FROST_MIXED_LUA_PAUSE");
    if (offset == std::string_view::npos ||
        kScript.find("-- FROST_MIXED_LUA_PAUSE", offset + 1) != std::string_view::npos)
        return 0;
    return 1 + std::ranges::count(kScript.substr(0, offset), '\n');
}

class EngineMixedRealCorpus : public testing::Test {
protected:
    void SetUp() override {
        const auto prepared = build.prepare({"FrostEditor"});
        ASSERT_TRUE(prepared) << prepared.error().describe();
        const auto adapter = debug::findLldbDap(io::realFileSystem(), debug::defaultLldbDapSearch());
        ASSERT_TRUE(adapter) << "mandatory pinned LLDB DAP missing: " << adapter.error().describe();
        project = directory.join("project");
        const auto copied = build.copyProject(project);
        ASSERT_TRUE(copied) << copied.error().describe();
        const auto installed = frost::test::installAcceptanceScript(project, kScript);
        ASSERT_TRUE(installed) << installed.error().describe();
        source = project + "/Assets/Scripts/FeatureTest/Hero.lua";
        auto image = io::toStdPath(build.program);
        ASSERT_TRUE(image);
#ifdef _WIN32
        if (image->extension() == ".com")
            image->replace_extension(".exe");
#endif
        nativeImage = io::fromStdPath(*image).value_or("");
        ASSERT_TRUE(io::realFileSystem().exists(nativeImage).value_or(false));
        test::RigOptions options;
        options.disableProjectEvaluation = true;
        options.workbench.engineAutomationDirectory = directory.join("config/Automation");
        host = std::make_unique<test::WorkbenchRig>(directory, options);
        ASSERT_TRUE(host->trust->trust(project));
        ASSERT_TRUE(host->trust->trust(build.corpus));
    }

    void TearDown() override {
        if (host) {
            if (auto* registry = workbench().debugSessions()) {
                registry->stopAll();
                EXPECT_TRUE(pumpUntil([&] { return !registry->anyDebugging(); }, kOperationTimeout));
            }
            workbench().disconnectEngineSession();
            workbench().shutdown();
            host.reset();
        }
        external.reset(); // Its original contained process handle owns fixture cleanup.
        if (nativeOwned && processId > 0)
            EXPECT_TRUE(pumpUntil([&] { return !processAlive(processId); }, kOperationTimeout))
                << "the exact launched native engine survived owned-session cleanup";
    }

    Workbench& workbench() { return *host->workbench; }

    void openProject() {
        const auto opened = workbench().openExternalProject(project);
        ASSERT_TRUE(opened) << opened.error().describe();
        ASSERT_TRUE(
            pumpUntil([&] { return workbench().engineState().boolAt("available") == true; }, kOperationTimeout));
        const auto configured = workbench().configureEngineTools(build.program, directory.join("config"),
                                                                 directory.join("config/Automation"));
        ASSERT_TRUE(configured) << configured.error().describe();
        ASSERT_TRUE(pumpUntil([&] { return workbench().engineState().boolAt("physicalRootValid") == true; },
                              kOperationTimeout));
        ASSERT_NE(workbench().debugController(), nullptr);
        ASSERT_GT(markerLine(), 0);
        const auto breakpoint = workbench().debugController()->breakpoints().add(source, markerLine());
        ASSERT_TRUE(breakpoint) << breakpoint.error().describe();
        lineBreakpoint = *breakpoint;
    }

    ui::DebugController* luaController() {
        auto* registry = workbench().debugSessions();
        if (!registry)
            return nullptr;
        ui::DebugController* found = nullptr;
        for (const auto& entry : registry->sessions())
            if (entry.controller->session() && entry.controller->session()->adapterId() == "frost-lua") {
                if (found != nullptr)
                    return nullptr;
                found = entry.controller;
            }
        return found;
    }

    void waitForLuaBreakpoint() {
        // The product may first stop at entry. Resume those actual entry stops so play.start can
        // finish before the OnUpdate breakpoint and the long native heartbeat suspension.
        for (unsigned entries = 0; entries != 8; ++entries) {
            ASSERT_TRUE(pumpUntil(
                [&] {
                    auto* lua = luaController();
                    return lua && lua->session() && lua->session()->state() == debug::DebugState::Stopped &&
                           lua->selectedFrame().has_value();
                },
                kStartTimeout))
                << workbench().engineState().dump();
            auto* lua = luaController();
            const auto frame = lua->selectedFrame();
            if (frame->line == markerLine() && lang::documentKey(frame->path) == lang::documentKey(source)) {
                ASSERT_EQ(lua->stopInfo()->reason, "breakpoint");
                ASSERT_TRUE(lua->stopInfo()->scriptPause.has_value());
                ASSERT_GT(lua->stopInfo()->scriptPause->vmGeneration, 0U);
                ASSERT_GT(lua->stopInfo()->scriptPause->stopGeneration, 0U);
                ASSERT_GT(lua->stopInfo()->scriptPause->osThreadId, 0U);
                ASSERT_TRUE(workbench().debugController()->breakpoints().remove(lineBreakpoint));
                ASSERT_TRUE(pumpUntil(
                    [&] {
                        const auto state = workbench().engineState();
                        return state.unsignedAt("pendingRequestCount") == 0 && state.stringAt("state") == "Play" &&
                               state.stringAt("phase") == "playing" && state.stringAt("playGeneration").size() == 32;
                    },
                    kOperationTimeout));
                return;
            }
            ASSERT_EQ(lua->stopInfo()->reason, "entry") << frame->name << " at " << frame->path << ':' << frame->line;
            ASSERT_TRUE(lua->resume());
        }
        FAIL() << "the actual Lua breakpoint was never reached after entry stops";
    }

    void captureAndResume(bool holdBeyondHeartbeat) {
        host->show();
        host->window->toolWindows().show("debug", false);
        ASSERT_TRUE(pumpUntil([&] { return workbench().mixedDebugSnapshot().canCapture; }, kStartTimeout))
            << workbench().mixedDebugSnapshot().status.toStdString();
        auto* lua = luaController();
        ASSERT_NE(lua, nullptr);
        ASSERT_TRUE(lua->session());
        const auto luaGeneration = lua->sessionGeneration();
        const auto originalStop = lua->session()->stopInfo();
        ASSERT_TRUE(originalStop && originalStop->scriptPause && originalStop->threadId);
        const auto originalFrames = frameKeys(lua->session()->frames(*originalStop->threadId));
        ASSERT_FALSE(originalFrames.empty());
        // Drain real initial scopes/breakpoint replies before recording the outgoing write oracle.
        const auto settle = std::chrono::steady_clock::now() + 1s;
        ASSERT_TRUE(pumpUntil([&] { return std::chrono::steady_clock::now() >= settle; }, 2s));
        const auto writes = lua->session()->sentMessageCount();
        ASSERT_TRUE(writes.has_value()) << "real Lua DAP has no factual successful-write counter";
        const auto captured = workbench().captureEngineNativeStack();
        ASSERT_TRUE(captured) << captured.error().describe();
        ASSERT_TRUE(pumpUntil([&] { return workbench().mixedDebugSnapshot().canResume; }, kStartTimeout))
            << workbench().mixedDebugSnapshot().status.toStdString();
        const auto combined = workbench().mixedDebugSnapshot();
        ASSERT_TRUE(combined.lua && combined.native && combined.pause);
        ASSERT_NE(combined.lua->sessionId, combined.native->sessionId);
        ASSERT_EQ(combined.pause, originalStop->scriptPause);
        ASSERT_EQ(frameKeys(combined.lua->frames), originalFrames);
        ASSERT_FALSE(combined.native->frames.frames.empty());
        ASSERT_NO_FATAL_FAILURE(checkShownStack(*combined.native, "mixedNativeStack"));
        ASSERT_NO_FATAL_FAILURE(checkShownStack(*combined.lua, "mixedLuaStack"));
        auto* native = workbench().debugSessions()->controller(combined.native->sessionId);
        ASSERT_NE(native, nullptr);
        ASSERT_TRUE(native->nativeOsThreadIdsVerified());
        ASSERT_TRUE(native->session() && native->session()->stopInfo());
        const auto originalNativeFrames = frameKeys(native->session()->frames(combined.native->threadId));
        ASSERT_FALSE(originalNativeFrames.empty());
        ASSERT_EQ(frameKeys(combined.native->frames), originalNativeFrames);
        ASSERT_EQ(native->session()->targetMode(),
                  nativeOwned ? debug::DebugTargetMode::Launch : debug::DebugTargetMode::Attach);
        ASSERT_EQ(lua->session()->targetMode(), debug::DebugTargetMode::Attach);
        ASSERT_TRUE(native->session()->stopInfo()->allThreadsStopped);
        ASSERT_TRUE(native->adapterStoppedEvidence());
        ASSERT_EQ(native->session()->processId(), combined.processId);
        ASSERT_EQ(workbench().engineState().integerAt("processId"), combined.processId);
        ASSERT_EQ(combined.native->threadId, static_cast<std::int64_t>(combined.pause->osThreadId));
        ASSERT_TRUE(std::ranges::any_of(native->session()->threads(),
                                        [&](const auto& thread) { return thread.id == combined.native->threadId; }));
        ASSERT_TRUE(std::ranges::any_of(combined.native->frames.frames, [](const auto& frame) {
            return frame.name.find("ScriptDebugger") != std::string::npos;
        })) << "the native stack did not contain the real Lua debugger hook";
        processId = combined.processId;
        const auto nativeGeneration = native->sessionGeneration();
        const auto nativeEvidence = native->adapterEvidenceSequence();
        const auto state = workbench().engineState();
        ASSERT_EQ(state.boolAt("connected"), true);
        ASSERT_EQ(state.boolAt("nativeDebuggerMatched"), true);
        ASSERT_EQ(state.boolAt("debuggerPaused"), true);
        ASSERT_EQ(lua->session()->sentMessageCount(), writes);
        const auto heartbeat = state.unsignedAt("heartbeatTimeoutMs").value_or(0);
        ASSERT_GT(heartbeat, 0U);
        ASSERT_LT(heartbeat, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 2000));
        const auto hold =
            holdBeyondHeartbeat
                ? std::max(61s,
                           std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds(heartbeat)) + 2s)
                : 1s;
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + hold;
        while (std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            const auto live = workbench().mixedDebugSnapshot();
            ASSERT_TRUE(live.canResume && live.lua && live.native && live.pause);
            ASSERT_EQ(live.pause, combined.pause);
            ASSERT_EQ(frameKeys(live.lua->frames), originalFrames);
            ASSERT_EQ(frameKeys(lua->session()->frames(*originalStop->threadId)), originalFrames);
            ASSERT_EQ(lua->session()->state(), debug::DebugState::Stopped);
            ASSERT_TRUE(lua->session()->stopInfo());
            ASSERT_EQ(lua->session()->stopInfo()->scriptPause, originalStop->scriptPause);
            ASSERT_EQ(frameKeys(live.native->frames), originalNativeFrames);
            ASSERT_EQ(frameKeys(native->session()->frames(combined.native->threadId)), originalNativeFrames);
            ASSERT_EQ(lua->sessionGeneration(), luaGeneration);
            ASSERT_EQ(native->sessionGeneration(), nativeGeneration);
            ASSERT_EQ(native->adapterEvidenceSequence(), nativeEvidence);
            ASSERT_TRUE(native->adapterStoppedEvidence());
            ASSERT_EQ(lua->session()->sentMessageCount(), writes);
            const auto observed = workbench().engineState();
            ASSERT_EQ(observed.boolAt("connected"), true);
            ASSERT_EQ(observed.integerAt("processId"), combined.processId);
            ASSERT_EQ(observed.unsignedAt("connectionGeneration"), state.unsignedAt("connectionGeneration"));
            ASSERT_EQ(observed.stringAt("serverGeneration"), state.stringAt("serverGeneration"));
            ASSERT_EQ(observed.stringAt("operationId"), state.stringAt("operationId"));
            ASSERT_EQ(observed.stringAt("phase"), "playing");
            ASSERT_EQ(observed.stringAt("playGeneration"), state.stringAt("playGeneration"));
            ASSERT_EQ(observed.unsignedAt("frame"), state.unsignedAt("frame"));
            ASSERT_EQ(observed.unsignedAt("sentCommandCount"), state.unsignedAt("sentCommandCount"));
            QThread::msleep(10);
        }
        if (holdBeyondHeartbeat) {
            const auto elapsed = std::chrono::steady_clock::now() - started;
            ASSERT_GT(elapsed, 60s);
            ASSERT_GT(elapsed, std::chrono::milliseconds(heartbeat));
        }
        std::vector<char> continuationOrder;
        const auto nativeConnection = QObject::connect(native, &ui::DebugController::adapterContinued, native,
                                                       [&](std::uint64_t) { continuationOrder.push_back('N'); });
        const auto luaConnection = QObject::connect(lua, &ui::DebugController::adapterContinued, lua,
                                                    [&](std::uint64_t) { continuationOrder.push_back('L'); });
        const auto resumed = workbench().resumeEngineMixedDebug();
        EXPECT_TRUE(resumed) << resumed.error().describe();
        const bool continued =
            resumed && pumpUntil([&] { return std::ranges::find(continuationOrder, 'L') != continuationOrder.end(); },
                                 kOperationTimeout);
        QObject::disconnect(nativeConnection);
        QObject::disconnect(luaConnection);
        ASSERT_TRUE(continued) << workbench().mixedDebugSnapshot().status.toStdString();
        const auto n = std::ranges::find(continuationOrder, 'N');
        const auto l = std::ranges::find(continuationOrder, 'L');
        ASSERT_NE(n, continuationOrder.end());
        ASSERT_LT(n, l) << "Lua continued before actual native continuation";
        ASSERT_TRUE(
            pumpUntil([&] { return workbench().engineState().boolAt("connected") == true; }, kOperationTimeout));
    }

    void checkShownStack(const ui::MixedDebugStack& actual, const char* objectName) {
        auto* tree = host->window->findChild<QTreeWidget*>(QString::fromUtf8(objectName));
        ASSERT_NE(tree, nullptr);
        ASSERT_EQ(tree->topLevelItemCount(), static_cast<int>(actual.frames.frames.size()));
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            const auto* row = tree->topLevelItem(i);
            const auto& frame = actual.frames.frames[static_cast<std::size_t>(i)];
            ASSERT_NE(row, nullptr);
            EXPECT_EQ(row->text(0).toStdString(), frame.name);
            EXPECT_EQ(row->data(0, Qt::UserRole).toULongLong(), actual.sessionId);
            EXPECT_EQ(row->data(0, Qt::UserRole + 1).toLongLong(), actual.threadId);
            EXPECT_EQ(row->data(0, Qt::UserRole + 2).toLongLong(), frame.id);
        }
    }

    frost::test::RealEngineBuild build;
    test::WorkDir directory{"engine-mixed-real"};
    std::unique_ptr<test::WorkbenchRig> host;
    std::unique_ptr<frost::test::RealEditor> external;
    std::string project, source, nativeImage;
    debug::BreakpointId lineBreakpoint = 0;
    std::int64_t processId = 0;
    bool nativeOwned = false;
};

TEST_F(EngineMixedRealCorpus, NativeOwnedLaunchRetainsTheSameLuaPauseBeyondHeartbeatAndContinuesInOrder) {
    run::RunConfiguration configuration;
    configuration.id = "corpus.engine.mixed";
    configuration.name = "Pinned engine C++ and Lua";
    configuration.type = run::RunType::FrostEditor;
    configuration.engineDebugMode = run::EngineDebugMode::NativeAndLua;
    configuration.program = nativeImage;
    configuration.engineProjectPath = project;
    configuration.workingDirectory = project;
    configuration.arguments = {"--offscreen", "--size", "320x240", "--scene", "Assets/Tests/FeatureTest.fscene"};
    const std::array configurations{configuration};
    const auto saved = run::saveWorkspaceRunConfigurations(io::realFileSystem(), project, configurations);
    ASSERT_TRUE(saved) << saved.error().describe();
    ASSERT_NO_FATAL_FAILURE(openProject());
    ASSERT_NE(workbench().runController(), nullptr);
    const auto selected = workbench().runController()->selectConfiguration(configuration.id);
    ASSERT_TRUE(selected) << selected.error().describe();
    nativeOwned = true;
    // The ordinary Debug action routes this explicit typed member through native launch before
    // the authenticated Lua dependent. Linux therefore traces the real LLDB-owned child.
    const auto started = workbench().debugController()->debug();
    ASSERT_TRUE(started) << started.error().describe();
    ASSERT_NO_FATAL_FAILURE(waitForLuaBreakpoint());
    processId = workbench().engineState().integerAt("processId").value_or(0);
    ASSERT_GT(processId, 0);
    ASSERT_NO_FATAL_FAILURE(captureAndResume(true));
    workbench().debugSessions()->stopAll();
    ASSERT_TRUE(pumpUntil([&] { return !workbench().debugSessions()->anyDebugging(); }, kOperationTimeout));
    ASSERT_TRUE(pumpUntil([&] { return !processAlive(processId); }, kOperationTimeout))
        << "the actual native-owned engine survived Stop All";
}

#ifdef _WIN32
TEST_F(EngineMixedRealCorpus, ExternalEditorKeepsItsActualProcessAfterOrdinaryMixedDetach) {
    external = std::make_unique<frost::test::RealEditor>();
    const auto endpoint = external->start(build, project, directory.join("config"));
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    ASSERT_NO_FATAL_FAILURE(openProject());
    ASSERT_TRUE(workbench().refreshEngineInstances());
    ASSERT_TRUE(pumpUntil(
        [&] {
            const auto state = workbench().engineState();
            const auto* instances = state.find("instances");
            return instances && std::ranges::any_of(instances->items(), [&](const Json& instance) {
                       return instance.stringAt("instanceId") == endpoint->instanceId;
                   });
        },
        kOperationTimeout));
    ASSERT_TRUE(workbench().pairEngineInstance(endpoint->instanceId));
    ASSERT_TRUE(pumpUntil([&] { return workbench().engineState().boolAt("connected") == true; }, kOperationTimeout));
    auto request =
        workbench().sendEngineCommand("scene.open", Json::object().set("path", "Assets/Tests/FeatureTest.fscene"));
    ASSERT_TRUE(request) << request.error().describe();
    ASSERT_TRUE(pumpUntil([&] { return workbench().engineState().unsignedAt("lastCompletedRequestId") == *request; },
                          kOperationTimeout));
    ASSERT_TRUE(workbench().engineState().stringAt("lastError").empty());
    const auto started = workbench().startEngineLuaDebug(true);
    ASSERT_TRUE(started) << started.error().describe();
    ASSERT_NO_FATAL_FAILURE(waitForLuaBreakpoint());
    auto* lua = luaController();
    ASSERT_NE(lua, nullptr);
    processId = workbench().engineState().integerAt("processId").value_or(0);
    ASSERT_GT(processId, 0);
    const auto captured = workbench().engineState();
    const auto revision = host->trust->snapshot(workbench().projectRoot()).revision;
    const auto root = workbench().projectRoot();
    const auto luaGeneration = lua->sessionGeneration();
    debug::DebugTarget target;
    target.name = "Pinned FrostEditor external native attach";
    target.program = nativeImage;
    target.workingDirectory = root;
    target.trustRoots = {root, build.corpus};
    const auto attached = workbench().debugSessions()->attachTarget(
        target, {},
        Json::object()
            .set("type", "lldb-dap")
            .set("request", "attach")
            .set("program", nativeImage)
            .set("pid", processId),
        [&, root, revision, captured, lua, luaGeneration] {
            const auto trust = host->trust->snapshot(root);
            const auto current = workbench().engineState();
            return workbench().projectRoot() == root && trust.trusted && trust.revision == revision &&
                   current.boolAt("connected") == true &&
                   current.stringAt("selectedInstanceId") == captured.stringAt("selectedInstanceId") &&
                   current.stringAt("serverGeneration") == captured.stringAt("serverGeneration") &&
                   current.unsignedAt("connectionGeneration") == captured.unsignedAt("connectionGeneration") &&
                   current.integerAt("processId") == processId && lua->sessionGeneration() == luaGeneration &&
                   lua->session();
        });
    ASSERT_TRUE(attached) << attached.error().describe();
    ASSERT_NO_FATAL_FAILURE(captureAndResume(false));
    workbench().debugSessions()->stopAll();
    ASSERT_TRUE(pumpUntil([&] { return !workbench().debugSessions()->anyDebugging(); }, kOperationTimeout));
    ASSERT_TRUE(processAlive(processId)) << "ordinary attach detach terminated the external editor";
    ASSERT_TRUE(pumpUntil(
        [&] {
            const auto state = workbench().engineState();
            const auto phase = state.stringAt("phase");
            return state.boolAt("connected") == true && state.stringAt("state") == "Edit" &&
                   state.stringAt("operationId") == captured.stringAt("operationId") &&
                   (phase == "completed" || phase == "cancelled" || phase == "failed");
        },
        kOperationTimeout));
    request = workbench().sendEngineCommand("project.info", Json::object());
    ASSERT_TRUE(request) << request.error().describe();
    ASSERT_TRUE(pumpUntil([&] { return workbench().engineState().unsignedAt("lastCompletedRequestId") == *request; },
                          kOperationTimeout));
    ASSERT_TRUE(workbench().engineState().stringAt("lastError").empty());
    ASSERT_TRUE(processAlive(processId));
}
#endif
} // namespace
} // namespace frost::app
