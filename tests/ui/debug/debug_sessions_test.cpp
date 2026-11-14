// SPDX-License-Identifier: Apache-2.0
// Registry projection tests over DAP messages; real concurrent lldb acceptance is a separate suite.
#include "frost/core/clock.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/rpc/framing.hpp"
#include "frost/ui/debug_retirement.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/debug_sessions_window.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/run_console.hpp"

#include "debug/debug_test_util.hpp"
#include "editor/editor_harness.hpp"
#include <QCoreApplication>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace frost::ui {
namespace {
using namespace std::chrono_literals;
using debug::test::workPath;
bool pump(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
class DebugSessionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(fs.createDirectories(root));
        auto opened = project::TrustStore::open(fs, workPath("session-trust.txt"));
        ASSERT_TRUE(opened);
        trust = std::move(*opened);
        ASSERT_TRUE(trust->trust(root));
        primary = make(catalog);
        sessions = std::make_unique<DebugSessionRegistry>(
            catalog, *primary, [this](debug::BreakpointManager& points) { return make(points); });
    }
    void TearDown() override {
        sessions.reset();
        primary.reset();
        retirements->drain(); // Test peers/trust/filesystem stay alive even after an assertion failure.
    }
    std::unique_ptr<DebugController> make(debug::BreakpointManager& points) {
        const auto index = adapters.size();
        auto adapter = std::make_unique<debug::FakeDebugAdapter>();
        debug::scriptProgram(*adapter, debug::FakeProgram{.sourcePath = source,
                                                          .codeLines = {index == 2 ? 20 : 10},
                                                          .entryLine = index == 2 ? 20 : 10,
                                                          .runsForever = true});
        auto* peer = adapter.get();
        adapters.push_back(std::move(adapter));
        DebugControllerOptions options;
        options.trust = trust.get();
        options.fs = &fs;
        options.allowRerun = false;
        options.retirements = retirements;
        options.adapterFactory = debug::test::fakeFactory(*peer);
        return std::make_unique<DebugController>(points, std::move(options));
    }
    debug::DebugTarget target(std::string name) {
        return {.name = std::move(name), .program = root + "/App.exe", .trustRoots = {root}};
    }
    DebugSessionId launch(std::string name) {
        auto created = sessions->startTarget(target(std::move(name)));
        EXPECT_TRUE(created);
        return created.value_or(0);
    }
    const std::string root = workPath("Sessions");
    const std::string source = workPath("Sessions/main.cpp");
    ManualClock clock;
    io::MemFs fs{clock};
    std::unique_ptr<project::TrustStore> trust;
    std::vector<std::unique_ptr<debug::FakeDebugAdapter>> adapters;
    std::shared_ptr<DebugRetirementBatch> retirements = std::make_shared<DebugRetirementBatch>();
    debug::BreakpointManager catalog;
    std::unique_ptr<DebugController> primary;
    std::unique_ptr<DebugSessionRegistry> sessions;
};

TEST_F(DebugSessionsTest, SharesDefinitionsButKeepsVerificationAndExceptionsInTheActualSession) {
    auto point = catalog.add(source, 10);
    ASSERT_TRUE(point);
    const auto first = launch("First");
    const auto second = launch("Second");
    auto* a = sessions->controller(first);
    auto* b = sessions->controller(second);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(pump([&] {
        return a->breakpoints().find(*point)->status == debug::BreakpointStatus::Verified &&
               b->breakpoints().find(*point)->status == debug::BreakpointStatus::Unverified;
    }));
    EXPECT_EQ(a->breakpoints().find(*point)->line, b->breakpoints().find(*point)->line);
    EXPECT_EQ(catalog.find(*point)->status, debug::BreakpointStatus::Unknown);
    a->breakpoints().setAvailableExceptionFilters(
        {debug::dap::ExceptionFilter{.filter = "lua_error", .label = "Lua error"}});
    b->breakpoints().setAvailableExceptionFilters(
        {debug::dap::ExceptionFilter{.filter = "cpp_throw", .label = "C++ throw"}});
    EXPECT_EQ(a->breakpoints().exceptionFilters().front().filter, "lua_error");
    EXPECT_EQ(b->breakpoints().exceptionFilters().front().filter, "cpp_throw");
    ASSERT_TRUE(a->breakpoints().setProperties(*point, "counter > 4", "", ""));
    EXPECT_EQ(b->breakpoints().find(*point)->condition, "counter > 4");
    EXPECT_EQ(catalog.find(*point)->condition, "counter > 4");
    EXPECT_EQ(b->breakpoints().exceptionFilters().front().filter, "cpp_throw");
}

TEST_F(DebugSessionsTest, GenericAdapterSendsStoredFunctionsAndExceptionsBeforeConfigurationDone) {
    ASSERT_TRUE(catalog.addFunction("onCreate"));
    catalog.setAvailableExceptionFilters({debug::dap::ExceptionFilter{.filter = "lua_error", .label = "Lua error"}});
    ASSERT_TRUE(catalog.setExceptionFilterEnabled("lua_error", true));
    auto* peer = adapters.front().get();
    peer->on("initialize", [](const rpc::Json&) {
        return debug::FakeDebugAdapter::Reply{
            .body = rpc::Json::object()
                        .set("supportsConfigurationDoneRequest", true)
                        .set("supportsFunctionBreakpoints", true)
                        .set("exceptionBreakpointFilters",
                             rpc::Json::array().push(
                                 rpc::Json::object().set("filter", "lua_error").set("label", "Lua error")))};
    });
    peer->on("attach", [](const rpc::Json&) {
        return debug::FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json::object()}}};
    });
    ASSERT_TRUE(primary->startAdapterTarget(target("Lua"), debug::test::fakeFactory(*peer), rpc::Json::object(),
                                            "frost-lua", debug::DebugTargetMode::Attach));
    // Do not pump Qt: the worker must configure from the captured options before the GUI's
    // onCapabilities relay runs, otherwise the first real VM entry can miss these definitions.
    ASSERT_TRUE(peer->waitForRequest("configurationDone"));
    const auto functions = peer->argumentsOf("setFunctionBreakpoints");
    ASSERT_FALSE(functions.empty());
    ASSERT_NE(functions.front().find("breakpoints"), nullptr);
    ASSERT_EQ(functions.front().find("breakpoints")->size(), 1U);
    EXPECT_EQ(functions.front().find("breakpoints")->items().front().stringAt("name"), "onCreate");
    const auto exceptions = peer->argumentsOf("setExceptionBreakpoints");
    ASSERT_FALSE(exceptions.empty());
    ASSERT_NE(exceptions.front().find("filters"), nullptr);
    EXPECT_EQ(*exceptions.front().find("filters"), rpc::Json::array().push("lua_error"));
    const auto commands = peer->commands();
    EXPECT_LT(std::ranges::find(commands, "setFunctionBreakpoints"), std::ranges::find(commands, "configurationDone"));
    EXPECT_LT(std::ranges::find(commands, "setExceptionBreakpoints"), std::ranges::find(commands, "configurationDone"));
}

TEST_F(DebugSessionsTest, PrivateNativeLaunchRejectsCustomAndConfiguredAdaptersBeforeAnyProcessStarts) {
    auto native = target("Engine");
    native.environment = {{"FROST_PRIVATE", "PRIVATE_BOOTSTRAP_SENTINEL"}};
    const auto sink = [](std::string_view, std::string_view) {};
    const auto admission = []() -> Result<void> { return {}; };
    EXPECT_FALSE(primary->startNativeEngineTarget(native, sink, {}, admission));
    EXPECT_TRUE(adapters.front()->requests().empty());
    const auto external = root + "/external-dap.exe";
    ASSERT_TRUE(fs.writeFileAtomic(external, "external adapter fixture"));
    io::ScriptedProcessLauncher launcher;
    debug::BreakpointManager points;
    DebugControllerOptions options;
    options.trust = trust.get();
    options.fs = &fs;
    options.launcher = &launcher;
    options.lldbDapSetting = [external] { return external; };
    DebugController configured(points, std::move(options));
    const auto result = configured.startNativeEngineTarget(native, sink, {}, admission);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(result.error().message().find("PRIVATE_BOOTSTRAP_SENTINEL"), std::string::npos);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_FALSE(configured.nativeOsThreadIdsVerified());
}

TEST_F(DebugSessionsTest, EmptyProgramIsAdmittedOnlyForAnExplicitLocalReadOnlyDump) {
    auto dump = target("crash.dmp");
    dump.program.clear();
    auto factory = debug::test::fakeFactory(*adapters.front());
    EXPECT_FALSE(primary->startAdapterTarget(dump, factory, rpc::Json::object(), "frost-dbgeng"));
    EXPECT_FALSE(primary->startAdapterTarget(dump, factory, rpc::Json::object(), "frost-dbgeng",
                                             debug::DebugTargetMode::Launch, true));
    EXPECT_FALSE(primary->startAdapterTarget(dump, factory, rpc::Json::object().set("dumpFile", "relative.dmp"),
                                             "frost-dbgeng", debug::DebugTargetMode::Launch, true));
    EXPECT_FALSE(primary->startAdapterTarget(dump, factory,
                                             rpc::Json::object().set("dumpFile", "//remote/share/crash.dmp"),
                                             "frost-dbgeng", debug::DebugTargetMode::Launch, true));
    EXPECT_TRUE(adapters.front()->requests().empty());
    ASSERT_TRUE(primary->startAdapterTarget(dump, factory, rpc::Json::object().set("dumpFile", root + "/crash.dmp"),
                                            "frost-dbgeng", debug::DebugTargetMode::Launch, true));
    ASSERT_TRUE(adapters.front()->waitForRequest("configurationDone"));
    EXPECT_TRUE(primary->readOnlySession());
    EXPECT_FALSE(primary->nativeSession());
    EXPECT_TRUE(adapters.front()->argumentsOf("setBreakpoints").empty());
    EXPECT_TRUE(adapters.front()->argumentsOf("setFunctionBreakpoints").empty());
    EXPECT_TRUE(adapters.front()->argumentsOf("setExceptionBreakpoints").empty());
    primary->stop();
    ASSERT_TRUE(adapters.front()->waitForRequest("disconnect"));
    EXPECT_EQ(adapters.front()->argumentsOf("disconnect").front().boolAt("terminateDebuggee"), false);
}

TEST_F(DebugSessionsTest, OutgoingResumeAndItsAckCannotWithdrawActualNativeStopEvidence) {
    ASSERT_TRUE(catalog.add(source, 10));
    const auto id = launch("Native paused");
    auto* controller = sessions->controller(id);
    ASSERT_TRUE(pump([&] { return controller->paused() && controller->selectedFrame(); }));
    EXPECT_TRUE(controller->adapterStoppedEvidence());
    adapters.back()->on("continue", [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{}; });
    int continued = 0;
    QObject::connect(controller, &DebugController::adapterContinued, controller, [&](std::uint64_t generation) {
        EXPECT_EQ(generation, controller->sessionGeneration());
        ++continued;
    });
    ASSERT_TRUE(controller->resume());
    ASSERT_TRUE(adapters.back()->waitForRequest("continue"));
    ASSERT_TRUE(pump([&] { return !controller->paused(); }));
    EXPECT_EQ(continued, 0);
    EXPECT_TRUE(controller->adapterStoppedEvidence());
    adapters.back()->sendEvent("continued", rpc::Json::object().set("threadId", 1).set("allThreadsContinued", true));
    ASSERT_TRUE(pump([&] { return continued == 1; }));
    EXPECT_FALSE(controller->adapterStoppedEvidence());
    adapters.back()->sendEvent("stopped", rpc::Json::object().set("reason", "breakpoint").set("threadId", 1));
    ASSERT_TRUE(pump([&] { return controller->paused(); }));
    const auto stoppedSequence = controller->adapterEvidenceSequence();
    adapters.back()->on("continue", [](const rpc::Json&) {
        return debug::FakeDebugAdapter::Reply{.success = false, .message = "resume refused"};
    });
    ASSERT_TRUE(controller->resume());
    ASSERT_TRUE(adapters.back()->waitForRequest("continue", 2));
    ASSERT_TRUE(pump([&] { return controller->paused(); }));
    EXPECT_EQ(controller->adapterEvidenceSequence(), stoppedSequence); // UI restoration is not another adapter stop.
    EXPECT_TRUE(controller->adapterStoppedEvidence());
}

TEST_F(DebugSessionsTest, OnlySelectedSessionPresentsExecutionPointAndGutterClickRunsOnce) {
    FROST_REQUIRE_OFFSCREEN();
    test::HarnessOptions options;
    options.path = source;
    test::EditorHarness editor(std::string(40, '\n'), options);
    sessions->attachEditor(editor.view.get());
    ASSERT_TRUE(catalog.add(source, 10));
    const auto first = launch("First");
    const auto second = launch("Second");
    auto* a = sessions->controller(first);
    auto* b = sessions->controller(second);
    ASSERT_TRUE(pump([&] { return a->paused() && a->selectedFrame() && b->state() == debug::DebugState::Running; }));
    ASSERT_TRUE(b->pause());
    ASSERT_TRUE(pump([&] { return b->paused() && b->selectedFrame(); }));
    EXPECT_EQ(editor.view->executionPoint(), std::optional<LineIndex>(LineIndex{19}));
    ASSERT_TRUE(sessions->select(first));
    EXPECT_EQ(editor.view->executionPoint(), std::optional<LineIndex>(LineIndex{9}));
    // Late events in the background session update its own frame, never the selected marker.
    adapters[2]->sendEvent("continued", rpc::Json::object().set("threadId", 1));
    ASSERT_TRUE(pump([&] { return b->state() == debug::DebugState::Running; }));
    EXPECT_EQ(editor.view->executionPoint(), std::optional<LineIndex>(LineIndex{9}));
    const auto before = catalog.size();
    emit editor.view->breakpointToggleRequested(29);
    EXPECT_EQ(catalog.size(), before + 1);
    EXPECT_EQ(a->breakpoints().size(), catalog.size());
    EXPECT_EQ(b->breakpoints().size(), catalog.size());
    sessions->detachEditor(editor.view.get());
}

TEST_F(DebugSessionsTest, PendingVerificationIsNeverPresentedAsVerifiedInALiveSelectedSession) {
    FROST_REQUIRE_OFFSCREEN();
    test::HarnessOptions options;
    options.path = source;
    test::EditorHarness editor(std::string(40, '\n'), options);
    sessions->attachEditor(editor.view.get());
    const auto id = launch("Pending");
    ASSERT_TRUE(pump([&] { return sessions->controller(id)->state() == debug::DebugState::Running; }));
    adapters.back()->on("setBreakpoints",
                        [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{.respond = false}; });
    ASSERT_TRUE(sessions->controller(id)->breakpoints().add(source, 10));
    ASSERT_TRUE(adapters.back()->waitForRequest("setBreakpoints"));
    ASSERT_EQ(editor.view->breakpointMarkers().size(), 1U);
    EXPECT_EQ(editor.view->breakpointMarkers().front().kind, BreakpointKind::Unverified);
    EXPECT_NE(editor.view->breakpointMarkers().front().message.find("Waiting"), std::string::npos);
    sessions->detachEditor(editor.view.get());
}

TEST_F(DebugSessionsTest, LazyToolWindowReusesTheExistingConsoleAndItsHistory) {
    QWidget parking;
    auto* existing = new RunConsole(&parking);
    primary->setConsole(existing);
    existing->appendOutput(build::RunOutputKind::Stdout, "before-tool-window\n");
    {
        DebugToolWindow window(*primary);
        EXPECT_EQ(window.console(), existing);
        EXPECT_TRUE(window.console()->text().contains(QStringLiteral("before-tool-window")));
        EXPECT_TRUE(window.isAncestorOf(existing));
        EXPECT_EQ(window.findChildren<RunConsole*>().size(), 1);
    }
    EXPECT_EQ(primary->console(), nullptr);
}

TEST_F(DebugSessionsTest, SessionViewsRetainSeparateConsolesAndStableIdsDoNotRepeat) {
    DebugSessionsWindow window(*sessions);
    const auto first = launch("First");
    const auto second = launch("Second");
    ASSERT_NE(window.view(first), nullptr);
    ASSERT_NE(window.view(second), nullptr);
    EXPECT_NE(window.view(first)->console(), window.view(second)->console());
    adapters[1]->sendEvent("output", rpc::Json::object().set("category", "stdout").set("output", "first-only"));
    ASSERT_TRUE(pump([&] { return window.view(first)->console()->text().contains(QStringLiteral("first-only")); }));
    EXPECT_FALSE(window.view(second)->console()->text().contains(QStringLiteral("first-only")));
    sessions->controller(first)->stop();
    ASSERT_TRUE(pump([&] { return !sessions->controller(first)->debugging(); }));
    ASSERT_TRUE(sessions->remove(first));
    EXPECT_EQ(window.view(first), nullptr);
    const auto replacement = launch("Replacement");
    EXPECT_GT(replacement, second);
    EXPECT_EQ(sessions->controller(first), nullptr);
}

TEST_F(DebugSessionsTest, NativePidLookupRefusesAmbiguityAndSelectionDoesNotChangeAuthority) {
    const auto first = launch("First");
    const auto second = launch("Second");
    adapters[1]->sendEvent("process", rpc::Json::object().set("systemProcessId", 1234));
    adapters[2]->sendEvent("process", rpc::Json::object().set("systemProcessId", 5678));
    ASSERT_TRUE(
        pump([&] { return sessions->nativeForPid(1234).id == first && sessions->nativeForPid(5678).id == second; }));
    ASSERT_TRUE(sessions->select(second));
    EXPECT_EQ(sessions->nativeForPid(1234).id, first);
    adapters[2]->sendEvent("process", rpc::Json::object().set("systemProcessId", 1234));
    ASSERT_TRUE(pump([&] { return sessions->nativeForPid(1234).id == 0; }));
}

TEST_F(DebugSessionsTest, ShutdownRetiresAttachOffGuiAndKeepsTransportUntilItsActualAck) {
    auto created = sessions->createSession(QStringLiteral("Attached"));
    ASSERT_TRUE(created);
    auto* peer = adapters.back().get();
    peer->on("attach",
             [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json()}}}; });
    peer->on("disconnect", [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{.respond = false}; });
    ASSERT_TRUE(sessions->controller(*created)->attachTarget(target("Attached"), debug::test::fakeFactory(*peer),
                                                             rpc::Json::object().set("type", "lldb-dap")));
    ASSERT_TRUE(pump([&] { return sessions->controller(*created)->state() == debug::DebugState::Running; }));
    sessions->shutdown();
    EXPECT_FALSE(sessions->controller(*created)->debugging());
    EXPECT_GT(retirements->pending(), 0U); // Shutdown returned while the peer deliberately withholds ACK.
    ASSERT_TRUE(peer->waitForRequest("disconnect"));
    EXPECT_FALSE(peer->closedByClient());
    for (const auto& request : peer->requests()) {
        if (request.stringAt("command") == "disconnect") {
            peer->sendRaw(rpc::encodeFrame(rpc::Json::object()
                                               .set("seq", 2000)
                                               .set("type", "response")
                                               .set("command", "disconnect")
                                               .set("request_seq", request.integerAt("seq").value())
                                               .set("success", true)
                                               .set("body", rpc::Json::object())
                                               .dump()));
            break;
        }
    }
    ASSERT_TRUE(pump([&] { return retirements->pending() == 0; }));
    EXPECT_EQ(peer->argumentsOf("disconnect").front().boolAt("terminateDebuggee"), false);
}

TEST_F(DebugSessionsTest, StopAllTerminatesLaunchesAndDetachesOrdinaryAttachedHosts) {
    const auto launched = launch("Owned");
    auto created = sessions->createSession(QStringLiteral("Attached"));
    ASSERT_TRUE(created);
    auto* peer = adapters.back().get();
    // The factory was already created with this peer and is consumed by attachTarget below.
    peer->on("attach",
             [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json()}}}; });
    ASSERT_TRUE(sessions->controller(*created)->attachTarget(target("Attached"), debug::test::fakeFactory(*peer),
                                                             rpc::Json::object().set("type", "lldb-dap")));
    ASSERT_TRUE(pump([&] { return sessions->controller(*created)->state() == debug::DebugState::Running; }));
    sessions->stopAll();
    ASSERT_TRUE(pump([&] { return !sessions->anyDebugging(); }));
    ASSERT_FALSE(adapters[1]->argumentsOf("disconnect").empty());
    ASSERT_FALSE(peer->argumentsOf("disconnect").empty());
    EXPECT_EQ(adapters[1]->argumentsOf("disconnect").back().boolAt("terminateDebuggee"), true);
    EXPECT_EQ(peer->argumentsOf("disconnect").back().boolAt("terminateDebuggee"), false);
    EXPECT_FALSE(sessions->controller(launched)->debugging());
}

TEST_F(DebugSessionsTest, RollbackHasNoUserStopSignalAndExplicitGroupStopSurvivesReentrantCancellation) {
    std::vector<DebugSessionId> explicitStops;
    QObject::connect(sessions.get(), &DebugSessionRegistry::explicitStopRequested, sessions.get(),
                     [&](auto id) { explicitStops.push_back(id); });
    const auto failedGroup = sessions->createGroup();
    auto failedChild = sessions->startTarget(target("Automatic rollback"), failedGroup);
    ASSERT_TRUE(failedChild);
    sessions->rollbackGroup(failedGroup);
    EXPECT_TRUE(explicitStops.empty());
    ASSERT_TRUE(pump([&] { return !sessions->controller(*failedChild)->debugging(); }));
    const auto selected = launch("User-selected Stop");
    ASSERT_TRUE(sessions->select(selected));
    sessions->stopSelected();
    EXPECT_EQ(explicitStops, std::vector<DebugSessionId>{selected});
    ASSERT_TRUE(pump([&] { return !sessions->controller(selected)->debugging(); }));
    explicitStops.clear();
    const auto group = sessions->createGroup();
    auto child = sessions->startTarget(target("User group Stop"), group);
    ASSERT_TRUE(child);
    QObject::connect(sessions.get(), &DebugSessionRegistry::groupStopRequested, sessions.get(),
                     [&](auto id) { sessions->stopGroup(id); });
    sessions->stopAll();
    EXPECT_EQ(explicitStops, std::vector<DebugSessionId>{*child});
    ASSERT_TRUE(pump([&] { return !sessions->controller(*child)->debugging(); }));
    explicitStops.clear();
    const auto consoleSession = launch("Console Stop");
    ASSERT_NE(sessions->controller(consoleSession)->console(), nullptr);
    emit sessions->controller(consoleSession)->console()->stopRequested();
    EXPECT_EQ(explicitStops, std::vector<DebugSessionId>{consoleSession});
    ASSERT_TRUE(pump([&] { return !sessions->controller(consoleSession)->debugging(); }));
}
} // namespace
} // namespace frost::ui
