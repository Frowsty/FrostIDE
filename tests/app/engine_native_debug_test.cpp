// SPDX-License-Identifier: Apache-2.0
// Native engine overlay binding against DAP messages; real PID/pause/termination uses DapReal.
#include "frost/core/clock.hpp"
#include "frost/debug/breakpoints.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/debug_controller.hpp"

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
} // namespace
} // namespace frost::app
