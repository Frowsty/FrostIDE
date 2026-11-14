// SPDX-License-Identifier: Apache-2.0
// Parser/UI policy fixtures only: no synthetic dump is passed to a real native debugger here.
// Actual engine Natvis and minidump inspection are covered by the separate DbgEng corpus target.
#include "frost/core/clock.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/debug_retirement.hpp"

#include "debug/debug_test_util.hpp"
#include "engine_dump_controller.hpp"
#include "support/crash_artifact_fixture.hpp"
#include "support/scratch_dir.hpp"
#include <QCoreApplication>
#include <QThread>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace frost::app {
namespace {
using namespace std::chrono_literals;
bool pumpDump(const std::function<bool()>& completed) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

class EngineDumpControllerTest : public testing::Test {
protected:
    void SetUp() override {
        guiThread = QThread::currentThread();
        auto parsed = frost::test::makeSyntheticCrashArtifact();
        ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
        artifact = std::move(*parsed);
        root = artifact->source().projectPath;
        ASSERT_TRUE(fs.createDirectories(root).has_value());
        auto opened = project::TrustStore::open(fs, debug::test::workPath("dump-trust.json"));
        ASSERT_TRUE(opened.has_value());
        trust = std::move(*opened);
        ASSERT_TRUE(trust->trust(root).has_value());
        peer.on("initialize", [](const rpc::Json&) {
            debug::FakeDebugAdapter::Reply reply;
            reply.body = rpc::Json::object().set("supportsConfigurationDoneRequest", true);
            return reply;
        });
        peer.on("launch", [](const rpc::Json&) {
            debug::FakeDebugAdapter::Reply reply;
            reply.events.emplace_back("initialized", rpc::Json::object());
            return reply;
        });
        peer.on("configurationDone", [](const rpc::Json&) {
            debug::FakeDebugAdapter::Reply reply;
            reply.events.emplace_back("stopped",
                                      rpc::Json::object().set("reason", "exception").set("allThreadsStopped", true));
            return reply;
        });
        primary = make(catalog);
        sessions = std::make_unique<ui::DebugSessionRegistry>(
            catalog, *primary, [this](debug::BreakpointManager& points) { return make(points); });
        EngineDumpControllerOptions options;
        options.notifications = &notifications;
        options.sessions = sessions.get();
        options.launcher = &io::realProcessLauncher();
        options.work = work;
        options.cacheDirectory = cache.path();
        options.adapterFactory = debug::test::fakeFactory(peer);
        controller = std::make_unique<EngineDumpController>(std::move(options));
    }
    void TearDown() override {
        if (controller)
            controller->shutdown();
        controller.reset();
        if (sessions)
            sessions->shutdown();
        sessions.reset();
        primary.reset();
        retirements->drain();
        work->drain(); // All captured test services remain alive until both lifetime barriers finish.
    }
    std::unique_ptr<ui::DebugController> make(debug::BreakpointManager& points) {
        ui::DebugControllerOptions options;
        options.fs = &fs;
        options.trust = trust.get();
        options.retirements = retirements;
        return std::make_unique<ui::DebugController>(points, std::move(options));
    }
    EngineDumpAdmission admission() {
        EngineDumpAdmission result;
        result.projectRoot = root;
        const auto current = admitted;
        result.current = [current]() -> Result<void> {
            return current->load() ? Result<void>()
                                   : Result<void>(fail(ErrorCode::Untrusted, "fixture admission revoked"));
        };
        result.validatePhysical = [this]() -> Result<void> {
            physicalOnGui = QThread::currentThread() == guiThread;
            ++physicalChecks;
            return {};
        };
        return result;
    }
    ManualClock clock;
    io::MemFs fs{clock};
    std::string root;
    std::shared_ptr<const engine::ValidatedCrashArtifact> artifact;
    std::unique_ptr<project::TrustStore> trust;
    debug::FakeDebugAdapter peer;
    debug::BreakpointManager catalog;
    std::shared_ptr<ui::DebugRetirementBatch> retirements = std::make_shared<ui::DebugRetirementBatch>();
    std::shared_ptr<EngineDumpWorkBatch> work = std::make_shared<EngineDumpWorkBatch>();
    std::unique_ptr<ui::DebugController> primary;
    std::unique_ptr<ui::DebugSessionRegistry> sessions;
    ui::NotificationCenter notifications;
    std::shared_ptr<std::atomic<bool>> admitted = std::make_shared<std::atomic<bool>>(true);
    std::atomic<unsigned> physicalChecks{0};
    std::atomic<bool> physicalOnGui{false};
    QThread* guiThread = nullptr;
    std::unique_ptr<EngineDumpController> controller;
    frost::test::ScratchDir cache{"dump-controller-cache"};
};

TEST_F(EngineDumpControllerTest, OffersWithoutLaunchingThenOpensOnlyTheExactPrivateReadOnlyCopy) {
    auto offered = controller->offer(artifact, admission());
    ASSERT_TRUE(offered.has_value());
    ASSERT_EQ(notifications.log().size(), 1U);
    EXPECT_TRUE(peer.requests().empty());
    EXPECT_EQ(physicalChecks.load(), 0U);
    const auto& notice = notifications.log().back().notification;
    EXPECT_TRUE(notice.text.contains(QStringLiteral("previously authenticated")));
    ASSERT_EQ(notice.actions.size(), 1U);
    EXPECT_EQ(notice.actions.front().label, QStringLiteral("Open dump"));
    notice.actions.front().run();
    ASSERT_TRUE(pumpDump([&] { return peer.argumentsOf("launch").size() == 1; }));
    EXPECT_EQ(physicalChecks.load(), 2U);
    EXPECT_FALSE(physicalOnGui.load());
    const auto launch = peer.argumentsOf("launch").front();
    EXPECT_TRUE(launch.stringAt("program").empty());
    const std::string dump(launch.stringAt("dumpFile"));
    EXPECT_FALSE(dump.empty());
    EXPECT_NE(lang::documentKey(dump).find(lang::documentKey(cache.path())), std::string::npos);
    auto actual = io::realFileSystem().readFile(dump);
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(*actual, artifact->dumpBytes());
    EXPECT_EQ(io::sha256Hex(*actual), artifact->sha256());
    auto* selected = sessions->selected();
    ASSERT_NE(selected, primary.get());
    ASSERT_TRUE(selected->readOnlySession());
    EXPECT_FALSE(selected->resume());
    EXPECT_TRUE(peer.argumentsOf("continue").empty());
    EXPECT_TRUE(peer.argumentsOf("setBreakpoints").empty());
    EXPECT_TRUE(peer.argumentsOf("setFunctionBreakpoints").empty());
    selected->stop();
    ASSERT_TRUE(pumpDump([&] { return !selected->debugging(); }));
    const auto disconnected = peer.argumentsOf("disconnect");
    ASSERT_FALSE(disconnected.empty());
    EXPECT_EQ(disconnected.back().boolAt("terminateDebuggee"), false);
    retirements->drain();
    ASSERT_TRUE(pumpDump([&] { return work->pending() == 0; }));
    EXPECT_FALSE(io::realFileSystem().exists(dump).value_or(true));
}

TEST_F(EngineDumpControllerTest, DuplicateAndRevokedOffersNeverDispatchAnAdapter) {
    auto offered = controller->offer(artifact, admission());
    auto duplicate = controller->offer(artifact, admission());
    ASSERT_TRUE(offered.has_value());
    ASSERT_TRUE(duplicate.has_value());
    EXPECT_EQ(*offered, *duplicate);
    EXPECT_EQ(notifications.log().size(), 1U);
    admitted->store(false);
    EXPECT_FALSE(controller->open(*offered).has_value());
    EXPECT_EQ(physicalChecks.load(), 0U);
    controller->revoke();
    admitted->store(true);
    EXPECT_FALSE(controller->open(*offered).has_value());
    EXPECT_TRUE(peer.requests().empty());
    EXPECT_EQ(work->pending(), 0U);
}

TEST_F(EngineDumpControllerTest, WrongProjectAndShutdownAreRejectedBeforeNotification) {
    auto wrong = admission();
    wrong.projectRoot += "-other";
    EXPECT_FALSE(controller->offer(artifact, std::move(wrong)).has_value());
    EXPECT_TRUE(notifications.log().empty());
    controller->shutdown();
    EXPECT_FALSE(controller->offer(artifact, admission()).has_value());
    EXPECT_TRUE(notifications.log().empty());
}

TEST_F(EngineDumpControllerTest, RevocationDuringBackgroundCopyPreventsPublicationAndLaunch) {
    auto offered = controller->offer(artifact, admission());
    ASSERT_TRUE(offered.has_value());
    ASSERT_TRUE(controller->open(*offered).has_value());
    // No GUI event processing can publish this copy between the two calls.
    controller->revoke();
    ASSERT_TRUE(pumpDump([&] { return work->pending() == 0; }));
    EXPECT_TRUE(peer.requests().empty());
    EXPECT_EQ(sessions->sessions().size(), 1U);
}

TEST_F(EngineDumpControllerTest, PhysicalRevocationAfterCopyPreventsAdapterDispatch) {
    auto source = admission();
    source.validatePhysical = [this]() -> Result<void> {
        if (++physicalChecks == 2)
            return fail(ErrorCode::Untrusted, "fixture physical identity changed during copy");
        return {};
    };
    auto offered = controller->offer(artifact, std::move(source));
    ASSERT_TRUE(offered.has_value());
    ASSERT_TRUE(controller->open(*offered).has_value());
    ASSERT_TRUE(pumpDump([&] { return notifications.log().size() == 2; }));
    EXPECT_EQ(physicalChecks.load(), 2U);
    EXPECT_TRUE(peer.requests().empty());
    EXPECT_EQ(sessions->sessions().size(), 1U);
}

TEST_F(EngineDumpControllerTest, RegistryPublicationRevocationPreventsAdapterDispatch) {
    QObject::connect(sessions.get(), &ui::DebugSessionRegistry::sessionsChanged, controller.get(),
                     [this] { controller->revoke(); });
    auto offered = controller->offer(artifact, admission());
    ASSERT_TRUE(offered.has_value());
    ASSERT_TRUE(controller->open(*offered).has_value());
    ASSERT_TRUE(pumpDump([&] { return notifications.log().size() == 2; }));
    EXPECT_TRUE(peer.requests().empty());
    EXPECT_EQ(sessions->sessions().size(), 1U);
}

TEST_F(EngineDumpControllerTest, ExplicitRevocationBeforeDelayedInitializePreventsActualLaunchWrite) {
    peer.on("initialize", [](const rpc::Json&) {
        debug::FakeDebugAdapter::Reply reply;
        reply.respond = false;
        return reply;
    });
    auto offered = controller->offer(artifact, admission());
    ASSERT_TRUE(offered.has_value());
    ASSERT_TRUE(controller->open(*offered).has_value());
    ASSERT_TRUE(pumpDump([&] { return peer.argumentsOf("initialize").size() == 1; }));
    ASSERT_TRUE(peer.argumentsOf("launch").empty());
    const auto initialize = peer.requests().front();
    controller->revoke();
    const auto response = rpc::Json::object()
                              .set("seq", 999)
                              .set("type", "response")
                              .set("request_seq", initialize.integerAt("seq").value())
                              .set("command", "initialize")
                              .set("success", true)
                              .set("body", rpc::Json::object().set("supportsConfigurationDoneRequest", true))
                              .dump();
    peer.sendRaw("Content-Length: " + std::to_string(response.size()) + "\r\n\r\n" + response);
    ASSERT_TRUE(pumpDump([&] { return !sessions->selected()->debugging(); }));
    EXPECT_TRUE(peer.argumentsOf("launch").empty());
}

TEST_F(EngineDumpControllerTest, TrustAdmissionChangeBeforeDelayedInitializePreventsActualLaunchWrite) {
    peer.on("initialize", [](const rpc::Json&) {
        debug::FakeDebugAdapter::Reply reply;
        reply.respond = false;
        return reply;
    });
    auto offered = controller->offer(artifact, admission());
    ASSERT_TRUE(offered.has_value());
    ASSERT_TRUE(controller->open(*offered).has_value());
    ASSERT_TRUE(pumpDump([&] { return peer.argumentsOf("initialize").size() == 1; }));
    ASSERT_TRUE(peer.argumentsOf("launch").empty());
    const auto initialize = peer.requests().front();
    admitted->store(false); // No GUI revoke call: the actual writer must recheck admission itself.
    const auto response = rpc::Json::object()
                              .set("seq", 999)
                              .set("type", "response")
                              .set("request_seq", initialize.integerAt("seq").value())
                              .set("command", "initialize")
                              .set("success", true)
                              .set("body", rpc::Json::object().set("supportsConfigurationDoneRequest", true))
                              .dump();
    peer.sendRaw("Content-Length: " + std::to_string(response.size()) + "\r\n\r\n" + response);
    ASSERT_TRUE(pumpDump([&] { return !sessions->selected()->debugging(); }));
    EXPECT_TRUE(peer.argumentsOf("launch").empty());
}

TEST_F(EngineDumpControllerTest, DestructionQueuesBlockedCleanupWithoutFilesystemWorkOnGui) {
    struct Gate {
        std::atomic<bool> released{false}, entered{false}, onGui{false};
    };
    const auto gate = std::make_shared<Gate>();
    struct Release {
        std::shared_ptr<Gate> gate;
        ~Release() {
            gate->released.store(true);
            gate->released.notify_all();
        }
    } release{gate};
    controller.reset();
    work->drain();
    work = std::make_shared<EngineDumpWorkBatch>([gate, gui = guiThread](std::string path) {
        gate->onGui = QThread::currentThread() == gui;
        gate->entered = true;
        // Record an accidental GUI cleanup without hanging this regression on its own thread.
        if (!gate->onGui.load())
            gate->released.wait(false);
        (void)io::realFileSystem().removeAll(path);
    });
    EngineDumpControllerOptions options;
    options.notifications = &notifications;
    options.sessions = sessions.get();
    options.launcher = &io::realProcessLauncher();
    options.work = work;
    options.cacheDirectory = cache.path();
    options.adapterFactory = debug::test::fakeFactory(peer);
    controller = std::make_unique<EngineDumpController>(std::move(options));
    auto offered = controller->offer(artifact, admission());
    ASSERT_TRUE(offered.has_value());
    ASSERT_TRUE(controller->open(*offered).has_value());
    const auto copiedDeadline = std::chrono::steady_clock::now() + 5s;
    // Do not process GUI timers: the offer is the final owner of the completed private copy.
    while ((physicalChecks.load() != 2 || work->pending() != 0) && std::chrono::steady_clock::now() < copiedDeadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(physicalChecks.load(), 2U);
    ASSERT_EQ(work->pending(), 0U);
    const auto before = std::chrono::steady_clock::now();
    controller.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 100ms);
    ASSERT_TRUE(pumpDump([&] { return gate->entered.load(); }));
    EXPECT_FALSE(gate->onGui.load());
    EXPECT_EQ(work->pending(), 1U);
    EXPECT_TRUE(peer.requests().empty());
    gate->released.store(true);
    gate->released.notify_all();
    work->drain();
    EXPECT_EQ(work->pending(), 0U);
}

} // namespace
} // namespace frost::app
