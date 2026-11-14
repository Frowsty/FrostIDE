// SPDX-License-Identifier: Apache-2.0
// Native-host Stop guards against protocol validator inputs. Actual pinned LLDB/HMAC pairing is
// covered by the mandatory mixed-engine corpus; these doubles do not claim a real engine process.
#include "frost/core/clock.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/project/trust_store.hpp"

#include "debug/debug_test_util.hpp"
#include "engine_native_host_internal.hpp"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <gtest/gtest.h>
namespace frost::app {
namespace {
bool events(const std::function<bool()>& done) {
    const QDeadlineTimer deadline(5000);
    while (!done()) {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}
class EngineNativeHostLeaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = project::TrustStore::open(fs, debug::test::workPath("host-trust"));
        ASSERT_TRUE(opened);
        trust = std::move(*opened);
        ASSERT_TRUE(trust->trust(debug::test::workPath("EngineProject")));
        ui::DebugControllerOptions options;
        options.trust = trust.get();
        options.fs = &fs;
        options.adapterFactory = debug::test::fakeFactory(peer);
        controller = std::make_unique<ui::DebugController>(points, std::move(options));
        debug::scriptProgram(peer, debug::FakeProgram{.runsForever = true});
        registry = std::make_unique<ui::DebugSessionRegistry>(
            points, *controller,
            [](debug::BreakpointManager&) -> std::unique_ptr<ui::DebugController> { return nullptr; });
    }
    void start(debug::DebugTargetMode mode) {
        debug::DebugTarget target;
        target.name = "validator host";
        target.program = debug::test::workPath("EngineProject/FrostEditor.exe");
        target.trustRoots = {debug::test::workPath("EngineProject")};
        if (mode == debug::DebugTargetMode::Launch)
            ASSERT_TRUE(controller->startTarget(target));
        else
            ASSERT_TRUE(
                controller->attachTarget(target, debug::test::fakeFactory(peer), rpc::Json::object().set("pid", 4321)));
        ASSERT_TRUE(events([&] { return controller->state() == debug::DebugState::Running; }));
        peer.sendEvent("process", rpc::Json::object().set("systemProcessId", 4321));
        ASSERT_TRUE(events([&] { return controller->session()->processId() == 4321; }));
    }
    std::shared_ptr<EngineNativeHostState> lease() {
        auto host = std::make_shared<EngineNativeHostState>();
        host->registry = registry.get();
        host->session = registry->primaryId();
        host->generation = controller->sessionGeneration();
        host->pid = 4321;
        return host;
    }
    ManualClock clock;
    io::MemFs fs{clock};
    std::unique_ptr<project::TrustStore> trust;
    debug::BreakpointManager points;
    debug::FakeDebugAdapter peer;
    std::unique_ptr<ui::DebugController> controller;
    std::unique_ptr<ui::DebugSessionRegistry> registry;
};
TEST(EngineNativeHostIngress, ArbitraryCategoriesAreNormalizedAndTinyEventFloodIsBounded) {
    auto host = std::make_shared<EngineNativeHostState>();
    auto sink = nativeEngineHostOutput(host);
    const std::string hugeCategory(2U * 1024U * 1024U, 'x');
    sink(hugeCategory, "x");
    ASSERT_EQ(host->output.size(), 1u);
    EXPECT_EQ(host->output.front().first, "other");
    EXPECT_EQ(host->bytes, std::string_view("other").size() + 1u);
    for (std::size_t i = 1; i < EngineNativeHostState::maxOutputEvents; ++i)
        sink("stdout", "x");
    EXPECT_FALSE(host->overflow);
    sink("stdout", "x");
    EXPECT_TRUE(host->overflow);
    EXPECT_EQ(host->output.size(), EngineNativeHostState::maxOutputEvents);
    const auto boundedBytes = host->bytes;
    sink("stderr", "private credential must never escape");
    EXPECT_EQ(host->bytes, boundedBytes);
}
TEST(EngineNativeHostIngress, TotalRetainedBytesIncludeCategoryAndPayloadAndInactiveSinkDiscards) {
    auto host = std::make_shared<EngineNativeHostState>();
    auto sink = nativeEngineHostOutput(host);
    const std::string payload(EngineNativeHostState::maxOutputBytes - std::string_view("stderr").size(), 'x');
    sink("stderr", payload);
    ASSERT_FALSE(host->overflow);
    EXPECT_EQ(host->bytes, EngineNativeHostState::maxOutputBytes);
    sink("stdout", "x");
    EXPECT_TRUE(host->overflow);
    EXPECT_EQ(host->output.size(), 1u);
    auto inactive = std::make_shared<EngineNativeHostState>();
    auto discard = nativeEngineHostOutput(inactive);
    inactive->active = false;
    discard("stdout", "not retained");
    EXPECT_TRUE(inactive->output.empty());
    inactive->active = true;
    inactive->discardRaw = true;
    discard("stderr", "not retained");
    EXPECT_TRUE(inactive->output.empty());
}
TEST_F(EngineNativeHostLeaseTest, WrongGenerationOrPidCannotStopAReplacement) {
    start(debug::DebugTargetMode::Launch);
    auto host = lease();
    EXPECT_TRUE(nativeEngineHostCurrent(host));
    ++host->generation;
    EXPECT_FALSE(stopNativeEngineHost(host));
    EXPECT_TRUE(peer.argumentsOf("disconnect").empty());
    --host->generation;
    host->pid = 9999;
    EXPECT_FALSE(stopNativeEngineHost(host));
    EXPECT_TRUE(peer.argumentsOf("disconnect").empty());
}
TEST_F(EngineNativeHostLeaseTest, ExternalAttachNeverGrantsNativeLaunchOwnership) {
    start(debug::DebugTargetMode::Attach);
    auto host = lease();
    EXPECT_FALSE(nativeEngineHostCurrent(host));
    EXPECT_FALSE(stopNativeEngineHost(host));
    EXPECT_TRUE(peer.argumentsOf("disconnect").empty());
    EXPECT_TRUE(controller->debugging());
}
TEST_F(EngineNativeHostLeaseTest, ExactLaunchStopUsesItsActualControllerAndDoesNotTouchOtherIdentities) {
    start(debug::DebugTargetMode::Launch);
    auto host = lease();
    ASSERT_TRUE(stopNativeEngineHost(host));
    ASSERT_TRUE(peer.waitForRequest("disconnect"));
    const auto requests = peer.argumentsOf("disconnect");
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests.front().boolAt("terminateDebuggee"), true);
}
} // namespace
} // namespace frost::app
