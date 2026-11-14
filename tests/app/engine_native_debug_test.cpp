// SPDX-License-Identifier: Apache-2.0
// Native engine overlay binding against DAP messages; real PID/pause/termination uses DapReal.
#include "frost/core/clock.hpp"
#include "frost/debug/breakpoints.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"

#include "../../src/app/src/engine_native_debug.hpp"
#include "debug/debug_test_util.hpp"
#include <QCoreApplication>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace frost::app {
namespace {
using namespace std::chrono_literals;
using debug::test::workPath;

bool eventsUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

class EngineNativeDebugTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = project::TrustStore::open(fs, workPath("trusted-roots.txt"));
        ASSERT_TRUE(opened);
        trust = std::move(*opened);
        ASSERT_TRUE(trust->trust(workPath("EngineProject")));
        ui::DebugControllerOptions options;
        options.trust = trust.get();
        options.fs = &fs;
        options.adapterFactory = debug::test::fakeFactory(adapter);
        controller = std::make_unique<ui::DebugController>(breakpoints, std::move(options));
        debug::scriptProgram(adapter, debug::FakeProgram{.runsForever = true});
        ASSERT_TRUE(controller->startTarget(debug::DebugTarget{.name = "FrostEditor",
                                                               .program = workPath("EngineProject/FrostEditor.exe"),
                                                               .trustRoots = {workPath("EngineProject")}}));
        ASSERT_TRUE(eventsUntil([&] { return controller->state() == debug::DebugState::Running; }));
        target = EngineNativeDebugTarget{4321, "instance", "server-generation", workPath("EngineProject"), 1, 7, 11};
        overlay =
            std::make_unique<EngineNativeDebugAdapter>(*controller, [this] { return target; }, [this] { ++changes; });
    }
    void process(std::int64_t pid) {
        adapter.sendEvent("process", rpc::Json::object().set("systemProcessId", pid));
        ASSERT_TRUE(eventsUntil([&] { return controller->session()->processId() == pid; }));
        QCoreApplication::processEvents();
    }
    void pause() {
        adapter.sendEvent("stopped", rpc::Json::object().set("reason", "pause").set("threadId", 1));
        ASSERT_TRUE(eventsUntil([&] { return overlay->paused(); }));
    }
    ManualClock clock;
    io::MemFs fs{clock};
    std::unique_ptr<project::TrustStore> trust;
    debug::FakeDebugAdapter adapter;
    debug::BreakpointManager breakpoints;
    std::unique_ptr<ui::DebugController> controller;
    std::optional<EngineNativeDebugTarget> target;
    std::unique_ptr<EngineNativeDebugAdapter> overlay;
    int changes = 0;
};

TEST_F(EngineNativeDebugTest, UsesOnlyExactNativePidAndActualDapStopContinueEvents) {
    EXPECT_FALSE(overlay->matched());
    process(9999);
    EXPECT_FALSE(overlay->matched());
    EXPECT_FALSE(overlay->paused());
    EXPECT_FALSE(overlay->terminate());
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
    process(4321);
    EXPECT_TRUE(overlay->matched());
    EXPECT_FALSE(overlay->paused());
    pause();
    EXPECT_TRUE(overlay->canTerminate());
    const auto pausedGeneration = controller->sessionGeneration();
    adapter.sendEvent("continued", rpc::Json::object().set("threadId", 1));
    ASSERT_TRUE(eventsUntil([&] { return !overlay->paused(); }));
    EXPECT_EQ(controller->sessionGeneration(), pausedGeneration);
    EXPECT_TRUE(overlay->matched());
    adapter.sendEvent("process", rpc::Json::object().set("systemProcessId", 0));
    ASSERT_TRUE(eventsUntil([&] { return !overlay->matched(); }));
    EXPECT_FALSE(overlay->canTerminate());
    process(4321);
    EXPECT_TRUE(overlay->matched());
    target.reset(); // The app revokes the supplier's proof on trust/project/identity changes.
    overlay->refresh();
    EXPECT_FALSE(overlay->matched());
    EXPECT_FALSE(overlay->paused());
    EXPECT_FALSE(overlay->canTerminate());
    EXPECT_FALSE(overlay->terminate());
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
    EXPECT_GT(changes, 0);
}

TEST_F(EngineNativeDebugTest, StaleGenerationIsRefusedAndTerminateAckDoesNotInventAnEnd) {
    process(4321);
    pause();
    auto refused = controller->terminateTarget(4321, controller->sessionGeneration() + 1);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
    ASSERT_TRUE(overlay->terminate());
    ASSERT_TRUE(adapter.waitForRequest("terminate"));
    EXPECT_TRUE(overlay->paused());
    EXPECT_TRUE(controller->debugging());
    EXPECT_FALSE(overlay->canTerminate());
    adapter.sendEvent("terminated", rpc::Json::object());
    ASSERT_TRUE(eventsUntil([&] { return !controller->debugging(); }));
    EXPECT_FALSE(overlay->matched());
    EXPECT_FALSE(overlay->paused());
}

TEST_F(EngineNativeDebugTest, ResumePresentationAndAckDoNotReleaseActualNativeStopEvidence) {
    process(4321);
    pause();
    adapter.on("continue", [](const rpc::Json&) {
        return debug::FakeDebugAdapter::Reply{.body = rpc::Json::object().set("allThreadsContinued", true)};
    });
    ASSERT_TRUE(controller->resume());
    ASSERT_TRUE(adapter.waitForRequest("continue"));
    ASSERT_TRUE(eventsUntil([&] { return controller->state() == debug::DebugState::Running; }));
    EXPECT_TRUE(overlay->paused());
    adapter.sendEvent("continued", rpc::Json::object().set("threadId", 1).set("allThreadsContinued", true));
    ASSERT_TRUE(eventsUntil([&] { return !overlay->paused(); }));
}

class EngineNativeDebugRegistryTest : public EngineNativeDebugTest {
protected:
    void SetUp() override {
        EngineNativeDebugTest::SetUp();
        if (HasFatalFailure())
            return;
        overlay.reset();
        registry = std::make_unique<ui::DebugSessionRegistry>(
            breakpoints, *controller, [this](debug::BreakpointManager& logical) {
                auto peer = std::make_unique<debug::FakeDebugAdapter>();
                debug::scriptProgram(*peer, debug::FakeProgram{.runsForever = true});
                ui::DebugControllerOptions options;
                options.trust = trust.get();
                options.fs = &fs;
                options.adapterFactory = debug::test::fakeFactory(*peer);
                peers.push_back(std::move(peer));
                return std::make_unique<ui::DebugController>(logical, std::move(options));
            });
        overlay =
            std::make_unique<EngineNativeDebugAdapter>(*registry, [this] { return target; }, [this] { ++changes; });
    }
    void TearDown() override {
        overlay.reset();
        registry.reset();
    }
    struct Peer {
        ui::DebugSessionId id;
        debug::FakeDebugAdapter* adapter;
    };
    std::optional<Peer> launch(std::int64_t pid) {
        auto id = registry->startTarget(debug::DebugTarget{.name = "Native peer",
                                                           .program = workPath("EngineProject/FrostEditor.exe"),
                                                           .trustRoots = {workPath("EngineProject")}});
        if (!id || !eventsUntil([&] { return registry->controller(*id)->state() == debug::DebugState::Running; })) {
            ADD_FAILURE() << "native registry peer did not start";
            return std::nullopt;
        }
        auto* peer = peers.back().get();
        peer->sendEvent("process", rpc::Json::object().set("systemProcessId", pid));
        if (!eventsUntil([&] { return registry->controller(*id)->session()->processId() == pid; })) {
            ADD_FAILURE() << "native registry peer did not publish PID";
            return std::nullopt;
        }
        return Peer{*id, peer};
    }
    std::vector<std::unique_ptr<debug::FakeDebugAdapter>> peers;
    std::unique_ptr<ui::DebugSessionRegistry> registry;
};

TEST_F(EngineNativeDebugRegistryTest, SelectionNeverRebindsAndTerminateUsesTheExactNativeController) {
    process(9999);
    auto peer = launch(4321);
    ASSERT_TRUE(peer);
    ASSERT_TRUE(overlay->binding());
    const auto bound = *overlay->binding();
    EXPECT_EQ(bound.registrySessionId, peer->id);
    ASSERT_TRUE(registry->select(registry->primaryId()));
    EXPECT_EQ(overlay->binding(), bound);
    adapter.sendEvent("stopped", rpc::Json::object().set("reason", "pause").set("threadId", 1));
    ASSERT_TRUE(eventsUntil([&] { return controller->state() == debug::DebugState::Stopped; }));
    EXPECT_FALSE(overlay->paused());
    peer->adapter->sendEvent("stopped", rpc::Json::object().set("reason", "pause").set("threadId", 1));
    ASSERT_TRUE(eventsUntil([&] { return overlay->paused(); }));
    ASSERT_TRUE(registry->select(peer->id));
    ASSERT_TRUE(registry->select(registry->primaryId()));
    EXPECT_EQ(overlay->binding(), bound);
    ASSERT_TRUE(overlay->terminate());
    ASSERT_TRUE(peer->adapter->waitForRequest("terminate"));
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
    EXPECT_TRUE(overlay->paused()); // A request or its ACK is not an actual DAP continuation/end.
}

TEST_F(EngineNativeDebugRegistryTest, GenerationCollisionAndPidReplacementCannotInheritAuthenticatedBinding) {
    process(4321);
    ASSERT_TRUE(overlay->binding());
    const auto original = *overlay->binding();
    auto peer = launch(4321);
    ASSERT_TRUE(peer);
    ASSERT_EQ(controller->sessionGeneration(), registry->controller(peer->id)->sessionGeneration());
    EXPECT_FALSE(overlay->matched()); // Two live native controllers for one PID are ambiguous.
    EXPECT_FALSE(overlay->terminate());
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
    EXPECT_TRUE(peer->adapter->argumentsOf("terminate").empty());
    adapter.sendEvent("terminated", rpc::Json::object());
    ASSERT_TRUE(eventsUntil([&] { return !controller->debugging(); }));
    ASSERT_EQ(registry->nativeForPid(4321).id, peer->id);
    ASSERT_TRUE(registry->select(peer->id));
    overlay->refresh();
    EXPECT_FALSE(overlay->matched());
    EXPECT_FALSE(overlay->terminate());
    EXPECT_TRUE(peer->adapter->argumentsOf("terminate").empty());
    ++target->connectionGeneration; // Only a fresh authenticated connection proof can bind the peer.
    overlay->refresh();
    ASSERT_TRUE(overlay->binding());
    EXPECT_NE(overlay->binding()->registrySessionId, original.registrySessionId);
    EXPECT_EQ(overlay->binding()->registrySessionId, peer->id);
}

TEST_F(EngineNativeDebugRegistryTest, RetainedStoppedBindingSurvivesSelectionButNeverRegistryRemoval) {
    process(9999);
    auto peer = launch(4321);
    ASSERT_TRUE(peer);
    peer->adapter->sendEvent("stopped", rpc::Json::object().set("reason", "pause").set("threadId", 1));
    ASSERT_TRUE(eventsUntil([&] { return overlay->paused(); }));
    const auto retained = *overlay->binding();
    // This supplier retains exactly the already authenticated tuple when engine heartbeat is lost.
    overlay = std::make_unique<EngineNativeDebugAdapter>(
        *registry, [retained] { return std::optional(retained.target); }, [this] { ++changes; });
    ASSERT_TRUE(registry->select(registry->primaryId()));
    EXPECT_EQ(overlay->binding(), retained);
    peer->adapter->sendEvent("terminated", rpc::Json::object());
    ASSERT_TRUE(eventsUntil([&] { return !registry->controller(peer->id)->debugging(); }));
    ASSERT_TRUE(registry->remove(peer->id));
    auto replacement = launch(4321);
    ASSERT_TRUE(replacement);
    ASSERT_EQ(registry->controller(replacement->id)->sessionGeneration(), retained.debugGeneration);
    EXPECT_FALSE(overlay->matched());
    EXPECT_FALSE(overlay->terminate());
    EXPECT_TRUE(replacement->adapter->argumentsOf("terminate").empty());
}

TEST_F(EngineNativeDebugRegistryTest, AttachedEngineExplicitStopUsesTerminateAndOrdinaryStopStillDetaches) {
    process(9999);
    auto id = registry->createSession(QStringLiteral("Attached engine"));
    ASSERT_TRUE(id);
    auto* peer = peers.back().get();
    peer->on("attach",
             [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json()}}}; });
    auto* attached = registry->controller(*id);
    ASSERT_TRUE(attached->attachTarget(debug::DebugTarget{.name = "Attached engine",
                                                          .program = workPath("EngineProject/FrostEditor.exe"),
                                                          .trustRoots = {workPath("EngineProject")}},
                                       debug::test::fakeFactory(*peer), rpc::Json::object().set("type", "lldb-dap")));
    ASSERT_TRUE(eventsUntil([&] { return attached->state() == debug::DebugState::Running; }));
    peer->sendEvent("process", rpc::Json::object().set("systemProcessId", 4321));
    ASSERT_TRUE(eventsUntil([&] { return overlay->matched(); }));
    peer->sendEvent("stopped", rpc::Json::object().set("reason", "pause").set("threadId", 1));
    ASSERT_TRUE(eventsUntil([&] { return overlay->paused(); }));
    ASSERT_TRUE(registry->select(registry->primaryId()));
    ASSERT_TRUE(overlay->terminate());
    ASSERT_TRUE(peer->waitForRequest("terminate"));
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
    ASSERT_TRUE(registry->select(*id));
    registry->stopSelected();
    ASSERT_TRUE(eventsUntil([&] { return !attached->debugging(); }));
    ASSERT_FALSE(peer->argumentsOf("disconnect").empty());
    EXPECT_EQ(peer->argumentsOf("disconnect").back().boolAt("terminateDebuggee"), false);
}
} // namespace
} // namespace frost::app
