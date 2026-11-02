// SPDX-License-Identifier: Apache-2.0
// Workbench integration tests for FrostEngine automation.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "../../src/app/src/engine_request_helper.hpp"
#include "workbench_rig.hpp"
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>

#include <filesystem>
#include <thread>

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
