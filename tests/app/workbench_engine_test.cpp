// SPDX-License-Identifier: Apache-2.0
// Workbench R6 session regressions through a real local HMAC-v2 socket peer. Historical test names
// stay traceable where behavior remains; real-editor oracle cases live in mandatory corpus acceptance.
#include "frost/build/cmake_service.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/engine_console.hpp"

#include "engine_request_helper.hpp"
#include "engine_session_peer.hpp"
#include "workbench_rig.hpp"
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#endif

namespace {
using namespace std::chrono_literals;
using namespace frost::app::test;
using Json = frost::rpc::Json;
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
// A physical directory alias: Windows junctions require no symlink privilege. Failure is a
// fixture failure, never a skipped canonical-identity regression. ScratchDir unlinks aliases.
void makeDirectoryAlias(const std::string& target, const std::string& alias) {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), MAX_PATH);
    ASSERT_GT(length, 0U);
    ASSERT_LT(length, system.size());
    const auto systemPath = frost::io::fromWide(std::wstring_view(system.data(), length));
    ASSERT_TRUE(systemPath);
    frost::io::ProcessSpec spec;
    spec.executable = *systemPath + "\\cmd.exe";
    spec.arguments = {"/d", "/c", "mklink", "/J", alias, target};
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    const auto made = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, "", 30s);
    ASSERT_TRUE(made) << made.error().describe();
    ASSERT_EQ(made->exitCode, 0) << made->stdoutBytes;
#else
    const auto targetPath = frost::io::toStdPath(target), aliasPath = frost::io::toStdPath(alias);
    ASSERT_TRUE(targetPath && aliasPath);
    std::error_code error;
    std::filesystem::create_directory_symlink(*targetPath, *aliasPath, error);
    ASSERT_FALSE(error) << error.message();
#endif
    const auto targetPathForOracle = frost::io::toStdPath(target);
    const auto aliasPathForOracle = frost::io::toStdPath(alias);
    ASSERT_TRUE(targetPathForOracle && aliasPathForOracle);
    std::error_code targetError, aliasError;
    const auto physicalTarget = std::filesystem::canonical(*targetPathForOracle, targetError);
    const auto physicalAlias = std::filesystem::canonical(*aliasPathForOracle, aliasError);
    ASSERT_FALSE(targetError) << targetError.message();
    ASSERT_FALSE(aliasError) << aliasError.message();
    ASSERT_EQ(physicalAlias, physicalTarget);
}

#ifdef _WIN32
// A real NTFS file alias with an unchanged parent, unlike retargeting a directory junction.
DWORD setProjectShortName(const std::filesystem::path& target, const wchar_t* name) {
    const HANDLE file = CreateFileW(target.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError();
    const BOOL changed = SetFileShortNameW(file, name);
    const DWORD error = changed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return error;
}
#endif

void advertise(const WorkDir& dir, const EngineSessionPeer& peer, std::string_view project = {}) {
    dir.write("automation/Instances/" + peer.instanceId() + ".json", peer.record(project).dump());
}
RigOptions peerOptions(const WorkDir& dir, frost::io::IProcessLauncher* launcher = nullptr) {
    RigOptions options;
    options.disableProjectEvaluation = true;
    options.launcher = launcher;
    options.workbench.engineAutomationDirectory = dir.join("automation");
    return options;
}
bool openTrusted(WorkbenchRig& rig, const std::string& project) {
    EXPECT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  project + "/missing-lua-server"));
    EXPECT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  project + "/missing-managed-server.dll"));
    auto trusted = rig.trust->trust(project);
    EXPECT_TRUE(trusted);
    if (!trusted)
        return false;
    auto opened = rig.workbench->openPath(project);
    EXPECT_TRUE(opened);
    return opened.has_value();
}
bool listed(WorkbenchRig& rig, std::string_view id) {
    const auto snapshot = rig.workbench->engineState();
    const auto* instances = snapshot.find("instances");
    return instances && std::ranges::any_of(instances->items(), [&](const Json& instance) {
               return instance.stringAt("instanceId") == id;
           });
}
bool pairPeer(WorkbenchRig& rig, const EngineSessionPeer& peer) {
    EXPECT_NE(peer.port(), 0U);
    if (!waitFor([&] { return listed(rig, peer.instanceId()); }, 5s)) {
        ADD_FAILURE() << "selected peer was not discovered";
        return false;
    }
    auto paired = rig.workbench->pairEngineInstance(peer.instanceId());
    EXPECT_TRUE(paired) << (paired ? "" : paired.error().message());
    return paired && waitFor(
                         [&] {
                             const auto value = rig.workbench->engineState();
                             return value.boolAt("connected") == true && value.stringAt("state") == "Edit";
                         },
                         5s);
}
frost::ui::EngineConsole* console(WorkbenchRig& rig) {
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    return panel ? panel->findChild<frost::ui::EngineConsole*>() : nullptr;
}
QTreeWidgetItem* rowContaining(frost::ui::EngineConsole* output, const QString& text) {
    for (int i = 0; i < output->view()->topLevelItemCount(); ++i) {
        auto* item = output->view()->topLevelItem(i);
        if (item->text(3).contains(text))
            return item;
    }
    return nullptr;
}
bool completed(WorkbenchRig& rig, std::uint64_t id) {
    const auto value = rig.workbench->engineState();
    const auto* result = value.find("lastReply");
    return value.unsignedAt("lastCompletedRequestId") == id && result && !result->isNull();
}
// Isolate CLI discovery from lifecycle scripts: the dummy executable is a real filesystem
// locator input, while only RunManager launches consume the controlled process-lifecycle queue.
class EngineRunLauncher final : public frost::io::IProcessLauncher {
public:
    frost::io::ScriptedProcessLauncher runs;
    frost::io::ScriptedProcessLauncher cli;
    std::function<void(const frost::io::ProcessSpec&)> afterRunLaunch;
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        if (!spec.arguments.empty() &&
            (spec.arguments.front() == "project.info" || spec.arguments.front() == "assets.list" ||
             spec.arguments.front() == "schema.export")) {
            cli.expect(frost::io::ScriptedProcess{
                .launchError = frost::Error(frost::ErrorCode::Unsupported,
                                            "the lifecycle fixture does not provide a project CLI")});
            return cli.launch(spec);
        }
        auto process = runs.launch(spec);
        if (process && afterRunLaunch)
            afterRunLaunch(spec);
        return process;
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        return runs.launchAsync(spec, context);
    }
};
std::string editorProgram(const WorkDir& dir) {
#ifdef _WIN32
    constexpr std::string_view relative = "bin/FrostEditor.exe";
#else
    constexpr std::string_view relative = "bin/FrostEditor";
#endif
    dir.write(relative, "scripted lifecycle executable");
    return dir.join(relative);
}
RigOptions runOptions(const WorkDir& dir, EngineRunLauncher& launcher, const std::string& program) {
    auto options = peerOptions(dir, &launcher);
    options.workbench.frostEditorPath = program;
    options.workbench.engineConfigDirectory = dir.join("private-engine-config");
    return options;
}
bool waitForConfiguration(WorkbenchRig& rig) {
    return waitFor(
        [&] {
            const auto* controller = rig.workbench->runController();
            return controller && std::ranges::any_of(controller->configurations().all(), [](const auto& configuration) {
                       return configuration.type == frost::run::RunType::FrostEditor;
                   });
        },
        3s);
}
bool runReservationReleased(WorkbenchRig& rig) {
    const auto state = rig.workbench->engineState();
    return state.boolAt("launchReserved") == false && state.unsignedAt("ownedRunId") == 0 &&
           !rig.workbench->runController()->running();
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
    options.disableProjectEvaluation = true;
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
    options.disableProjectEvaluation = true;
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

TEST(WorkbenchEngine, RefusesUntrustedAndCancelsPendingOnProjectSwitch) {
    WorkDir dir("engine-trust-switch");
    dir.write("first/Game.frostproj", "{}");
    dir.write("second/Other.frostproj", "{}");
    EngineSessionPeer peer(dir.join("first"));
    peer.hold("play.start");
    advertise(dir, peer);
    frost::io::ScriptedProcessLauncher launcher;
    WorkbenchRig rig(dir, peerOptions(dir, &launcher));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("first")));
    auto denied = rig.workbench->sendEngineCommand("play.start", Json::object());
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), frost::ErrorCode::Untrusted);
    EXPECT_TRUE(peer.requests("auth.challenge").empty());
    EXPECT_TRUE(launcher.launches().empty());
    ASSERT_TRUE(openTrusted(rig, dir.join("first")));
    ASSERT_TRUE(pairPeer(rig, peer));
    ASSERT_TRUE(rig.workbench->sendEngineCommand("play.start", Json::object()));
    ASSERT_TRUE(waitFor([&] { return peer.requests("play.start").size() == 1; }, 3s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("second")));
    ASSERT_TRUE(waitFor([&] { return peer.closed.load(); }, 3s));
    EXPECT_FALSE(rig.workbench->engineState().boolAt("connected").value_or(true));
    EXPECT_EQ(peer.requests("play.start").size(), 1U);
    EXPECT_TRUE(launcher.launches().empty());
}
TEST(WorkbenchEngine, LateCompletionCannotPopulateAnotherProject) {
    WorkDir dir("engine-stale-completion");
    dir.write("first/Game.frostproj", "{}");
    dir.write("second/Other.frostproj", "{}");
    EngineSessionPeer peer(dir.join("first"));
    peer.hold("play.state");
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("first")));
    ASSERT_TRUE(pairPeer(rig, peer));
    auto* output = console(rig);
    ASSERT_NE(output, nullptr);
    ASSERT_TRUE(rig.workbench->sendEngineCommand("play.state", Json::object()));
    ASSERT_TRUE(waitFor([&] { return peer.requests("play.state").size() == 1; }, 3s));
    peer.release("play.state", Json::object().set("late", "old-project-reply"));
    // Let the peer write while the GUI thread deliberately does not drain queued callbacks.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (peer.replies("play.state") == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(peer.replies("play.state"), 1U);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("second")));
    static_cast<void>(waitFor([] { return false; }, 200ms));
    EXPECT_EQ(output->retainedEntries(), 0U);
    EXPECT_EQ(rig.workbench->projectRoot(), dir.join("second"));
    EXPECT_TRUE(rig.workbench->engineState().find("lastReply")->isNull());
}
TEST(WorkbenchEngine, ShutdownCancelsStalledSessionAndPermanentlyRefusesRequests) {
    WorkDir dir("engine-shutdown");
    dir.write("project/Game.frostproj", "{}");
    EngineSessionPeer peer(dir.join("project"));
    peer.stallChallenge = true;
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(waitFor([&] { return listed(rig, peer.instanceId()); }, 3s));
    ASSERT_TRUE(rig.workbench->pairEngineInstance(peer.instanceId()));
    ASSERT_TRUE(waitFor([&] { return peer.requests("auth.challenge").size() == 1; }, 3s));
    const auto start = std::chrono::steady_clock::now();
    rig.workbench->shutdown();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 4s);
    EXPECT_TRUE(waitFor([&] { return peer.closed.load(); }, 2s));
    EXPECT_FALSE(rig.workbench->sendEngineCommand("play.start", Json::object()));
    EXPECT_FALSE(rig.workbench->pairEngineInstance(peer.instanceId()));
    EXPECT_TRUE(peer.requests("play.start").empty());
}
TEST(WorkbenchEngine, DiscoveryFindsAliasRecordInsteadOfOtherProjectFallback) {
    WorkDir dir("engine-alias-record");
    dir.write("project/Game.frostproj", "{}");
    dir.write("other/Other.frostproj", "{}");
    ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("project"), dir.join("alias")));
    EngineSessionPeer peer(dir.join("project"));
    EngineSessionPeer other(dir.join("other"), 'b');
    advertise(dir, peer, dir.join("alias"));
    dir.write("automation/Automation.json", other.record().dump());
    advertise(dir, other);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(waitFor([&] { return listed(rig, peer.instanceId()); }, 3s));
    const auto snapshot = rig.workbench->engineState();
    ASSERT_NE(snapshot.find("instances"), nullptr);
    ASSERT_EQ(snapshot.find("instances")->size(), 1U);
    EXPECT_EQ(snapshot.find("instances")->items().front().stringAt("instanceId"), peer.instanceId());
    EXPECT_TRUE(other.requests("auth.challenge").empty());
    // Discovery compares physical project identity. Authentication separately preserves the exact
    // server-signed canonical wire path, so refresh that truthful record before pairing.
    advertise(dir, peer);
    ASSERT_TRUE(rig.workbench->refreshEngineInstances());
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->engineState().boolAt("discoveryBusy").value_or(true); }, 3s));
    ASSERT_TRUE(pairPeer(rig, peer));
    EXPECT_EQ(peer.authenticated.load(), 1U);
    EXPECT_TRUE(other.requests("auth.challenge").empty());
}
TEST(WorkbenchEngine, PairsCanonicalRecordWhenWorkspaceWasOpenedThroughAlias) {
    WorkDir dir("engine-alias-workspace");
    dir.write("project/Game.frostproj", "{}");
    dir.write("other/Other.frostproj", "{}");
    ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("project"), dir.join("alias")));
    EngineSessionPeer peer(dir.join("project"));
    EngineSessionPeer other(dir.join("other"), 'b');
    advertise(dir, peer);
    advertise(dir, other);
    dir.write("automation/Automation.json", other.record().dump());
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("alias")));
    ASSERT_TRUE(pairPeer(rig, peer));
    EXPECT_EQ(peer.authenticated.load(), 1U);
    EXPECT_TRUE(other.requests("auth.challenge").empty());
    const auto snapshot = rig.workbench->engineState();
    EXPECT_EQ(snapshot.stringAt("selectedInstanceId"), peer.instanceId());
    EXPECT_EQ(snapshot.boolAt("connected"), true);
}
TEST(WorkbenchEngine, PeerPendingSocketDoesNotOutliveWorkbench) {
    WorkDir dir("engine-stalled-socket");
    dir.write("project/Game.frostproj", "{}");
    EngineSessionPeer peer(dir.join("project"));
    peer.hold("play.state");
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, peer));
    ASSERT_TRUE(rig.workbench->sendEngineCommand("play.state", Json::object()));
    ASSERT_TRUE(waitFor([&] { return peer.requests("play.state").size() == 1; }, 3s));
    const auto start = std::chrono::steady_clock::now();
    rig.workbench->shutdown();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 4s);
    ASSERT_TRUE(waitFor([&] { return peer.closed.load(); }, 2s));
    EXPECT_FALSE(rig.workbench->sendEngineCommand("play.state", Json::object()));
    EXPECT_EQ(peer.requests("play.state").size(), 1U);
    rig.window.reset();
    rig.workbench.reset();
}
TEST(WorkbenchEngine, RevokedTrustStopsLiveSessionBeforeAction) {
    WorkDir dir("engine-revocation");
    dir.write("project/Game.frostproj", "{}");
    EngineSessionPeer peer(dir.join("project"));
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, peer));
    auto* output = console(rig);
    ASSERT_NE(output, nullptr);
    peer.hold("play.state");
    ASSERT_TRUE(rig.workbench->sendEngineCommand("play.state", Json::object()));
    ASSERT_TRUE(waitFor([&] { return peer.requests("play.state").size() == 1; }, 3s));
    ASSERT_TRUE(rig.trust->untrust(dir.join("project")));
    auto refused = rig.workbench->sendEngineCommand("play.start", Json::object());
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Untrusted);
    peer.release("play.state", Json::object().set("late", "revoked-trust-reply"));
    EXPECT_TRUE(waitFor([&] { return peer.closed.load(); }, 3s));
    static_cast<void>(waitFor([] { return false; }, 100ms));
    EXPECT_TRUE(peer.requests("play.start").empty());
    EXPECT_EQ(output->retainedEntries(), 0U);
    EXPECT_FALSE(rig.workbench->engineState().boolAt("connected").value_or(true));
}
TEST(WorkbenchEngine, LogRetentionEvictsExplicitlyAndProjectSwitchClearsResults) {
    WorkDir dir("engine-log-retention");
    dir.write("first/Game.frostproj", "{}");
    dir.write("second/Other.frostproj", "{}");
    EngineSessionPeer peer(dir.join("first"));
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("first")));
    ASSERT_TRUE(pairPeer(rig, peer));
    auto* output = console(rig);
    ASSERT_NE(output, nullptr);
    // Pace pushed events through the real app ingress so this tests console retention rather
    // than its separate 512-event ingress overflow. R6 retains at most 5,000 console records.
    for (int begin = 0; begin < 5020; begin += 20) {
        for (int i = begin; i < begin + 20; ++i)
            peer.log("entry " + std::to_string(i));
        ASSERT_TRUE(waitFor(
            [&] {
                return output->retainedEntries() + output->droppedEntries() >= static_cast<std::size_t>(begin + 20);
            },
            3s));
    }
    EXPECT_EQ(output->retainedEntries(), 5000U);
    EXPECT_GE(output->droppedEntries(), 20U);
    EXPECT_LE(output->retainedBytes(), 4U * 1024U * 1024U);
    EXPECT_TRUE(hasStatus(output, QStringLiteral("discarded")));
    EXPECT_TRUE(peer.requests("log.get").empty()); // Live log stream does not poll pages.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("second")));
    EXPECT_EQ(output->retainedEntries(), 0U);
    EXPECT_EQ(output->droppedEntries(), 0U);
}
TEST(WorkbenchEngine, ResultActivationRevalidatesLocationBeforeOpening) {
    WorkDir dir("engine-location");
    dir.write("project/Game.frostproj", "{}");
    dir.write("project/Assets/Scripts/Test.lua", "local a = 1\nlocal b = 2\n");
    EngineSessionPeer peer(dir.join("project"));
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, peer));
    auto* output = console(rig);
    ASSERT_NE(output, nullptr);
    peer.log("failure", "Assets/Scripts/Test.lua", 2);
    ASSERT_TRUE(waitFor([&] { return rowContaining(output, QStringLiteral("failure")) != nullptr; }, 3s));
    auto* item = rowContaining(output, QStringLiteral("failure"));
    ASSERT_GT(item->childCount(), 0);
    EXPECT_TRUE(output->activate(item));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->currentView() != nullptr; }, 3s));
    EXPECT_EQ(rig.workbench->currentView()->status().line, 2);
    EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("project/Assets/Scripts/Test.lua"));
    // A second event resolved earlier must be checked against the filesystem again on activation.
    peer.log("removed source", "Assets/Scripts/Test.lua", 1);
    ASSERT_TRUE(waitFor([&] { return rowContaining(output, QStringLiteral("removed source")) != nullptr; }, 3s));
    ASSERT_TRUE(frost::io::realFileSystem().removeFile(dir.join("project/Assets/Scripts/Test.lua")));
    EXPECT_TRUE(output->activate(rowContaining(output, QStringLiteral("removed source"))));
    ASSERT_TRUE(waitFor(
        [&] { return hasStatus(rig.window->toolWindows().content("frostEngine"), QStringLiteral("Engine source")); },
        3s));
    EXPECT_EQ(rig.workbench->currentView()->status().line, 2);
    EXPECT_TRUE(peer.requests("navigation.resolve").empty()); // Local worker revalidates physical containment.
}
TEST(WorkbenchEngine, RealDirectoryAliasCannotExposeOutOfProjectDiagnosticLocations) {
    WorkDir dir("engine-physical-diagnostic");
    dir.write("project/Game.frostproj", "{}");
    dir.write("project/Assets/Inside/Script.lua", "return {}\n");
    dir.write("outside/Script.lua", "return 'outside'\n");
    ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("outside"), dir.join("project/Assets/Escape")));
    ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("project/Assets/Inside"), dir.join("project/Assets/Alias")));
    EngineSessionPeer peer(dir.join("project"));
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, peer));
    auto* output = console(rig);
    ASSERT_NE(output, nullptr);
    peer.log("escaped diagnostic", "Assets/Escape/Script.lua", 1);
    ASSERT_TRUE(waitFor([&] { return rowContaining(output, QStringLiteral("escaped diagnostic")) != nullptr; }, 3s));
    auto* escaped = rowContaining(output, QStringLiteral("escaped diagnostic"));
    EXPECT_EQ(escaped->childCount(), 0);
    EXPECT_FALSE(output->activate(escaped));
    EXPECT_EQ(rig.workbench->currentView(), nullptr);
    peer.log("in-project alias diagnostic", "Assets/Alias/Script.lua", 1);
    ASSERT_TRUE(
        waitFor([&] { return rowContaining(output, QStringLiteral("in-project alias diagnostic")) != nullptr; }, 3s));
    auto* inside = rowContaining(output, QStringLiteral("in-project alias diagnostic"));
    ASSERT_GT(inside->childCount(), 0);
    EXPECT_TRUE(output->activate(inside));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->currentView() != nullptr; }, 3s));
    EXPECT_EQ(rig.workbench->currentView()->path(),
              frost::io::realFileSystem().physicalPath(dir.join("project/Assets/Inside/Script.lua")).value());
    EXPECT_TRUE(peer.requests("navigation.resolve").empty());
}

TEST(WorkbenchEngine, RetargetingRealWorkspaceAliasRevokesItsOriginalPhysicalAdmission) {
    WorkDir dir("engine-physical-root-retarget");
    dir.write("first/Game.frostproj", "{}");
    dir.write("second/Game.frostproj", "{}");
    ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("first"), dir.join("alias")));
    EngineSessionPeer peer(dir.join("first"));
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("alias")));
    ASSERT_TRUE(pairPeer(rig, peer));
    ASSERT_TRUE(frost::io::realFileSystem().removeFile(dir.join("alias")));
    ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("second"), dir.join("alias")));
    const auto requested = rig.workbench->sendEngineCommand("editor.focus", Json::object());
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("physicalRootValid") == false; }, 3s));
    if (requested)
        ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().unsignedAt("pendingRequestCount") == 0U; }, 3s));
    EXPECT_TRUE(peer.requests("editor.focus").empty());
    // Restoring the old target does not revive an already invalidated admission.
    ASSERT_TRUE(frost::io::realFileSystem().removeFile(dir.join("alias")));
    ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("first"), dir.join("alias")));
    EXPECT_FALSE(rig.workbench->sendEngineCommand("editor.focus", Json::object()));
    EXPECT_TRUE(peer.requests("editor.focus").empty());
}

TEST(WorkbenchEngine, ActualSessionHelperRejectsTrustedAliasRetargetAfterAuthenticationBeforeCommandWrite) {
    WorkDir dir("engine-helper-physical-admission");
    dir.write("first/Game.frostproj", "{}");
    dir.write("second/Game.frostproj", "{}");
    ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("first"), dir.join("alias")));
    auto& fs = frost::io::realFileSystem();
    const auto trustPath = dir.join("trusted-roots.txt");
    auto trust = frost::project::TrustStore::open(fs, trustPath);
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(dir.join("alias")));
    std::atomic<bool> retargeted{false}; // Outlives the peer's joined callback thread.
    EngineSessionPeer peer(dir.join("first"));
    advertise(dir, peer);
    peer.setBeforeReply([&](std::string_view method) {
        if (method != "schema.export")
            return;
        // Schema arrives during the authenticated handshake, before the helper submits the
        // requested editor.focus. Keep the replacement present until admission observes it.
        EXPECT_GT(peer.authenticated.load(), 0U);
        ASSERT_TRUE(fs.removeFile(dir.join("alias")));
        ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("second"), dir.join("alias")));
        retargeted = true;
    });
    const auto input = Json::object()
                           .set("root", dir.join("alias"))
                           .set("directory", dir.join("automation"))
                           .set("trustPath", trustPath)
                           .set("method", "editor.focus")
                           .set("params", Json::object());
    auto completed = std::async(std::launch::async, [&] { return frost::app::executeEngineHelper(input); });
    ASSERT_EQ(completed.wait_for(15s), std::future_status::ready);
    const auto result = completed.get();
    ASSERT_TRUE(retargeted.load());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Untrusted);
    EXPECT_TRUE(peer.requests("editor.focus").empty());
    EXPECT_TRUE((*trust)->isTrusted(dir.join("alias"))) << "the lexical authorization itself did not change";
    ASSERT_TRUE(fs.removeFile(dir.join("alias")));
    ASSERT_NO_FATAL_FAILURE(makeDirectoryAlias(dir.join("first"), dir.join("alias")));
    EXPECT_TRUE(peer.requests("editor.focus").empty());
}

TEST(WorkbenchEngine, SessionPeerValidatesUnsavedBufferAndReportsState) {
    WorkDir dir("engine-buffer-state");
    dir.write("project/Game.frostproj", "{}");
    dir.write("project/Assets/Scripts/AutomationTest.lua", "return {}\n");
    EngineSessionPeer peer(dir.join("project"));
    peer.result("script.validate", Json::object().set("valid", false).set("message", "Script validation failed"));
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, peer));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project/Assets/Scripts/AutomationTest.lua")));
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->editor().typeText("this is invalid Lua!\n"));
    ASSERT_TRUE(view->document().dirty());
    const auto snapshot = view->editor().document().text().text();
    ASSERT_NE(console(rig), nullptr);
    auto* validate = button(rig.window->toolWindows().content("frostEngine"), QStringLiteral("Validate Script"));
    ASSERT_NE(validate, nullptr);
    validate->click();
    ASSERT_TRUE(view->editor().typeText("-- changed after validation was submitted\n"));
    ASSERT_TRUE(waitFor([&] { return peer.requests("script.validate").size() == 1; }, 3s));
    const auto requests = peer.requests("script.validate");
    const auto* params = requests.front().find("params");
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->stringAt("path"), "Assets/Scripts/AutomationTest.lua");
    EXPECT_EQ(params->stringAt("source"), snapshot);
    EXPECT_NE(view->editor().document().text().text(), snapshot);
    const auto validationId = rig.workbench->engineState().unsignedAt("lastRequestId");
    ASSERT_TRUE(validationId);
    ASSERT_TRUE(waitFor([&] { return completed(rig, *validationId); }, 3s));
    EXPECT_EQ(rig.workbench->engineState().find("lastReply")->boolAt("valid"), false);
    EXPECT_TRUE(view->document().dirty());
    EXPECT_EQ(dir.read("project/Assets/Scripts/AutomationTest.lua"), "return {}\n");
    // A successful command response does not fabricate toolbar state; only push events change it.
    auto play = rig.workbench->sendEngineCommand("play.start", Json::object());
    ASSERT_TRUE(play);
    ASSERT_TRUE(waitFor([&] { return completed(rig, *play); }, 3s));
    EXPECT_EQ(rig.workbench->engineState().stringAt("state"), "Edit");
    peer.state("Play", false, 7);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().stringAt("state") == "Play"; }, 3s));
    EXPECT_TRUE(rig.window->mainToolbar().engineState()->playing);
    auto stopped = rig.workbench->sendEngineCommand("play.stop", Json::object());
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(waitFor([&] { return completed(rig, *stopped); }, 3s));
    peer.state("Edit", false, 7);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().stringAt("state") == "Edit"; }, 3s));
    EXPECT_FALSE(rig.window->mainToolbar().engineState()->playing);
}
TEST(WorkbenchEngine, SessionPeerRefusesDiscoveryForAnotherProject) {
    WorkDir dir("engine-wrong-project");
    dir.write("project/Game.frostproj", "{}");
    dir.write("other/Other.frostproj", "{}");
    EngineSessionPeer other(dir.join("other"));
    advertise(dir, other);
    dir.write("automation/Automation.json", other.record().dump());
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("discoveryBusy") == false; }, 3s));
    EXPECT_FALSE(listed(rig, other.instanceId()));
    EXPECT_FALSE(rig.workbench->pairEngineInstance(other.instanceId()));
    EXPECT_FALSE(rig.workbench->sendEngineCommand("play.start", Json::object()));
    EXPECT_TRUE(other.requests("auth.challenge").empty());
    EXPECT_TRUE(other.requests("play.start").empty());
}
TEST(WorkbenchEngine, SessionPeerTestFailureAndLogLocationsNavigate) {
    WorkDir dir("engine-result-locations");
    dir.write("project/Game.frostproj", "{}");
    dir.write("project/Assets/Scripts/Failure.lua", "local a = 1\nerror('intentional failure')\n");
    EngineSessionPeer peer(dir.join("project"));
    peer.result("test.run", Json::object()
                                .set("passed", false)
                                .set("results", Json::array({Json::object()
                                                                 .set("message", "intentional failure")
                                                                 .set("path", "Assets/Scripts/Failure.lua")
                                                                 .set("line", 2)})));
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, peer));
    auto* output = console(rig);
    ASSERT_NE(output, nullptr);
    auto requested = rig.workbench->sendEngineCommand("test.run", Json::object());
    ASSERT_TRUE(requested);
    ASSERT_TRUE(waitFor([&] { return completed(rig, *requested); }, 3s));
    ASSERT_TRUE(waitFor(
        [&] {
            auto* item = rowContaining(output, QStringLiteral("intentional failure"));
            return item && item->childCount() != 0;
        },
        3s));
    auto* failure = rowContaining(output, QStringLiteral("intentional failure"));
    EXPECT_TRUE(output->activate(failure));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->currentView() != nullptr; }, 3s));
    EXPECT_EQ(rig.workbench->currentView()->status().line, 2);
    EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("project/Assets/Scripts/Failure.lua"));
    peer.log("integration log", "Assets/Scripts/Failure.lua", 1);
    ASSERT_TRUE(waitFor([&] { return rowContaining(output, QStringLiteral("integration log")) != nullptr; }, 3s));
    EXPECT_TRUE(output->activate(rowContaining(output, QStringLiteral("integration log"))));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->currentView()->status().line == 1; }, 3s));
}
TEST(WorkbenchEngine, ToolWindowExistsOnlyForFrostEngineProjectsAndHasNoLuaDebugAction) {
    WorkDir dir("engine-gating");
    dir.write("plain/main.lua", "return 1\n");
    dir.write("game/Game.frostproj", "{}");
    dir.write("legacy/Old.feproject", "{}");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("plain")));
    EXPECT_EQ(rig.workbench->engineState().boolAt("available"), false);
    EXPECT_FALSE(rig.window->toolWindows().isVisible("frostEngine"));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("game")));
    ASSERT_EQ(rig.workbench->engineState().boolAt("available"), true);
    auto* output = console(rig);
    ASSERT_NE(output, nullptr);
    EXPECT_FALSE(output->submitCommand(QStringLiteral("play.start")));
    EXPECT_FALSE(rig.workbench->registry().find("engine.debugLua"));
    auto* panel = rig.window->toolWindows().content("frostEngine");
    for (auto* candidate : panel->findChildren<QPushButton*>())
        EXPECT_FALSE(candidate->text().contains(QStringLiteral("Lua"))) << candidate->text().toStdString();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("plain")));
    EXPECT_FALSE(rig.window->toolWindows().isVisible("frostEngine"));
    EXPECT_FALSE(output->submitCommand(QStringLiteral("play.start")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("legacy")));
    EXPECT_EQ(rig.workbench->engineState().boolAt("available"), true);
}
TEST(WorkbenchEngine, ToolWindowGateAndLuaDefaultsAgreeOnWhatAFrostEngineProjectIs) {
    WorkDir dir("engine-one-rule");
    dir.write("upper/GAME.FROSTPROJ", "{}");
    dir.write("legacy/Old.FeProject", "{}");
    dir.write("stemless/.frostproj", "{}");
    dir.write("nested/sub/Game.frostproj", "{}");
    dir.write("plain/main.lua", "return 1\n");
    WorkbenchRig rig(dir);
    for (const char* folder : {"upper", "legacy", "stemless", "nested", "plain"}) {
        ASSERT_TRUE(rig.workbench->openPath(dir.join(folder))) << folder;
        const bool lua = frost::lang::isFrostEngineLuaWorkspace(frost::io::realFileSystem(), dir.join(folder));
        const bool expected = std::string_view(folder) == "upper" || std::string_view(folder) == "legacy";
        EXPECT_EQ(lua, expected) << folder;
        EXPECT_EQ(rig.workbench->engineState().boolAt("available"), lua) << folder;
        if (!lua)
            EXPECT_FALSE(rig.window->toolWindows().isVisible("frostEngine"));
    }
}
TEST(WorkbenchEngine, ShowsTheEngineErrorInsteadOfAGenericFailure) {
    WorkDir dir("engine-error-message");
    dir.write("project/Game.frostproj", "{}");
    EngineSessionPeer peer(dir.join("project"));
    peer.refuse("play.start", -32000, "Play refused: a scene is still loading");
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, peer));
    ASSERT_TRUE(rig.workbench->sendEngineCommand("play.start", Json::object()));
    EXPECT_TRUE(waitFor(
        [&] {
            return rig.workbench->engineState().stringAt("lastError").find("Play refused: a scene is still loading") !=
                   std::string_view::npos;
        },
        3s));
    EXPECT_NE(rig.workbench->engineState().stringAt("lastError").find("-32000"), std::string_view::npos);
    EXPECT_EQ(peer.requests("play.start").size(), 1U);
}
TEST(WorkbenchEngine, CancelRequestReleasesPendingTestRunAndKeepsSessionUsable) {
    WorkDir dir("engine-test-run-cancel");
    dir.write("project/Game.frostproj", "{}");
    EngineSessionPeer peer(dir.join("project"));
    peer.hold("test.run");
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, peer));
    ASSERT_NE(console(rig), nullptr);
    auto* panel = rig.window->toolWindows().content("frostEngine");
    auto* runTests = button(panel, QStringLiteral("Run Tests"));
    auto* cancel = button(panel, QStringLiteral("Cancel Request"));
    ASSERT_NE(runTests, nullptr);
    ASSERT_NE(cancel, nullptr);
    runTests->click();
    ASSERT_TRUE(waitFor([&] { return peer.requests("test.run").size() == 1; }, 3s));
    static_cast<void>(waitFor([] { return false; }, 500ms));
    EXPECT_EQ(peer.requests("test.run").size(), 1U);
    EXPECT_TRUE(rig.workbench->engineState().boolAt("connected").value_or(false));
    EXPECT_TRUE(rig.workbench->engineState().find("lastReply")->isNull());
    ASSERT_TRUE(cancel->isEnabled());
    const auto requestId = rig.workbench->engineState().unsignedAt("lastRequestId");
    ASSERT_TRUE(requestId && *requestId != 0);
    cancel->click();
    ASSERT_TRUE(
        waitFor([&] { return rig.workbench->engineState().unsignedAt("lastCompletedRequestId") == *requestId; }, 3s));
    const auto cancelled = rig.workbench->engineState();
    EXPECT_EQ(cancelled.unsignedAt("pendingRequestCount"), 0U);
    EXPECT_EQ(cancelled.stringAt("lastError"), "engine request interrupted; outcome uncertain");
    EXPECT_TRUE(rig.workbench->engineState().boolAt("connected").value_or(false));
    // Cancellation releases this request, not the authenticated session, and never replays it.
    peer.release("test.run", Json::object().set("late", "cancelled-run-result"));
    ASSERT_TRUE(waitFor([&] { return peer.replies("test.run") == 1; }, 3s));
    static_cast<void>(waitFor([] { return false; }, 100ms));
    EXPECT_TRUE(rig.workbench->engineState().find("lastReply")->isNull());
    auto focus = rig.workbench->sendEngineCommand("editor.focus", Json::object().set("afterCancel", true));
    ASSERT_TRUE(focus);
    EXPECT_TRUE(waitFor([&] { return completed(rig, *focus); }, 3s));
    EXPECT_EQ(peer.requests("test.run").size(), 1U);
}
TEST(WorkbenchEngine, SchemaRefusalTransmitsNoCommandAndLeavesSessionUsable) {
    WorkDir dir("engine-command-policy");
    dir.write("project/Game.frostproj", "{}");
    EngineSessionPeer peer(dir.join("project"));
    advertise(dir, peer);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, peer));
    const auto before = rig.workbench->engineState().unsignedAt("sentCommandCount");
    auto denied = rig.workbench->sendEngineCommand("scene.delete", Json::object());
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), frost::ErrorCode::PermissionDenied);
    EXPECT_TRUE(peer.requests("scene.delete").empty());
    EXPECT_EQ(rig.workbench->engineState().unsignedAt("sentCommandCount"), before);
    auto allowed = rig.workbench->sendEngineCommand("editor.focus", Json::object());
    ASSERT_TRUE(allowed);
    EXPECT_TRUE(waitFor([&] { return completed(rig, *allowed); }, 3s));
}

TEST(WorkbenchEngine, AcceptedBeforeTaskFailureReleasesExactOwnedReservationAndAllowsRetry) {
    WorkDir dir("engine-beforetask-failure");
    dir.write("project/Game.frostproj", "{}");
    const auto program = editorProgram(dir);
    EngineRunLauncher launcher;
    WorkbenchRig rig(dir, runOptions(dir, launcher, program));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(waitForConfiguration(rig));
    auto* controller = rig.workbench->runController();
    auto edited = controller->configurations().all();
    auto configuration =
        std::ranges::find(edited, frost::run::RunType::FrostEditor, &frost::run::RunConfiguration::type);
    ASSERT_NE(configuration, edited.end());
    const std::string id = configuration->id;
    const std::string task = dir.join("missing-before-task");
    configuration->beforeLaunchTasks.push_back({task, {}, {}});
    ASSERT_TRUE(controller->replaceConfigurations(edited));
    launcher.runs.expect(frost::io::ScriptedProcess{
        .expectExecutable = task,
        .launchError = frost::Error(frost::ErrorCode::NotFound, "before task executable is unavailable")});
    auto accepted = controller->runConfiguration(id);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(rig.workbench->engineState().unsignedAt("ownedRunId"), *accepted);
    EXPECT_EQ(rig.workbench->engineState().boolAt("launchReserved"), true);
    ASSERT_TRUE(waitFor([&] { return runReservationReleased(rig); }, 5s));
    ASSERT_EQ(launcher.runs.launches().size(), 1U);
    EXPECT_EQ(launcher.runs.launches().front().spec.executable, task);
    EXPECT_FALSE(launcher.runs.launches().front().launched);
    edited = controller->configurations().all();
    configuration = std::ranges::find(edited, id, &frost::run::RunConfiguration::id);
    ASSERT_NE(configuration, edited.end());
    configuration->beforeLaunchTasks.clear();
    ASSERT_TRUE(controller->replaceConfigurations(edited));
    launcher.runs.expect(frost::io::ScriptedProcess{
        .expectExecutable = program,
        .launchError = frost::Error(frost::ErrorCode::IoError, "retry reached engine process creation")});
    auto retry = controller->runConfiguration(id);
    ASSERT_TRUE(retry) << retry.error().message();
    EXPECT_NE(*retry, *accepted);
    ASSERT_TRUE(waitFor([&] { return runReservationReleased(rig); }, 5s));
    ASSERT_EQ(launcher.runs.launches().size(), 2U);
    EXPECT_EQ(launcher.runs.launches().back().spec.executable, program);
}
TEST(WorkbenchEngine, MalformedOwnedEndpointStopsItsRunAndAllowsFreshLaunch) {
    WorkDir dir("engine-malformed-endpoint");
    dir.write("project/Game.frostproj", "{}");
    const auto program = editorProgram(dir);
    EngineRunLauncher launcher;
    WorkbenchRig rig(dir, runOptions(dir, launcher, program));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(waitForConfiguration(rig));
    const auto malformed =
        Json::object()
            .set("ok", true)
            .set("command", "editor")
            .set("result",
                 Json::object().set("host", "192.0.2.1").set("port", 9999).set("projectPath", dir.join("project")));
    launcher.runs.expect(frost::io::ScriptedProcess{
        .expectExecutable = program, .stdoutBytes = malformed.dump() + "\n", .neverExits = true});
    auto started = rig.workbench->startEngineEditor();
    ASSERT_TRUE(started);
    ASSERT_TRUE(waitFor([&] { return runReservationReleased(rig); }, 5s));
    ASSERT_EQ(launcher.runs.launches().size(), 1U);
    EXPECT_TRUE(launcher.runs.launches().front().terminated);
    EXPECT_EQ(rig.workbench->engineState().boolAt("connected"), false);
    launcher.runs.expect(frost::io::ScriptedProcess{
        .expectExecutable = program,
        .launchError = frost::Error(frost::ErrorCode::PermissionDenied, "retry process refused")});
    auto retry = rig.workbench->startEngineEditor();
    ASSERT_TRUE(retry);
    EXPECT_NE(*retry, *started);
    EXPECT_TRUE(waitFor([&] { return runReservationReleased(rig); }, 5s));
    EXPECT_EQ(launcher.runs.launches().size(), 2U);
}
TEST(WorkbenchEngine, OwnedAuthenticationDeadlineIncludesAStalledChallengeAndReleasesTheRun) {
    WorkDir dir("engine-owned-auth-deadline");
    dir.write("project/Game.frostproj", "{}");
    EngineSessionPeer peer(dir.join("project"));
    ASSERT_NE(peer.port(), 0U);
    peer.stallChallenge = true;
    const auto program = editorProgram(dir);
    EngineRunLauncher launcher;
    WorkbenchRig rig(dir, runOptions(dir, launcher, program));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(waitForConfiguration(rig));
    const auto endpoint = Json::object().set("ok", true).set("command", "editor").set("result", peer.record());
    launcher.runs.expect(frost::io::ScriptedProcess{
        .expectExecutable = program, .stdoutBytes = endpoint.dump() + "\n", .neverExits = true});
    auto started = rig.workbench->startEngineEditor();
    ASSERT_TRUE(started);
    ASSERT_TRUE(waitFor([&] { return !peer.requests("auth.challenge").empty(); }, 5s));
    EXPECT_EQ(rig.workbench->engineState().boolAt("awaitingAuthentication"), true);
    EXPECT_EQ(rig.workbench->engineState().unsignedAt("ownedRunId"), *started);
    // The production owned-start deadline is 30 seconds and covers authentication, not merely
    // seeing a discovery record. No elapsed-time shortcut supplies a successful handshake.
    ASSERT_TRUE(waitFor([&] { return runReservationReleased(rig); }, 35s));
    EXPECT_EQ(rig.workbench->engineState().boolAt("awaitingAuthentication"), false);
    EXPECT_EQ(rig.workbench->engineState().boolAt("connected"), false);
    ASSERT_EQ(launcher.runs.launches().size(), 1U);
    EXPECT_TRUE(launcher.runs.launches().front().terminated);
    EXPECT_TRUE(waitFor([&] { return peer.closed.load(); }, 3s));
    EXPECT_EQ(peer.authenticated.load(), 0U);
}

TEST(WorkbenchEngine, RePairDropsQueuedReplyFromPreviousAuthenticatedInstance) {
    WorkDir dir("engine-repair-stale-result");
    dir.write("project/Game.frostproj", "{}");
    EngineSessionPeer first(dir.join("project"));
    EngineSessionPeer second(dir.join("project"), 'b');
    first.hold("play.state");
    advertise(dir, first);
    advertise(dir, second);
    WorkbenchRig rig(dir, peerOptions(dir));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(pairPeer(rig, first));
    auto* output = console(rig);
    ASSERT_NE(output, nullptr);
    ASSERT_TRUE(rig.workbench->sendEngineCommand("play.state", Json::object()));
    ASSERT_TRUE(waitFor([&] { return first.requests("play.state").size() == 1; }, 3s));
    first.release("play.state", Json::object().set("stale", "previous-instance-reply"));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (first.replies("play.state") == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(first.replies("play.state"), 1U);
    ASSERT_TRUE(pairPeer(rig, second));
    EXPECT_EQ(rig.workbench->engineState().stringAt("selectedInstanceId"), second.instanceId());
    EXPECT_TRUE(rig.workbench->engineState().find("lastReply")->isNull());
    EXPECT_EQ(rowContaining(output, QStringLiteral("previous-instance-reply")), nullptr);
    EXPECT_TRUE(first.closed.load());
}
TEST(WorkbenchEngine, OwnedLifecycleInvalidationReleasesItsRunAndRejectsQueuedReplies) {
    for (const std::string_view reason : {"trust", "project", "shutdown"}) {
        SCOPED_TRACE(reason);
        WorkDir dir("engine-owned-invalidation");
        dir.write("project/Game.frostproj", "{}");
        dir.write("other/Other.frostproj", "{}");
        EngineSessionPeer peer(dir.join("project"));
        ASSERT_NE(peer.port(), 0U);
        peer.hold("play.state");
        const auto program = editorProgram(dir);
        EngineRunLauncher launcher;
        WorkbenchRig rig(dir, runOptions(dir, launcher, program));
        ASSERT_TRUE(openTrusted(rig, dir.join("project")));
        ASSERT_TRUE(waitForConfiguration(rig));
        const auto endpoint = Json::object().set("ok", true).set("command", "editor").set("result", peer.record());
        launcher.runs.expect(frost::io::ScriptedProcess{
            .expectExecutable = program, .stdoutBytes = endpoint.dump() + "\n", .neverExits = true});
        auto started = rig.workbench->startEngineEditor();
        ASSERT_TRUE(started);
        ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().stringAt("state") == "Edit"; }, 5s));
        auto* output = console(rig);
        ASSERT_NE(output, nullptr);
        ASSERT_TRUE(rig.workbench->sendEngineCommand("play.state", Json::object()));
        ASSERT_TRUE(waitFor([&] { return peer.requests("play.state").size() == 1; }, 3s));
        peer.release("play.state", Json::object().set("stale", "invalidated-owned-reply"));
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (peer.replies("play.state") == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        ASSERT_EQ(peer.replies("play.state"), 1U);
        if (reason == "trust") {
            ASSERT_TRUE(rig.trust->untrust(dir.join("project")));
        } else if (reason == "project") {
            ASSERT_TRUE(rig.workbench->openPath(dir.join("other")));
        } else {
            rig.workbench->shutdown();
        }
        ASSERT_TRUE(waitFor([&] { return runReservationReleased(rig); }, 5s));
        EXPECT_TRUE(waitFor([&] { return peer.closed.load(); }, 3s));
        EXPECT_FALSE(rig.workbench->sendEngineCommand("play.start", Json::object()));
        EXPECT_TRUE(peer.requests("play.start").empty());
        EXPECT_TRUE(rig.workbench->engineState().find("lastReply")->isNull());
        EXPECT_EQ(rowContaining(output, QStringLiteral("invalidated-owned-reply")), nullptr);
        ASSERT_EQ(launcher.runs.launches().size(), 1U);
        EXPECT_TRUE(launcher.runs.launches().front().terminated);
    }
}

TEST(WorkbenchEngine, NativeDapStopSurvivesHeartbeatLossButReplacementGenerationCannotInheritBinding) {
    for (const bool replaceDebugger : {false, true}) {
        SCOPED_TRACE(replaceDebugger);
        WorkDir dir("engine-native-heartbeat-loss");
        dir.write("project/Game.frostproj", "{}");
        dir.write("project/engine-debuggee", "DAP fixture target");
        dir.write("project/main.cpp", "int main() { return 0; }\n");
        constexpr std::int64_t pid = 4321;
        EngineSessionPeer peer(dir.join("project"));
        peer.processId = pid;
        advertise(dir, peer);
        frost::debug::FakeDebugAdapter firstAdapter, replacementAdapter;
        for (auto* adapter : {&firstAdapter, &replacementAdapter})
            frost::debug::scriptProgram(*adapter, {.sourcePath = dir.join("project/main.cpp"), .runsForever = true});
        auto options = peerOptions(dir);
        unsigned adapterIndex = 0;
        options.workbench.debugAdapterFactory = [&](frost::debug::DapHandlers handlers)
            -> frost::Result<std::unique_ptr<frost::debug::IDebugAdapterClient>> {
            auto& adapter = adapterIndex++ == 0 ? firstAdapter : replacementAdapter;
            return frost::debug::makeDapClient(adapter.transport(), std::move(handlers));
        };
        WorkbenchRig rig(dir, options);
        ASSERT_TRUE(openTrusted(rig, dir.join("project")));
        ASSERT_TRUE(pairPeer(rig, peer));
        auto* controller = rig.workbench->debugController();
        ASSERT_NE(controller, nullptr);
        const frost::debug::DebugTarget target{.name = "FrostEditor native fixture",
                                               .program = dir.join("project/engine-debuggee"),
                                               .trustRoots = {dir.join("project")}};
        ASSERT_TRUE(controller->startTarget(target));
        ASSERT_TRUE(waitFor([&] { return controller->state() == frost::debug::DebugState::Running; }, 3s));
        firstAdapter.sendEvent("process", Json::object().set("systemProcessId", pid));
        ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("nativeDebuggerMatched") == true; }, 3s));
        firstAdapter.sendEvent("stopped", Json::object().set("reason", "pause").set("threadId", 1));
        ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("debuggerPaused") == true; }, 3s));
        const auto authenticatedDebugGeneration = controller->sessionGeneration();
        peer.heartbeats = false; // Model a native all-stop suspending the engine's network thread.
        ASSERT_TRUE(waitFor(
            [&] {
                const auto state = rig.workbench->engineState();
                const auto& toolbar = rig.window->mainToolbar().engineState();
                return state.boolAt("connected") == false && state.boolAt("nativeDebuggerMatched") == true &&
                       state.boolAt("debuggerPaused") == true && toolbar && toolbar->canStop;
            },
            5s));
        ASSERT_EQ(rig.workbench->engineState().boolAt("nativeDebuggerMatched"), true);
        ASSERT_EQ(rig.workbench->engineState().boolAt("debuggerPaused"), true);
        ASSERT_TRUE(rig.window->mainToolbar().engineState());
        EXPECT_TRUE(rig.window->mainToolbar().engineState()->canStop);
        EXPECT_FALSE(rig.workbench->sendEngineCommand("play.stop", Json::object()));
        auto stop = rig.workbench->registry().find("engine.stop");
        ASSERT_TRUE(stop);
        frost::input::ActionContext context;
        context.trusted = true;
        if (!replaceDebugger) {
            EXPECT_EQ(rig.workbench->registry().tryInvoke(*stop, context), frost::input::InvokeResult::Ran);
            ASSERT_TRUE(waitFor([&] { return firstAdapter.argumentsOf("terminate").size() == 1; }, 3s));
            EXPECT_TRUE(peer.requests("play.stop").empty());
            EXPECT_EQ(controller->sessionGeneration(), authenticatedDebugGeneration);
            EXPECT_EQ(controller->state(), frost::debug::DebugState::Stopped);
            EXPECT_TRUE(controller->debugging()); // A DAP acknowledgement is not an invented exit.
            firstAdapter.sendEvent("terminated", Json::object());
            ASSERT_TRUE(waitFor([&] { return !controller->debugging(); }, 3s));
        } else {
            const auto disconnects = [&] {
                const auto snapshot = rig.workbench->engineState();
                const auto* events = snapshot.find("events");
                return events ? std::ranges::count_if(
                                    events->items(),
                                    [](const Json& event) { return event.stringAt("topic") == "disconnected"; })
                              : 0;
            };
            ASSERT_GT(disconnects(), 0);
            controller->stop();
            ASSERT_TRUE(waitFor([&] { return !controller->debugging(); }, 3s));
            ASSERT_TRUE(controller->startTarget(target));
            ASSERT_TRUE(waitFor([&] { return controller->state() == frost::debug::DebugState::Running; }, 3s));
            EXPECT_NE(controller->sessionGeneration(), authenticatedDebugGeneration);
            replacementAdapter.sendEvent("process", Json::object().set("systemProcessId", pid));
            replacementAdapter.sendEvent("stopped", Json::object().set("reason", "pause").set("threadId", 1));
            ASSERT_TRUE(waitFor(
                [&] {
                    return controller->session() && controller->session()->processId() == pid &&
                           controller->state() == frost::debug::DebugState::Stopped;
                },
                3s));
            EXPECT_EQ(rig.workbench->engineState().boolAt("nativeDebuggerMatched"), false);
            // The peer no longer accepts connections. A subsequent real reconnect/handshake
            // failure must not relatch this new DAP session merely because its PID matches.
            const auto disconnectsAfterReplacement = disconnects();
            ASSERT_TRUE(waitFor([&] { return disconnects() > disconnectsAfterReplacement; }, 10s));
            EXPECT_EQ(rig.workbench->engineState().boolAt("nativeDebuggerMatched"), false);
            EXPECT_NE(rig.workbench->engineState().boolAt("debuggerPaused"), true);
            EXPECT_FALSE(rig.window->mainToolbar().engineState()->canStop);
            EXPECT_EQ(rig.workbench->registry().tryInvoke(*stop, context), frost::input::InvokeResult::Disabled);
            EXPECT_TRUE(replacementAdapter.argumentsOf("terminate").empty());
            EXPECT_TRUE(peer.requests("play.stop").empty());
        }
    }
}
TEST(WorkbenchEngine, WorkerLaunchLatchesExactProjectFileAndRefusesSameRootAliasRetarget) {
    WorkDir dir("engine-project-file-latch");
    dir.write("project/A.frostproj", "{}");
    dir.write("project/B.frostproj", "{}");
#ifdef _WIN32
    const std::string aliasName = "project/SELECT~1.FRO";
#else
    const std::string aliasName = "project/Selected.frostproj";
#endif
    const auto alias = frost::io::toStdPath(dir.join(aliasName));
    const auto first = frost::io::toStdPath(dir.join("project/A.frostproj"));
    const auto second = frost::io::toStdPath(dir.join("project/B.frostproj"));
    ASSERT_TRUE(alias && first && second);
#ifndef _WIN32
    std::error_code error;
#endif
#ifdef _WIN32
    ASSERT_EQ(setProjectShortName(*first, L"SELECT~1.FRO"), ERROR_SUCCESS);
#else
    std::filesystem::create_symlink(*first, *alias, error);
    ASSERT_FALSE(error) << "required project-file alias fixture unavailable: " << error.message();
#endif
    const auto physicalRoot = std::filesystem::canonical(*first).parent_path();
    ASSERT_EQ(std::filesystem::canonical(*alias), std::filesystem::canonical(*first));
    ASSERT_EQ(std::filesystem::canonical(*second).parent_path(), physicalRoot);
    const auto program = editorProgram(dir);
    const std::string task = dir.join("before-task");
    EngineRunLauncher launcher;
    WorkbenchRig rig(dir, runOptions(dir, launcher, program));
    ASSERT_TRUE(openTrusted(rig, dir.join("project")));
    ASSERT_TRUE(waitForConfiguration(rig));
    ASSERT_NE(console(rig), nullptr);
    auto* controller = rig.workbench->runController();
    auto edited = controller->configurations().all();
    auto configuration =
        std::ranges::find(edited, frost::run::RunType::FrostEditor, &frost::run::RunConfiguration::type);
    ASSERT_NE(configuration, edited.end());
    const std::string id = configuration->id;
    configuration->engineProjectPath = dir.join(aliasName);
    configuration->arguments = {"--log-level", "warn"};
    configuration->beforeLaunchTasks.push_back({task, {"unchanged-task-argument"}, {}});
    ASSERT_TRUE(controller->replaceConfigurations(edited));
    std::atomic<bool> retargeted{false};
    std::atomic<int> retargetError{0};
    launcher.afterRunLaunch = [&](const frost::io::ProcessSpec& spec) {
        if (spec.executable != task)
            return;
#ifdef _WIN32
        DWORD failure = setProjectShortName(*first, L"");
        if (failure == ERROR_SUCCESS)
            failure = setProjectShortName(*second, L"SELECT~1.FRO");
        retargetError = static_cast<int>(failure);
#else
        std::error_code failure;
        std::filesystem::remove(*alias, failure);
        if (!failure)
            std::filesystem::create_symlink(*second, *alias, failure);
        retargetError = failure.value();
#endif
        retargeted = true;
    };
    launcher.runs.expect(frost::io::ScriptedProcess{.expectExecutable = task});
    auto accepted = controller->runConfiguration(id);
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(waitFor([&] { return runReservationReleased(rig); }, 5s));
    ASSERT_TRUE(retargeted.load());
    ASSERT_EQ(retargetError.load(), 0);
    ASSERT_EQ(std::filesystem::canonical(*alias), std::filesystem::canonical(*second));
    ASSERT_NE(std::filesystem::canonical(*alias), std::filesystem::canonical(*first));
    ASSERT_EQ(std::filesystem::canonical(*alias).parent_path(), physicalRoot);
    ASSERT_EQ(std::filesystem::canonical(*first).parent_path(), physicalRoot);
    ASSERT_TRUE(std::filesystem::exists(*first));
    ASSERT_TRUE(std::filesystem::exists(*second));
    ASSERT_EQ(launcher.runs.launches().size(), 1U) << "retargeted alias reached engine process creation";
    EXPECT_TRUE(hasStatus(rig.window->toolWindows().content("frostEngine"),
                          QStringLiteral("target changed after its launch was admitted")));
    EXPECT_EQ(launcher.runs.launches().front().spec.executable, task);
    EXPECT_EQ(launcher.runs.launches().front().spec.arguments, std::vector<std::string>{"unchanged-task-argument"});
    // A fresh explicit run gets its own latch and passes the original physical file to the child.
    launcher.afterRunLaunch = {};
#ifdef _WIN32
    ASSERT_EQ(setProjectShortName(*second, L""), ERROR_SUCCESS);
    ASSERT_EQ(setProjectShortName(*first, L"SELECT~1.FRO"), ERROR_SUCCESS);
#else
    std::filesystem::remove(*alias, error);
    ASSERT_FALSE(error);
    std::filesystem::create_symlink(*first, *alias, error);
    ASSERT_FALSE(error);
#endif
    ASSERT_EQ(std::filesystem::canonical(*alias), std::filesystem::canonical(*first));
    edited = controller->configurations().all();
    configuration = std::ranges::find(edited, id, &frost::run::RunConfiguration::id);
    ASSERT_NE(configuration, edited.end());
    configuration->beforeLaunchTasks.clear();
    ASSERT_TRUE(controller->replaceConfigurations(edited));
    launcher.runs.expect(frost::io::ScriptedProcess{
        .expectExecutable = program,
        .launchError = frost::Error(frost::ErrorCode::IoError, "positive latch reached engine launch")});
    auto retry = controller->runConfiguration(id);
    ASSERT_TRUE(retry);
    ASSERT_TRUE(waitFor([&] { return runReservationReleased(rig); }, 5s));
    ASSERT_EQ(launcher.runs.launches().size(), 2U);
    const auto arguments = launcher.runs.launches().back().spec.arguments;
    ASSERT_GE(arguments.size(), 4U);
    EXPECT_EQ(arguments.front(), "--project");
    EXPECT_EQ(arguments[1], frost::io::realFileSystem().physicalPath(dir.join("project/A.frostproj")).value());
    const auto logLevel = std::ranges::find(arguments, "--log-level");
    ASSERT_NE(logLevel, arguments.end());
    ASSERT_NE(std::next(logLevel), arguments.end());
    EXPECT_EQ(*std::next(logLevel), "warn");
}

} // namespace
