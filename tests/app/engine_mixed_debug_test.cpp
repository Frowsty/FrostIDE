// SPDX-License-Identifier: Apache-2.0
// Protocol-double regressions for ordering/identity; real engine+LLDB acceptance is separate.
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/mixed_debug_window.hpp"

#include "debug/debug_test_util.hpp"
#include "engine_mixed_debug.hpp"
#include "workbench_rig.hpp"
#include <QPushButton>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <memory>
#include <vector>
namespace frost::app {
namespace {
using namespace std::chrono_literals;
class EngineMixedDebugTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory = std::make_unique<test::WorkDir>("mixed-wire");
        directory->write("workspace/native.cpp", "void native() {}\n");
        directory->write("workspace/script.lua", "function onCreate() end\n");
        root = directory->join("workspace");
        nativePeer = std::make_unique<debug::FakeDebugAdapter>();
        luaPeer = std::make_unique<debug::FakeDebugAdapter>();
        debug::scriptProgram(*nativePeer, debug::FakeProgram{.sourcePath = directory->join("workspace/native.cpp"),
                                                             .entryLine = 1,
                                                             .runsForever = true});
        debug::scriptProgram(*luaPeer, debug::FakeProgram{.sourcePath = directory->join("workspace/script.lua"),
                                                          .entryLine = 1,
                                                          .runsForever = true});
        nativePeer->on("threads", [](const rpc::Json&) {
            return debug::FakeDebugAdapter::Reply{
                .body = rpc::Json::object().set(
                    "threads",
                    rpc::Json::array().push(rpc::Json::object().set("id", 9132).set("name", "OS execution thread")))};
        });
        nativePeer->on("pause", [](const rpc::Json&) {
            return debug::FakeDebugAdapter::Reply{
                .events = {
                    {"stopped",
                     rpc::Json::object().set("reason", "pause").set("threadId", 9132).set("allThreadsStopped", true)}}};
        });
        nativePeer->on("continue", [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{}; });
        luaPeer->on("threads", [](const rpc::Json&) {
            return debug::FakeDebugAdapter::Reply{
                .body = rpc::Json::object().set(
                    "threads", rpc::Json::array().push(rpc::Json::object().set("id", 7).set("name", "Lua coroutine")))};
        });
        luaPeer->on("attach", [](const rpc::Json&) {
            return debug::FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json::object()}}};
        });
        test::RigOptions options;
        options.workbench.debugAdapterFactory = debug::test::fakeFactory(*nativePeer);
        rig = std::make_unique<test::WorkbenchRig>(*directory, std::move(options));
        ASSERT_TRUE(rig->trust->trust(root));
        ASSERT_TRUE(rig->workbench->openPath(root));
        auto& registry = *rig->workbench->debugSessions();
        const debug::DebugTarget target{
            .name = "Engine", .program = directory->join("workspace/Engine.exe"), .trustRoots = {root}};
        auto nativeId = registry.startTarget(target);
        ASSERT_TRUE(nativeId);
        auto luaId = registry.createSession(QStringLiteral("Lua"));
        ASSERT_TRUE(luaId);
        auto* lua = registry.controller(*luaId);
        ASSERT_TRUE(lua->startAdapterTarget(target, debug::test::fakeFactory(*luaPeer), rpc::Json::object(),
                                            "frost-lua", debug::DebugTargetMode::Attach));
        nativeBinding.target = {.pid = 4321,
                                .instanceId = "engine-instance",
                                .serverGeneration = "engine-generation",
                                .projectRoot = root,
                                .connectionGeneration = 5,
                                .sessionEpoch = 8,
                                .trustRevision = 12};
        nativeBinding.registrySessionId = *nativeId;
        nativeBinding.controller = registry.controller(*nativeId);
        nativeBinding.debugGeneration = nativeBinding.controller->sessionGeneration();
        luaBinding.registrySessionId = *luaId;
        luaBinding.controllerGeneration = lua->sessionGeneration();
        luaBinding.identity = {.projectPath = root,
                               .instanceId = "engine-instance",
                               .generation = "engine-generation",
                               .debugSessionId = "private-session",
                               .processId = 4321,
                               .connectionGeneration = 5};
        luaBinding.root = root;
        luaBinding.epoch = 3;
        luaBinding.sessionEpoch = 8;
        luaBinding.trustRevision = 12;
        luaBinding.current = [this] { return ownerCurrent; };
        nativePeer->sendEvent("process", rpc::Json::object().set("systemProcessId", 4321));
        ASSERT_TRUE(test::waitFor(
            [&] {
                return nativeBinding.controller->session()->processId() == 4321 &&
                       nativeBinding.controller->session()->state() == debug::DebugState::Running &&
                       lua->session()->state() == debug::DebugState::Running;
            },
            10s));
        coordinator = std::make_unique<EngineMixedDebugCoordinator>(
            registry,
            [this](std::uint64_t id) -> std::optional<EngineLuaDebugBinding> {
                return id == luaBinding.registrySessionId && ownerCurrent ? std::optional(luaBinding) : std::nullopt;
            },
            [this]() -> std::optional<EngineNativeDebugBinding> {
                return ownerCurrent ? std::optional(nativeBinding) : std::nullopt;
            },
            [this](const auto&) { return mappingVerified; });
        pauseLua(true);
        ASSERT_TRUE(test::waitFor([&] { return coordinator->snapshot().canCapture; }, 10s));
    }
    void TearDown() override {
        coordinator.reset();
        rig.reset(); // Its scoped retirement drain precedes destruction of these test peers.
    }
    void pauseLua(bool metadata) {
        auto event = rpc::Json::object().set("reason", "breakpoint").set("threadId", 7);
        if (metadata)
            event.set("vmGeneration", std::uint64_t{11})
                .set("osThreadId", std::uint64_t{9132})
                .set("stopGeneration", std::uint64_t{22});
        luaPeer->sendEvent("stopped", std::move(event));
    }
    void capture() {
        ASSERT_TRUE(coordinator->captureNativeStack());
        ASSERT_TRUE(test::waitFor([&] { return coordinator->snapshot().canResume; }, 10s));
    }
    std::unique_ptr<test::WorkDir> directory;
    std::unique_ptr<debug::FakeDebugAdapter> nativePeer, luaPeer;
    std::unique_ptr<test::WorkbenchRig> rig;
    std::unique_ptr<EngineMixedDebugCoordinator> coordinator;
    EngineNativeDebugBinding nativeBinding;
    EngineLuaDebugBinding luaBinding;
    std::string root;
    bool ownerCurrent = true, mappingVerified = true; // Explicit double admission, never product provenance.
};

TEST_F(EngineMixedDebugTest, CapturesActualWireStacksAndWaitsForNativeContinuedBeforeLuaContinue) {
    EXPECT_FALSE(
        nativeBinding.controller->nativeOsThreadIdsVerified()); // The custom test factory has no product proof.
    const auto luaStackRequests = luaPeer->argumentsOf("stackTrace").size();
    capture();
    const auto snapshot = coordinator->snapshot();
    ASSERT_TRUE(snapshot.pause);
    ASSERT_TRUE(snapshot.native);
    ASSERT_TRUE(snapshot.lua);
    EXPECT_EQ(snapshot.processId, 4321);
    EXPECT_EQ(snapshot.pause->vmGeneration, 11U);
    EXPECT_EQ(snapshot.pause->stopGeneration, 22U);
    EXPECT_EQ(snapshot.pause->osThreadId, 9132U);
    EXPECT_EQ(snapshot.native->threadId, 9132);
    EXPECT_EQ(snapshot.lua->threadId, 7);
    EXPECT_EQ(snapshot.native->frames.frames.front().path, directory->join("workspace/native.cpp"));
    EXPECT_EQ(snapshot.lua->frames.frames.front().path, directory->join("workspace/script.lua"));
    EXPECT_EQ(luaPeer->argumentsOf("stackTrace").size(), luaStackRequests);
    ui::MixedDebugWindow window;
    window.setSnapshot(snapshot);
    EXPECT_GT(window.findChild<QTreeWidget*>(QStringLiteral("mixedNativeStack"))->topLevelItemCount(), 0);
    EXPECT_GT(window.findChild<QTreeWidget*>(QStringLiteral("mixedLuaStack"))->topLevelItemCount(), 0);
    EXPECT_TRUE(window.findChild<QPushButton*>(QStringLiteral("mixedDebugContinue"))->isEnabled());
    EXPECT_FALSE(coordinator->selectFrame(luaBinding.registrySessionId, 7, snapshot.lua->frames.frames.front().id));
    const auto evidence = nativeBinding.controller->adapterEvidenceSequence();
    ASSERT_TRUE(coordinator->resumeBoth());
    ASSERT_TRUE(nativePeer->waitForRequest("continue"));
    ASSERT_TRUE(test::waitFor([&] { return !nativeBinding.controller->paused(); }, 10s));
    EXPECT_TRUE(nativeBinding.controller->adapterStoppedEvidence());
    EXPECT_EQ(nativeBinding.controller->adapterEvidenceSequence(), evidence);
    EXPECT_TRUE(luaPeer->argumentsOf("continue").empty());
    EXPECT_FALSE(coordinator->snapshot().native);
    EXPECT_FALSE(coordinator->snapshot().lua);
    nativePeer->sendEvent("continued", rpc::Json::object().set("threadId", 9132).set("allThreadsContinued", true));
    ASSERT_TRUE(test::waitFor([&] { return !luaPeer->argumentsOf("continue").empty(); }, 10s));
    EXPECT_FALSE(nativeBinding.controller->adapterStoppedEvidence());
}

TEST_F(EngineMixedDebugTest, SamePidWithReplacedConnectionCannotInheritTheCombinedPause) {
    capture();
    ++nativeBinding.target.connectionGeneration;
    ++luaBinding.identity.connectionGeneration;
    coordinator->refresh();
    EXPECT_FALSE(coordinator->snapshot().native);
    EXPECT_FALSE(coordinator->snapshot().lua);
    EXPECT_FALSE(coordinator->snapshot().canResume);
    EXPECT_FALSE(coordinator->resumeBoth());
    EXPECT_TRUE(luaPeer->argumentsOf("continue").empty());
    ownerCurrent = false;
    coordinator->refresh();
    EXPECT_FALSE(coordinator->captureNativeStack());
}

TEST_F(EngineMixedDebugTest, ANewNativeStopBeforeDeferredLuaContinueCannotResumeLua) {
    capture();
    ASSERT_TRUE(coordinator->resumeBoth());
    ASSERT_TRUE(nativePeer->waitForRequest("continue"));
    // Both actual events arrive before GUI delivery; Running presentation or a continue ACK
    // cannot override the newer native all-stop observed by the session worker.
    nativePeer->sendEvent("continued", rpc::Json::object().set("threadId", 9132));
    nativePeer->sendEvent(
        "stopped",
        rpc::Json::object().set("reason", "breakpoint").set("threadId", 9132).set("allThreadsStopped", true));
    ASSERT_TRUE(debug::test::waitUntil([&] {
        const auto stop = nativeBinding.controller->session()->stopInfo();
        return stop && stop->reason == "breakpoint";
    }));
    ASSERT_TRUE(test::waitFor(
        [&] {
            return nativeBinding.controller->adapterStoppedEvidence() &&
                   coordinator->snapshot().status.contains(QStringLiteral("no longer current"));
        },
        10s));
    EXPECT_TRUE(luaPeer->argumentsOf("continue").empty());
    EXPECT_FALSE(coordinator->snapshot().canResume);
}

TEST_F(EngineMixedDebugTest, UnknownMappingAndMissingActualOsThreadMetadataRefuseWithoutNativePause) {
    mappingVerified = false;
    coordinator->refresh();
    EXPECT_FALSE(coordinator->snapshot().canCapture);
    EXPECT_TRUE(coordinator->snapshot().status.contains(QStringLiteral("verified pinned LLDB")));
    EXPECT_FALSE(coordinator->captureNativeStack());
    EXPECT_TRUE(nativePeer->argumentsOf("pause").empty());
    mappingVerified = true;
    luaPeer->sendEvent("continued", rpc::Json::object());
    auto* child = rig->workbench->debugSessions()->controller(luaBinding.registrySessionId);
    ASSERT_TRUE(test::waitFor([&] { return child->session()->state() == debug::DebugState::Running; }, 10s));
    pauseLua(false);
    ASSERT_TRUE(test::waitFor([&] { return child->session()->state() == debug::DebugState::Stopped; }, 10s));
    coordinator->refresh();
    EXPECT_FALSE(coordinator->snapshot().canCapture);
    EXPECT_FALSE(coordinator->captureNativeStack());
    EXPECT_TRUE(nativePeer->argumentsOf("pause").empty());
}
} // namespace
} // namespace frost::app
