// SPDX-License-Identifier: Apache-2.0
// R6/R7 Workbench acceptance against isolated real pinned engines, scene providers and dump adapters.
#include "frost/engine/crash_artifact.hpp"
#include "frost/engine/engine_test_provider.hpp"
#include "frost/engine/project_content.hpp"
#include "frost/engine/schema.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/utf.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/engine_console.hpp"
#include "frost/ui/toolwindows/engine_view.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "engine_crash_observer.hpp"
#include "support/real_engine_session.hpp"
#include "workbench_rig.hpp"
#include <QAction>
#include <QMenu>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeView>
#include <QTreeWidget>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#ifndef FROST_DBGENG_ADAPTER_PATH
#error "The real crash composition case requires the built frost-dbgeng-dap path"
#endif
#endif

namespace {
using namespace std::chrono_literals;
using namespace frost::app::test;
using frost::rpc::Json;

QTreeWidgetItem* reportedConsoleFrame(frost::ui::EngineConsole& console, const QString& message,
                                      const std::string& path, int line) {
    for (int row = 0; row < console.view()->topLevelItemCount(); ++row) {
        auto* item = console.view()->topLevelItem(row);
        if (!item->text(3).contains(message))
            continue;
        for (int child = 0; child < item->childCount(); ++child) {
            auto* frame = item->child(child);
            if (frame->data(0, Qt::UserRole).toString() == QString::fromStdString(path) &&
                frame->data(0, Qt::UserRole + 1).toInt() == line)
                return frame;
        }
    }
    return nullptr;
}

// Keep an IDE-owned editor on the same invisible desktop as external acceptance editors.
class HiddenEngineLauncher final : public frost::io::IProcessLauncher {
public:
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& original) override {
        auto spec = hidden(original);
        if (!spec)
            return std::unexpected(spec.error());
        return frost::io::realProcessLauncher().launch(*spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& original, const frost::io::AsyncContext& context) override {
        auto spec = hidden(original);
        if (!spec)
            return std::unexpected(spec.error());
        return frost::io::realProcessLauncher().launchAsync(*spec, context);
    }

private:
    frost::Result<frost::io::ProcessSpec> hidden(const frost::io::ProcessSpec& original) const {
        auto spec = original;
#ifdef _WIN32
        if (!desktop.valid())
            return frost::fail(frost::ErrorCode::IoError, "private desktop unavailable for owned editor");
        spec.desktop = desktop.name();
#endif
        return spec;
    }
#ifdef _WIN32
    frost::test::PrivateDesktop desktop;
#endif
};

class RealEngineWorkbench : public testing::Test {
protected:
    void SetUp() override {
        auto ready = build.prepare();
        ASSERT_TRUE(ready) << ready.error().describe();
        project = dir.join("project");
        auto copied = build.copyProject(project);
        ASSERT_TRUE(copied) << copied.error().describe();
        config = dir.join("engine-config");
    }
    RigOptions options() const {
        RigOptions options;
        options.workbench.frostEditorPath = build.program;
        options.workbench.engineAutomationDirectory = config + "/Automation";
        options.workbench.engineConfigDirectory = config;
        options.workbench.engineEditorArguments = {"--offscreen", "--size", "320x240"};
        return options;
    }
    void open(WorkbenchRig& rig) {
        ASSERT_TRUE(rig.trust->trust(project));
        ASSERT_TRUE(rig.workbench->openExternalProject(project));
        rig.window->toolWindows().show("frostEngine");
    }
    void pair(WorkbenchRig& rig, const frost::engine::EngineEndpoint& endpoint) {
        ASSERT_TRUE(waitFor([&] { return !rig.workbench->engineState().boolAt("discoveryBusy").value_or(true); }, 30s));
        ASSERT_TRUE(rig.workbench->refreshEngineInstances());
        ASSERT_TRUE(waitFor(
            [&] {
                const auto snapshot = rig.workbench->engineState();
                const auto* instances = snapshot.find("instances");
                return !snapshot.boolAt("discoveryBusy").value_or(true) && instances &&
                       std::ranges::any_of(instances->items(), [&](const Json& item) {
                           return item.stringAt("instanceId") == endpoint.instanceId;
                       });
            },
            30s));
        ASSERT_TRUE(rig.workbench->pairEngineInstance(endpoint.instanceId));
        ASSERT_TRUE(waitFor(
            [&] {
                const auto state = rig.workbench->engineState();
                return state.boolAt("connected").value_or(false) && !state.stringAt("state").empty();
            },
            30s));
        EXPECT_EQ(rig.workbench->engineState().stringAt("selectedInstanceId"), endpoint.instanceId);
    }
    bool hasConsoleText(WorkbenchRig& rig, std::string_view marker) {
        auto* console = rig.window->findChild<frost::ui::EngineConsole*>(QStringLiteral("engineConsole"));
        if (!console)
            return false;
        for (int row = 0; row < console->view()->topLevelItemCount(); ++row)
            for (int column = 0; column < console->view()->columnCount(); ++column)
                if (console->view()->topLevelItem(row)->text(column).contains(
                        QString::fromUtf8(marker.data(), static_cast<qsizetype>(marker.size()))))
                    return true;
        return false;
    }
    frost::test::RealEngineBuild build;
    WorkDir dir{"engine-session-acceptance"};
    std::string project, config;
};

class RealPlayerWorkbench : public RealEngineWorkbench {
protected:
    void SetUp() override {
        auto ready = build.prepare({"FrostEditor", "FrostRuntime"});
        ASSERT_TRUE(ready) << ready.error().describe();
        project = dir.join("project");
        auto copied = build.copyProject(project);
        ASSERT_TRUE(copied) << copied.error().describe();
        config = dir.join("engine-config");
    }
};

TEST_F(RealEngineWorkbench, RealConsoleAdmissionSendsAllowedCommandAndDenialSendsZeroFrames) {
    auto exported = build.cli("schema.export", project);
    ASSERT_TRUE(exported) << exported.error().describe();
    auto schema = frost::engine::parseSchemaDocument(*exported);
    ASSERT_TRUE(schema) << schema.error().describe();
    const auto excluded = std::ranges::find_if(
        schema->commands, [](const auto& command) { return !command.idePolicy || !command.idePolicy->allowed; });
    ASSERT_NE(excluded, schema->commands.end()) << "real schema has no command outside the IDE policy";
    frost::test::RealEditor editor;
    auto endpoint = editor.start(build, project, config);
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    WorkbenchRig rig(dir, options());
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_NO_FATAL_FAILURE(pair(rig, *endpoint));
    const auto before = rig.workbench->engineState().unsignedAt("sentCommandCount");
    ASSERT_TRUE(before);
    auto denied = rig.workbench->sendEngineCommand("r6.not-in-the-authenticated-schema", Json::object());
    EXPECT_FALSE(denied);
    EXPECT_EQ(rig.workbench->engineState().unsignedAt("sentCommandCount"), before);
    auto knownDenied = rig.workbench->sendEngineCommand(excluded->name, Json::object());
    EXPECT_FALSE(knownDenied);
    EXPECT_EQ(rig.workbench->engineState().unsignedAt("sentCommandCount"), before);
    // The console's own submission route must apply exactly the same production policy.
    auto* console = rig.window->findChild<frost::ui::EngineConsole*>(QStringLiteral("engineConsole"));
    ASSERT_NE(console, nullptr);
    ASSERT_TRUE(console->submitCommand(QStringLiteral("r6.not-in-the-authenticated-schema")));
    EXPECT_EQ(rig.workbench->engineState().unsignedAt("sentCommandCount"), before);
    ASSERT_TRUE(console->submitCommand(QStringLiteral("editor.focus")));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->engineState().unsignedAt("sentCommandCount").value_or(0) > *before; }, 20s));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto snapshot = rig.workbench->engineState();
            const auto* reply = snapshot.find("lastReply");
            return reply && !reply->isNull();
        },
        20s));
    EXPECT_FALSE(hasConsoleText(rig, endpoint->secret));
}

TEST_F(RealEngineWorkbench, ToolbarFollowsRealPushEventsAcrossTwentyPauseResumeCycles) {
    frost::test::RealEditor editor;
    auto endpoint = editor.start(build, project, config);
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    WorkbenchRig rig(dir, options());
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_NO_FATAL_FAILURE(pair(rig, *endpoint));
    auto& toolbar = rig.window->mainToolbar();
    auto* play = toolbar.button(frost::ui::MainToolbar::kEnginePlay);
    auto* pause = toolbar.button(frost::ui::MainToolbar::kEnginePause);
    ASSERT_NE(play, nullptr);
    ASSERT_NE(pause, nullptr);
    auto sequence = rig.workbench->engineState().unsignedAt("eventSequence").value_or(0);
    play->click();
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            return state.unsignedAt("eventSequence").value_or(0) > sequence && state.boolAt("playing").value_or(false);
        },
        20s));
    for (int cycle = 0; cycle < 20; ++cycle) {
        SCOPED_TRACE(cycle);
        for (bool paused : {true, false}) {
            sequence = rig.workbench->engineState().unsignedAt("eventSequence").value_or(0);
            auto* control = paused ? pause : play;
            ASSERT_TRUE(control->isEnabled());
            control->click();
            ASSERT_TRUE(waitFor(
                [&] {
                    const auto state = rig.workbench->engineState();
                    return state.unsignedAt("eventSequence").value_or(0) > sequence && state.boolAt("paused") == paused;
                },
                20s));
            ASSERT_TRUE(toolbar.engineState());
            EXPECT_EQ(toolbar.engineState()->paused, paused);
            EXPECT_EQ(toolbar.engineState()->playing, rig.workbench->engineState().boolAt("playing").value_or(false));
            const auto state = rig.workbench->engineState();
            const auto* events = state.find("events");
            ASSERT_NE(events, nullptr);
            EXPECT_TRUE(std::ranges::any_of(events->items(), [&](const Json& event) {
                const auto* data = event.find("data");
                return event.stringAt("topic") == "play.state" && event.unsignedAt("sequence").value_or(0) > sequence &&
                       data && data->boolAt("paused") == paused;
            }));
        }
    }
    sequence = rig.workbench->engineState().unsignedAt("eventSequence").value_or(0);
    ASSERT_TRUE(pause->isEnabled());
    pause->click();
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            return state.unsignedAt("eventSequence").value_or(0) > sequence && state.boolAt("paused") == true;
        },
        20s));
    auto* step = toolbar.button(frost::ui::MainToolbar::kEngineStep);
    ASSERT_NE(step, nullptr);
    ASSERT_NE(step->menu(), nullptr);
    const auto stepChoices = step->menu()->actions();
    const auto selected =
        std::ranges::find_if(stepChoices, [](const QAction* action) { return action->data().toInt() > 1; });
    ASSERT_NE(selected, stepChoices.end());
    const auto requestedFrames = static_cast<std::uint64_t>((*selected)->data().toInt());
    for (bool chooseCountFromMenu : {true, false}) {
        const auto before = rig.workbench->engineState();
        const auto frame = before.unsignedAt("frame");
        ASSERT_TRUE(frame);
        sequence = before.unsignedAt("eventSequence").value_or(0);
        ASSERT_TRUE(step->isEnabled());
        if (chooseCountFromMenu)
            (*selected)->trigger();
        else
            step->click(); // The actual toolbar button must repeat the selected Step(n) count.
        EXPECT_EQ(static_cast<std::uint64_t>(toolbar.engineStepFrames()), requestedFrames);
        ASSERT_TRUE(waitFor(
            [&] {
                const auto state = rig.workbench->engineState();
                return state.unsignedAt("eventSequence").value_or(0) > sequence &&
                       state.unsignedAt("frame") == *frame + requestedFrames && state.boolAt("paused") == true;
            },
            20s));
        ASSERT_TRUE(toolbar.engineState());
        EXPECT_TRUE(toolbar.engineState()->paused);
        const auto after = rig.workbench->engineState();
        const auto* events = after.find("events");
        ASSERT_NE(events, nullptr);
        EXPECT_TRUE(std::ranges::any_of(events->items(), [&](const Json& event) {
            const auto* data = event.find("data");
            return event.stringAt("topic") == "play.state" && event.unsignedAt("sequence").value_or(0) > sequence &&
                   data && data->unsignedAt("frames") == *frame + requestedFrames && data->boolAt("paused") == true;
        }));
    }
}

TEST_F(RealEngineWorkbench, RealScriptLogStreamsAndProblemsNavigatesToTheEngineOracleFileAndLine) {
    constexpr std::string_view marker = "R6 real-engine acceptance log";
    constexpr std::string_view errorMarker = "R6 real-engine acceptance failure";
    ASSERT_TRUE(frost::test::installAcceptanceScript(
        project, "local Probe = {}\nfunction Probe:OnCreate()\n  print('R6 real-engine acceptance log')\n"
                 "  error('R6 real-engine acceptance failure')\nend\nreturn Probe\n"));
    frost::engine::EngineCliRequest request;
    request.command = "test.run";
    request.project = project;
    request.workingDirectory = project;
    request.params = Json::object()
                         .set("scenes", Json::array().push("Tests/FeatureTest.fscene"))
                         .set("frames", 1)
                         .set("headless", true);
    auto oracle = frost::engine::runEngineCliCommand(frost::io::realProcessLauncher(), build.program, request);
    ASSERT_TRUE(oracle) << oracle.error().describe();
    ASSERT_TRUE(oracle->result);
    const auto* scenes = oracle->result->find("scenes");
    ASSERT_NE(scenes, nullptr);
    std::optional<Json> diagnostic;
    for (const auto& scene : scenes->items())
        if (const auto* errors = scene.find("scriptErrors"))
            for (const auto& error : errors->items())
                if (error.stringAt("message").contains(errorMarker))
                    diagnostic = error;
    ASSERT_TRUE(diagnostic) << "real test.run did not report the seeded script error";
    const auto line = diagnostic->unsignedAt("line");
    ASSERT_TRUE(line);
    ASSERT_LE(*line, static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
    const int expectedLine = static_cast<int>(*line);
    const std::string expectedPath =
        frost::io::normalizePath(project + "/" + std::string(diagnostic->stringAt("path"))).value_or("");
    ASSERT_FALSE(expectedPath.empty());
    frost::test::RealEditor editor;
    auto endpoint = editor.start(build, project, config);
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    WorkbenchRig rig(dir, options());
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_NO_FATAL_FAILURE(pair(rig, *endpoint));
    auto started = rig.workbench->sendEngineCommand("play.start", Json::object());
    ASSERT_TRUE(started) << started.error().describe();
    ASSERT_TRUE(waitFor([&] { return hasConsoleText(rig, marker); }, 30s));
    const auto live = rig.workbench->engineState();
    const auto* events = live.find("events");
    ASSERT_NE(events, nullptr);
    std::optional<std::string> timestamp;
    for (const auto& event : events->items()) {
        const auto* data = event.find("data");
        if (event.stringAt("topic") != "log.entry" || !data || !data->stringAt("message").contains(marker))
            continue;
        const auto* value = data->find("timestamp");
        if (value && (value->isString() || value->kind() == Json::Kind::Integer))
            timestamp = value->isString() ? *value->asString() : value->dump();
    }
    ASSERT_TRUE(timestamp) << "real log push omitted its timestamp";
    auto* console = rig.window->findChild<frost::ui::EngineConsole*>(QStringLiteral("engineConsole"));
    ASSERT_NE(console, nullptr);
    bool exactTimestamp = false;
    for (int row = 0; row < console->view()->topLevelItemCount(); ++row) {
        const auto* item = console->view()->topLevelItem(row);
        if (item->text(3).contains(QString::fromUtf8(marker.data(), static_cast<qsizetype>(marker.size()))))
            exactTimestamp = item->text(0) == QString::fromStdString(*timestamp);
    }
    EXPECT_TRUE(exactTimestamp) << "Engine Console timestamp differs from the real log push";
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->problems() &&
                   std::ranges::any_of(rig.workbench->problems()->model()->problems(),
                                       [&](const frost::ui::Problem& problem) {
                                           return problem.source == frost::ui::DiagnosticSource::Engine &&
                                                  problem.diagnostic.diagnostic.message.contains(errorMarker) &&
                                                  problem.diagnostic.path == expectedPath &&
                                                  problem.diagnostic.diagnostic.line == expectedLine;
                                       });
        },
        30s));
    auto* problems = rig.workbench->problems();
    const auto& rows = problems->model()->problems();
    const auto found = std::ranges::find_if(rows, [&](const auto& problem) {
        return problem.source == frost::ui::DiagnosticSource::Engine && problem.diagnostic.path == expectedPath &&
               problem.diagnostic.diagnostic.line == expectedLine;
    });
    ASSERT_NE(found, rows.end());
    const auto source = problems->model()->index(static_cast<int>(std::distance(rows.begin(), found)), 0);
    ASSERT_TRUE(problems->activate(problems->filterModel()->mapFromSource(source)));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->currentView() != nullptr; }));
    EXPECT_EQ(rig.workbench->currentView()->path(), expectedPath);
    EXPECT_EQ(rig.workbench->currentView()->status().line, expectedLine);
}

TEST_F(RealEngineWorkbench, EngineViewMatchesRealCliAssetsAndPickerSelectsExactRealInstance) {
    auto oracle = build.cli("assets.list", project);
    auto scenes = build.cli("assets.list", project, Json::object().set("type", "Scene"));
    ASSERT_TRUE(oracle) << oracle.error().describe();
    ASSERT_TRUE(scenes) << scenes.error().describe();
    ASSERT_NE(oracle->find("assets"), nullptr);
    ASSERT_NE(scenes->find("assets"), nullptr);
    frost::test::RealEditor first, second;
    auto a = first.start(build, project, config);
    auto b = second.start(build, project, config);
    ASSERT_TRUE(a) << a.error().describe();
    ASSERT_TRUE(b) << b.error().describe();
    WorkbenchRig rig(dir, options());
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_NO_FATAL_FAILURE(pair(rig, *b));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            const auto* content = state.find("engineProject");
            return content && content->boolAt("available").value_or(false);
        },
        30s));
    const auto state = rig.workbench->engineState();
    const auto* content = state.find("engineProject");
    ASSERT_NE(content, nullptr);
    EXPECT_EQ(content->unsignedAt("assetCount"), oracle->find("assets")->size());
    EXPECT_EQ(content->unsignedAt("sceneCount"), scenes->find("assets")->size());
    ASSERT_NE(content->find("assets"), nullptr);
    const auto exactAssets = [](const Json& list) {
        std::vector<std::tuple<std::string, std::string, std::uint64_t>> out;
        for (const auto& item : list.items()) {
            const auto handle = frost::engine::engineHandleAt(item, "handle");
            EXPECT_TRUE(handle) << "real asset listing contained an inexact handle";
            if (handle)
                out.emplace_back(item.stringAt("path"), item.stringAt("type"), handle->value);
        }
        std::ranges::sort(out);
        return out;
    };
    EXPECT_EQ(exactAssets(*content->find("assets")), exactAssets(*oracle->find("assets")));
    EXPECT_EQ(state.stringAt("selectedInstanceId"), b->instanceId);
    // Real connected usage query, then project-settings text navigation (never workspace switching).
    auto info = build.cli("project.info", project);
    ASSERT_TRUE(info);
    const auto* start = info->find("startScene");
    ASSERT_NE(start, nullptr);
    const std::string scenePath(start->stringAt("path"));
    ASSERT_FALSE(scenePath.empty());
    auto usages = build.cli("assets.find-references", project, Json::object().set("asset", scenePath));
    ASSERT_TRUE(usages);
    auto& windows = rig.window->toolWindows();
    windows.show("engineProject");
    auto* view = qobject_cast<frost::ui::EngineView*>(windows.content("engineProject"));
    ASSERT_NE(view, nullptr);
    auto* assets = view->tree()->topLevelItem(1);
    QTreeWidgetItem* target{};
    for (int row = 0; row < assets->childCount(); ++row)
        if (assets->child(row)->text(0) == QString::fromStdString(scenePath))
            target = assets->child(row);
    ASSERT_NE(target, nullptr);
    view->tree()->setCurrentItem(target);
    auto* find = view->findChild<QPushButton*>(QStringLiteral("engineFindAssetUsages"));
    ASSERT_NE(find, nullptr);
    find->click();
    auto* rows = view->findChild<QTreeWidget*>(QStringLiteral("engineAssetUsages"));
    ASSERT_NE(rows, nullptr);
    ASSERT_TRUE(waitFor([&] { return rows->topLevelItemCount() > 0; }, 30s));
    EXPECT_EQ(static_cast<std::size_t>(rows->topLevelItemCount()), usages->find("references")->size());
    QTreeWidgetItem* settings{};
    for (int row = 0; row < rows->topLevelItemCount(); ++row)
        if (rows->topLevelItem(row)->text(0).endsWith(QStringLiteral(".frostproj")))
            settings = rows->topLevelItem(row);
    ASSERT_NE(settings, nullptr);
    const auto rootBefore = rig.workbench->projectRoot();
    rows->itemActivated(settings, 0);
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->currentView() &&
                   rig.workbench->currentView()->path() == info->stringAt("projectFile");
        },
        30s));
    EXPECT_EQ(rig.workbench->projectRoot(), rootBefore);
    rig.workbench->disconnectEngineSession();
    // Disconnecting an externally paired editor does not grant ownership of its process.
    frost::engine::EngineSessionOptions check;
    check.endpoint = *b;
    check.trusted = [](std::string_view, std::uint64_t) { return true; };
    auto alive = frost::engine::EngineSession::create(std::move(check));
    ASSERT_TRUE(alive) << alive.error().describe();
    ASSERT_TRUE(waitFor([&] { return (*alive)->connected(); }, 20s));
    (*alive)->shutdown();
}

TEST_F(RealEngineWorkbench, RealEngineRefusesDiscoveryForAnotherProject) {
    const std::string otherProject = dir.join("other-project");
    auto copied = build.copyProject(otherProject);
    ASSERT_TRUE(copied) << copied.error().describe();
    frost::test::RealEditor other;
    auto endpoint = other.start(build, otherProject, config);
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    const auto currentRoot = frost::io::realFileSystem().canonicalPath(project);
    ASSERT_TRUE(currentRoot);
    EXPECT_NE(frost::io::normalizePath(endpoint->projectPath), frost::io::normalizePath(*currentRoot));
    // Prove that this is an actual advertised editor in the configured discovery directory;
    // absence from the app's picker must be project filtering, not missing fixture setup.
    const auto record =
        frost::io::realFileSystem().readFile(config + "/Automation/Instances/" + endpoint->instanceId + ".json");
    ASSERT_TRUE(record) << "the real other-project editor did not publish its discovery record";
    const auto advertised = frost::rpc::parseJson(*record);
    ASSERT_TRUE(advertised) << "the real editor published an invalid discovery record";
    EXPECT_EQ(advertised->stringAt("instanceId"), endpoint->instanceId);
    EXPECT_EQ(advertised->stringAt("generation"), endpoint->generation);
    EXPECT_EQ(advertised->stringAt("projectPath"), endpoint->projectPath);

    struct Observation {
        std::mutex mutex;
        std::optional<frost::engine::EngineIdentity> identity;
        std::optional<frost::engine::EngineState> state;
    };
    const auto observe = [&](Observation& observation) {
        frost::engine::EngineSessionOptions options;
        options.endpoint = *endpoint;
        options.trustRevision = 1;
        options.trusted = [physicalProject = endpoint->projectPath](std::string_view requested,
                                                                    std::uint64_t revision) {
            return requested == physicalProject && revision == 1;
        };
        frost::engine::EngineSessionCallbacks callbacks;
        callbacks.connected = [&observation](const frost::engine::EngineIdentity& identity) {
            std::lock_guard lock(observation.mutex);
            observation.identity = identity;
        };
        callbacks.state = [&observation](const frost::engine::EngineState& state) {
            std::lock_guard lock(observation.mutex);
            observation.state = state;
        };
        return frost::engine::EngineSession::create(std::move(options), std::move(callbacks));
    };
    Observation before;
    auto initial = observe(before);
    ASSERT_TRUE(initial) << initial.error().describe();
    ASSERT_TRUE(waitFor(
        [&] {
            std::lock_guard lock(before.mutex);
            return before.identity.has_value() && before.state.has_value();
        },
        30s));
    frost::engine::EngineState oracle;
    {
        std::lock_guard lock(before.mutex);
        oracle = *before.state;
    }
    (*initial)->shutdown();

    WorkbenchRig rig(dir, options());
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("discoveryBusy") == false; }, 30s));
    ASSERT_TRUE(rig.workbench->refreshEngineInstances());
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("discoveryBusy") == false; }, 30s));
    const auto discovered = rig.workbench->engineState();
    const auto* instances = discovered.find("instances");
    ASSERT_NE(instances, nullptr);
    EXPECT_FALSE(std::ranges::any_of(instances->items(), [&](const Json& instance) {
        return instance.stringAt("instanceId") == endpoint->instanceId;
    }));
    EXPECT_EQ(discovered.boolAt("connected"), false);
    EXPECT_TRUE(discovered.stringAt("selectedInstanceId").empty());
    const auto sent = discovered.unsignedAt("sentCommandCount");
    ASSERT_TRUE(sent);
    auto refusedPair = rig.workbench->pairEngineInstance(endpoint->instanceId);
    ASSERT_FALSE(refusedPair);
    EXPECT_TRUE(refusedPair.error().code() == frost::ErrorCode::NotFound ||
                refusedPair.error().code() == frost::ErrorCode::PermissionDenied);
    EXPECT_FALSE(refusedPair.error().message().empty());
    EXPECT_FALSE(refusedPair.error().message().contains(endpoint->secret));
    auto* play = rig.window->mainToolbar().button(frost::ui::MainToolbar::kEnginePlay);
    ASSERT_NE(play, nullptr);
    EXPECT_FALSE(play->isEnabled());
    play->click();
    auto refusedPlay = rig.workbench->sendEngineCommand("play.start", Json::object());
    ASSERT_FALSE(refusedPlay);
    EXPECT_FALSE(refusedPlay.error().message().empty());
    const auto rejected = rig.workbench->engineState();
    EXPECT_EQ(rejected.unsignedAt("sentCommandCount"), sent);
    EXPECT_EQ(rejected.boolAt("connected"), false);
    EXPECT_TRUE(rejected.stringAt("selectedInstanceId").empty());
    EXPECT_FALSE(rejected.dump().contains(endpoint->secret));
    rig.workbench->shutdown();

    // A second independent HMAC bootstrap reads the real engine's fresh pushed baseline after
    // the refused app actions and app shutdown. It must retain the original identity and state.
    Observation after;
    auto unchanged = observe(after);
    ASSERT_TRUE(unchanged) << unchanged.error().describe();
    ASSERT_TRUE(waitFor(
        [&] {
            std::lock_guard lock(after.mutex);
            return after.identity.has_value() && after.state.has_value();
        },
        30s));
    {
        std::lock_guard lock(after.mutex);
        EXPECT_EQ(after.identity->instanceId, endpoint->instanceId);
        EXPECT_EQ(after.identity->generation, endpoint->generation);
        EXPECT_EQ(after.identity->projectPath, endpoint->projectPath);
        EXPECT_EQ(after.state->state, oracle.state);
        EXPECT_EQ(after.state->mode, oracle.mode);
        EXPECT_EQ(after.state->playing, oracle.playing);
        EXPECT_EQ(after.state->paused, oracle.paused);
        EXPECT_EQ(after.state->frame, oracle.frame);
    }
    (*unchanged)->shutdown();
}

TEST_F(RealEngineWorkbench, StartEditorRunsATypedOwnedConfigurationAndAuthenticatesBeforeStop) {
    HiddenEngineLauncher launcher;
    auto configured = options();
    configured.launcher = &launcher;
    WorkbenchRig rig(dir, configured);
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto& configs = rig.workbench->runController()->configurations().all();
            return std::ranges::any_of(configs,
                                       [](const auto& item) { return item.type == frost::run::RunType::FrostEditor; });
        },
        30s));
    auto started = rig.workbench->startEngineEditor();
    ASSERT_TRUE(started) << started.error().describe();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("connected").value_or(false); }, 120s));
    const auto state = rig.workbench->engineState();
    EXPECT_FALSE(state.stringAt("selectedInstanceId").empty());
    EXPECT_GT(state.unsignedAt("connectionGeneration").value_or(0), 0U);
    EXPECT_GT(state.unsignedAt("heartbeatTimeoutMs").value_or(0), 0U);
    auto status = rig.workbench->runManager()->status(*started);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->configuration.type, frost::run::RunType::FrostEditor);
    EXPECT_FALSE(hasConsoleText(rig, "\"token\""));
    rig.workbench->runManager()->stop(*started);
    ASSERT_TRUE(waitFor(
        [&] {
            return !rig.workbench->engineState().boolAt("connected").value_or(false) &&
                   !rig.workbench->runManager()->status(*started).has_value();
        },
        30s));
}

TEST_F(RealPlayerWorkbench, SelectedPlayerRunAuthenticatesPushesActualPlayerStateAndToolbarStopEndsTheOwnedRun) {
    HiddenEngineLauncher launcher;
    auto configured = options();
    configured.launcher = &launcher;
    WorkbenchRig rig(dir, configured);
    ASSERT_NO_FATAL_FAILURE(open(rig));
    auto* controller = rig.workbench->runController();
    ASSERT_NE(controller, nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            return std::ranges::any_of(controller->configurations().all(),
                                       [](const auto& item) { return item.type == frost::run::RunType::FrostPlayer; });
        },
        30s));
    auto configurations = controller->configurations().all();
    auto player =
        std::ranges::find(configurations, frost::run::RunType::FrostPlayer, &frost::run::RunConfiguration::type);
    ASSERT_NE(player, configurations.end());
    player->arguments = {"--headless"};
    const std::string id = player->id;
    ASSERT_TRUE(controller->replaceConfigurations(configurations));
    ASSERT_TRUE(controller->selectConfiguration(id));
    auto* run = rig.window->mainToolbar().button(frost::ui::MainToolbar::kRun);
    ASSERT_NE(run, nullptr);
    ASSERT_TRUE(run->isEnabled());
    run->click();
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            return controller->configurationRun(id).has_value() && state.boolAt("connected") == true &&
                   state.stringAt("role") == "player" && !state.stringAt("state").empty();
        },
        120s));
    const auto runId = controller->configurationRun(id);
    ASSERT_TRUE(runId);
    const auto status = rig.workbench->runManager()->status(*runId);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->configuration.type, frost::run::RunType::FrostPlayer);
    EXPECT_EQ(frost::io::normalizePath(status->configuration.engineProjectPath), frost::io::normalizePath(project));
    const auto state = rig.workbench->engineState();
    EXPECT_GT(state.unsignedAt("heartbeatTimeoutMs").value_or(0), 0U);
    EXPECT_FALSE(state.stringAt("selectedInstanceId").empty());
    const auto* events = state.find("events");
    ASSERT_NE(events, nullptr);
    EXPECT_TRUE(std::ranges::any_of(events->items(), [&](const Json& event) {
        const auto* data = event.find("data");
        return event.stringAt("topic") == "play.state" && data && data->stringAt("state") == state.stringAt("state") &&
               data->boolAt("paused") == state.boolAt("paused");
    }));
    EXPECT_FALSE(hasConsoleText(rig, "\"token\""));
    auto* stop = rig.window->mainToolbar().button(frost::ui::MainToolbar::kStop);
    ASSERT_NE(stop, nullptr);
    ASSERT_TRUE(stop->isEnabled());
    stop->click();
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->engineState().boolAt("connected") == false &&
                   !rig.workbench->runManager()->status(*runId) && !controller->configurationRunning(id);
        },
        30s));
}

TEST_F(RealEngineWorkbench, FullExternalRestartRequiresExplicitReplacementSelectionAmongSameProjectInstances) {
    frost::test::RealEditor original, distractor;
    auto first = original.start(build, project, config);
    auto other = distractor.start(build, project, config);
    ASSERT_TRUE(first) << first.error().describe();
    ASSERT_TRUE(other) << other.error().describe();
    WorkbenchRig rig(dir, options());
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_NO_FATAL_FAILURE(pair(rig, *first));
    auto play = rig.workbench->sendEngineCommand("play.start", Json::object());
    ASSERT_TRUE(play) << play.error().describe();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("playing") == true; }, 20s));
    auto pause = rig.workbench->sendEngineCommand("play.pause", Json::object().set("paused", true));
    ASSERT_TRUE(pause) << pause.error().describe();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("paused") == true; }, 20s));
    const auto previous = rig.workbench->engineState();
    const auto timeout = previous.unsignedAt("heartbeatTimeoutMs");
    ASSERT_TRUE(timeout);
    ASSERT_GT(*timeout, 0U);
    ASSERT_TRUE(original.kill());
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("connected") == false; },
                        std::chrono::milliseconds(*timeout)));
    original.stop();
    frost::test::RealEditor replacement;
    auto next = replacement.start(build, project, config);
    ASSERT_TRUE(next) << next.error().describe();
    ASSERT_NE(next->instanceId, first->instanceId);
    ASSERT_NE(next->generation, first->generation);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->engineState().boolAt("discoveryBusy") == false; }, 30s));
    ASSERT_TRUE(rig.workbench->refreshEngineInstances());
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            const auto* instances = state.find("instances");
            if (state.boolAt("discoveryBusy") != false || !instances)
                return false;
            const auto listed = [&](std::string_view id) {
                return std::ranges::any_of(instances->items(),
                                           [&](const Json& instance) { return instance.stringAt("instanceId") == id; });
            };
            return listed(next->instanceId) && listed(other->instanceId);
        },
        30s));
    const auto unpaired = rig.workbench->engineState();
    EXPECT_EQ(unpaired.boolAt("connected"), false);
    EXPECT_EQ(unpaired.stringAt("selectedInstanceId"), first->instanceId);
    EXPECT_NE(unpaired.stringAt("selectedInstanceId"), other->instanceId);
    EXPECT_NE(unpaired.stringAt("selectedInstanceId"), next->instanceId);
    ASSERT_TRUE(rig.workbench->pairEngineInstance(next->instanceId));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            return state.boolAt("connected") == true && state.stringAt("selectedInstanceId") == next->instanceId &&
                   state.stringAt("serverGeneration") == next->generation && !state.stringAt("state").empty();
        },
        30s));
    const auto fresh = rig.workbench->engineState();
    EXPECT_EQ(fresh.stringAt("role"), "editor");
    EXPECT_NE(fresh.stringAt("serverGeneration"), previous.stringAt("serverGeneration"));
    EXPECT_EQ(fresh.boolAt("paused"), false);
    EXPECT_EQ(fresh.boolAt("playing"), false);
    const auto* events = fresh.find("events");
    ASSERT_NE(events, nullptr);
    EXPECT_TRUE(std::ranges::any_of(events->items(), [&](const Json& event) {
        const auto* data = event.find("data");
        return event.stringAt("topic") == "play.state" && data && data->stringAt("state") == fresh.stringAt("state");
    }));
    EXPECT_FALSE(std::ranges::any_of(events->items(), [](const Json& event) {
        const auto* data = event.find("data");
        return event.stringAt("topic") == "play.state" && data && data->boolAt("paused") == true;
    })) << "the explicitly replaced session retained the original editor's paused projection";
    auto request = rig.workbench->sendEngineCommand("editor.focus", Json::object());
    ASSERT_TRUE(request) << request.error().describe();
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            const auto* reply = state.find("lastReply");
            return state.unsignedAt("lastRequestId") == *request && reply && !reply->isNull();
        },
        20s));
    EXPECT_EQ(rig.workbench->engineState().stringAt("selectedInstanceId"), next->instanceId);
    EXPECT_EQ(rig.workbench->engineState().stringAt("serverGeneration"), next->generation);
}

TEST_F(RealEngineWorkbench, RealEngineValidatesUnsavedBufferAndReportsState) {
    const std::string path = frost::io::normalizePath(project + "/Assets/Scripts/FeatureTest/Hero.lua").value_or("");
    ASSERT_FALSE(path.empty());
    const auto disk = frost::io::realFileSystem().readFile(path);
    ASSERT_TRUE(disk);
    frost::test::RealEditor editor;
    auto endpoint = editor.start(build, project, config);
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    WorkbenchRig rig(dir, options());
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_NO_FATAL_FAILURE(pair(rig, *endpoint));
    ASSERT_TRUE(rig.workbench->openPath(path));
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->editor().typeText("local r6Invalid = )\n"));
    const auto source = view->editor().document().text().text();
    ASSERT_TRUE(view->document().dirty());
    auto oracle = build.cli("script.validate", project,
                            Json::object().set("path", "Assets/Scripts/FeatureTest/Hero.lua").set("source", source));
    ASSERT_TRUE(oracle) << oracle.error().describe();
    ASSERT_EQ(oracle->boolAt("valid"), false);
    const auto* errors = oracle->find("errors");
    ASSERT_NE(errors, nullptr);
    ASSERT_FALSE(errors->items().empty());
    const auto line = errors->items().front().unsignedAt("line");
    ASSERT_TRUE(line);
    ASSERT_GT(*line, 0U);
    ASSERT_LE(*line, static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
    const auto before = rig.workbench->engineState().unsignedAt("lastRequestId");
    auto* validate = rig.window->findChild<QPushButton*>(QStringLiteral("engineValidateScriptButton"));
    ASSERT_NE(validate, nullptr);
    ASSERT_TRUE(validate->isEnabled());
    validate->click();
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            const auto* reply = state.find("lastReply");
            return state.unsignedAt("lastRequestId") != before && reply && !reply->isNull();
        },
        30s));
    const auto state = rig.workbench->engineState();
    ASSERT_NE(state.find("lastReply"), nullptr);
    EXPECT_EQ(*state.find("lastReply"), *oracle);
    EXPECT_TRUE(view->document().dirty());
    EXPECT_EQ(view->editor().document().text().text(), source);
    EXPECT_EQ(frost::io::realFileSystem().readFile(path), disk);
    auto* console = rig.window->findChild<frost::ui::EngineConsole*>(QStringLiteral("engineConsole"));
    ASSERT_NE(console, nullptr);
    const int expectedLine = static_cast<int>(*line);
    ASSERT_TRUE(waitFor(
        [&] {
            return reportedConsoleFrame(*console, QStringLiteral("script.validate:"), path, expectedLine) != nullptr;
        },
        30s));
    ASSERT_TRUE(
        console->activate(reportedConsoleFrame(*console, QStringLiteral("script.validate:"), path, expectedLine)));
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->currentView() && rig.workbench->currentView()->path() == path &&
                   rig.workbench->currentView()->status().line == expectedLine;
        },
        10s));
    EXPECT_TRUE(view->document().dirty());
    const auto sequence = state.unsignedAt("eventSequence").value_or(0);
    auto play = rig.workbench->sendEngineCommand("play.start", Json::object());
    ASSERT_TRUE(play) << play.error().describe();
    ASSERT_TRUE(waitFor(
        [&] {
            const auto pushed = rig.workbench->engineState();
            return pushed.unsignedAt("eventSequence").value_or(0) > sequence && pushed.boolAt("playing") == true;
        },
        20s));
    EXPECT_EQ(frost::io::realFileSystem().readFile(path), disk);
}

TEST_F(RealEngineWorkbench, RealEngineTestFailureAndLogLocationsNavigate) {
    constexpr std::string_view failureMarker = "R6 intentional test failure";
    constexpr std::string_view logMarker = "R6 real navigable integration log";
    ASSERT_TRUE(frost::test::installAcceptanceScript(
        project,
        "local Probe = {}\nfunction Probe:OnCreate()\n print('R6 real navigable integration log')\n"
        " Test.Case('R6 navigation case', function()\n  Test.Assert(false, 'R6 intentional test failure')\n end)\n"
        "end\nreturn Probe\n"));
    const auto params = Json::object()
                            .set("scenes", Json::array().push("Tests/FeatureTest.fscene"))
                            .set("frames", 1)
                            .set("headless", true);
    auto oracle = build.cli("test.run", project, params);
    ASSERT_TRUE(oracle) << oracle.error().describe();
    const auto* scenes = oracle->find("scenes");
    ASSERT_NE(scenes, nullptr);
    std::optional<Json> failure;
    for (const auto& scene : scenes->items()) {
        const auto* results = scene.find("testResults");
        const auto* failures = results ? results->find("failures") : nullptr;
        if (failures)
            for (const auto& entry : failures->items())
                if (entry.stringAt("message").contains(failureMarker))
                    failure = entry;
    }
    ASSERT_TRUE(failure) << "real test.run did not report the seeded failing case";
    const auto line = failure->unsignedAt("line");
    ASSERT_TRUE(line);
    ASSERT_GT(*line, 0U);
    ASSERT_LE(*line, static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
    const auto path = frost::io::normalizePath(project + "/" + std::string(failure->stringAt("path"))).value_or("");
    ASSERT_FALSE(path.empty());
    frost::test::RealEditor editor;
    auto endpoint = editor.start(build, project, config);
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    WorkbenchRig rig(dir, options());
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_NO_FATAL_FAILURE(pair(rig, *endpoint));
    auto* console = rig.window->findChild<frost::ui::EngineConsole*>(QStringLiteral("engineConsole"));
    ASSERT_NE(console, nullptr);
    const auto before = rig.workbench->engineState().unsignedAt("lastRequestId");
    ASSERT_TRUE(console->submitCommand(QStringLiteral("test.run"), QString::fromStdString(params.dump())));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            const auto* reply = state.find("lastReply");
            return state.unsignedAt("lastRequestId") != before && reply && reply->find("scenes");
        },
        30s));
    const int expectedLine = static_cast<int>(*line);
    ASSERT_TRUE(waitFor(
        [&] { return reportedConsoleFrame(*console, QStringLiteral("test.run:"), path, expectedLine) != nullptr; },
        30s));
    ASSERT_TRUE(console->activate(reportedConsoleFrame(*console, QStringLiteral("test.run:"), path, expectedLine)));
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->currentView() && rig.workbench->currentView()->path() == path &&
                   rig.workbench->currentView()->status().line == expectedLine;
        },
        10s));
    ASSERT_TRUE(waitFor([&] { return hasConsoleText(rig, logMarker); }, 30s));
    const auto live = rig.workbench->engineState();
    const auto* events = live.find("events");
    ASSERT_NE(events, nullptr);
    std::optional<Json> logSource;
    for (const auto& event : events->items()) {
        const auto* data = event.find("data");
        if (event.stringAt("topic") == "log.entry" && data && data->stringAt("message").contains(logMarker))
            if (const auto* source = data->find("source"))
                logSource = *source;
    }
    ASSERT_TRUE(logSource) << "real pushed log omitted its navigable source";
    const auto logLine = logSource->unsignedAt("line");
    ASSERT_TRUE(logLine);
    ASSERT_GT(*logLine, 0U);
    ASSERT_LE(*logLine, static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
    const std::string reportedPath(logSource->stringAt("path"));
    const auto logPath =
        frost::io::normalizePath(frost::io::isAbsolutePath(reportedPath) ? reportedPath : project + "/" + reportedPath)
            .value_or("");
    ASSERT_FALSE(logPath.empty());
    ASSERT_TRUE(waitFor(
        [&] {
            return reportedConsoleFrame(*console,
                                        QString::fromUtf8(logMarker.data(), static_cast<qsizetype>(logMarker.size())),
                                        logPath, static_cast<int>(*logLine)) != nullptr;
        },
        30s));
    auto* frame =
        reportedConsoleFrame(*console, QString::fromUtf8(logMarker.data(), static_cast<qsizetype>(logMarker.size())),
                             logPath, static_cast<int>(*logLine));
    ASSERT_TRUE(console->activate(frame));
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->currentView() && rig.workbench->currentView()->path() == logPath &&
                   rig.workbench->currentView()->status().line == static_cast<int>(*logLine);
        },
        10s));
}

TEST_F(RealEngineWorkbench, RevokedTrustCompletesOwnedEditorAndShutdownPreservesPairedExternalEditor) {
    frost::test::RealEditor external;
    auto endpoint = external.start(build, project, config);
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    HiddenEngineLauncher launcher;
    auto configured = options();
    configured.launcher = &launcher;
    WorkbenchRig rig(dir, configured);
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_NO_FATAL_FAILURE(pair(rig, *endpoint));
    rig.workbench->disconnectEngineSession();
    auto owned = rig.workbench->startEngineEditor();
    ASSERT_TRUE(owned) << owned.error().describe();
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            return state.boolAt("connected") == true && state.stringAt("selectedInstanceId") != endpoint->instanceId;
        },
        120s));
    const auto sent = rig.workbench->engineState().unsignedAt("sentCommandCount");
    ASSERT_TRUE(rig.trust->untrust(project));
    auto denied = rig.workbench->sendEngineCommand("play.start", Json::object());
    EXPECT_FALSE(denied);
    EXPECT_EQ(rig.workbench->engineState().unsignedAt("sentCommandCount"), sent);
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runManager()->status(*owned); }, 30s));
    EXPECT_TRUE(rig.workbench->runManager()->waitForIdle(30s))
        << "revocation did not reach actual RunManager process-wait completion";
    rig.workbench->shutdown();
    frost::engine::EngineSessionOptions observation;
    observation.endpoint = *endpoint;
    observation.trusted = [](std::string_view, std::uint64_t) { return true; };
    auto survivor = frost::engine::EngineSession::create(std::move(observation));
    ASSERT_TRUE(survivor) << survivor.error().describe();
    ASSERT_TRUE(waitFor([&] { return (*survivor)->connected(); }, 20s))
        << "Workbench shutdown terminated an externally paired editor";
    (*survivor)->shutdown();
}
TEST_F(RealEngineWorkbench, ActualSceneListingAndSelectedRunPopulateTheWorkbenchUnitTestsProvider) {
    auto& fs = frost::io::realFileSystem();
    ASSERT_TRUE(frost::test::installAcceptanceScript(
        project,
        "local Cases = {}\nfunction Cases:OnCreate()\n"
        "  Test.Case('real composed passing case', function() Test.Equal(2 + 2, 4) end)\n"
        "  Test.Case('real composed failing case', function() Test.Equal(2 + 2, 5, 'real composed failure') end)\n"
        "end\nreturn Cases\n"));
    ASSERT_TRUE(build.cli("assets.refresh", project));
    auto rawListing = build.cli("test.list", project, Json::object().set("folder", "Tests"));
    ASSERT_TRUE(rawListing) << rawListing.error().describe();
    auto listing = frost::engine::parseEngineTestListing(*rawListing);
    ASSERT_TRUE(listing) << listing.error().describe();
    ASSERT_FALSE(listing->tests.empty());
    const auto selected =
        std::ranges::find(listing->tests, "Assets/Tests/FeatureTest.fscene", &frost::engine::EngineSceneTest::scene);
    ASSERT_NE(selected, listing->tests.end());
    // Match the actual product provider's default invocation; the independent CLI owns the oracle.
    const frost::engine::EngineTestProviderOptions defaults;
    auto rawRun = build.cli("test.run", project,
                            Json::object()
                                .set("scenes", Json::array({selected->scene}))
                                .set("frames", defaults.frames)
                                .set("mode", "headless"));
    ASSERT_TRUE(rawRun) << rawRun.error().describe();
    const std::array scenes{selected->scene};
    auto reports = frost::engine::parseEngineTestRun(*rawRun, scenes);
    ASSERT_TRUE(reports) << reports.error().describe();
    ASSERT_EQ(reports->size(), scenes.size());
    const auto& oracle = reports->front();
    ASSERT_EQ(oracle.outcome, frost::engine::EngineTestOutcome::Failed);
    ASSERT_FALSE(oracle.cases.empty());
    ASSERT_FALSE(oracle.failures.empty());

    WorkbenchRig rig(dir, options());
    ASSERT_NO_FATAL_FAILURE(open(rig));
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->engineState().boolAt("discoveryBusy").value_or(true); }, 30s));
    rig.window->toolWindows().show(frost::app::kUnitTestsToolWindow, false);
    auto* providers = qobject_cast<QTabWidget*>(rig.window->toolWindows().content(frost::app::kUnitTestsToolWindow));
    ASSERT_NE(providers, nullptr);
    auto* widget = providers->findChild<QWidget*>(QStringLiteral("engineUnitTestsProvider"));
    ASSERT_NE(widget, nullptr);
    providers->setCurrentWidget(widget);
    auto* tree = widget->findChild<QTreeWidget*>(QStringLiteral("engineTestsTree"));
    auto* runSelected = widget->findChild<QPushButton*>(QStringLiteral("engineTestsRunSelected"));
    auto* runAll = widget->findChild<QPushButton*>(QStringLiteral("engineTestsRunAll"));
    auto* runFailed = widget->findChild<QPushButton*>(QStringLiteral("engineTestsRunFailed"));
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(runSelected, nullptr);
    ASSERT_NE(runAll, nullptr);
    ASSERT_NE(runFailed, nullptr);
    ASSERT_TRUE(waitFor([&] { return runAll->isEnabled(); }, 30s));
    ASSERT_EQ(static_cast<std::size_t>(tree->topLevelItemCount()), listing->tests.size());
    QTreeWidgetItem* selectedRow = nullptr;
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        auto* item = tree->topLevelItem(index);
        const auto& actual = listing->tests[static_cast<std::size_t>(index)];
        EXPECT_EQ(item->text(0).toStdString(), actual.scene);
        EXPECT_EQ(item->data(0, Qt::UserRole).toString().toStdString(), actual.id);
        EXPECT_EQ(item->childCount(), 0); // test.list does not invent runtime Test.Case declarations.
        if (actual.id == selected->id)
            selectedRow = item;
    }
    ASSERT_NE(selectedRow, nullptr);
    tree->setCurrentItem(selectedRow);
    ASSERT_TRUE(runSelected->isEnabled());
    runSelected->click();
    ASSERT_TRUE(waitFor([&] { return runAll->isEnabled() && runFailed->isEnabled(); }, 30s));
    // render() replaces tree items. Reacquire by the real scene identity after the asynchronous run.
    selectedRow = nullptr;
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
        if (tree->topLevelItem(index)->data(0, Qt::UserRole).toString().toStdString() == selected->id)
            selectedRow = tree->topLevelItem(index);
    ASSERT_NE(selectedRow, nullptr);
    EXPECT_EQ(selectedRow->text(1).toStdString(), frost::engine::toString(oracle.outcome));
    ASSERT_EQ(static_cast<std::size_t>(selectedRow->childCount()), oracle.cases.size() + oracle.failures.size());
    for (std::size_t index = 0; index < oracle.cases.size(); ++index) {
        const auto& expected = oracle.cases[index];
        const auto* child = selectedRow->child(static_cast<int>(index));
        EXPECT_EQ(child->text(0).toStdString(),
                  expected.peer.empty() ? expected.name : expected.peer + ": " + expected.name);
        EXPECT_EQ(child->text(1).toStdString(), frost::engine::toString(expected.outcome));
        if (!expected.duration)
            EXPECT_EQ(child->text(2), QString::fromUtf8("—"));
    }
    const auto failure =
        std::ranges::find_if(oracle.failures, [](const auto& value) { return value.location.has_value(); });
    ASSERT_NE(failure, oracle.failures.end());
    const auto failureIndex = static_cast<std::size_t>(std::distance(oracle.failures.begin(), failure));
    auto* failureRow = selectedRow->child(static_cast<int>(oracle.cases.size() + failureIndex));
    EXPECT_EQ(failureRow->text(0).toStdString(), failure->message);
    EXPECT_EQ(failureRow->data(0, Qt::UserRole + 2).toString().toStdString(), failure->location->path);
    EXPECT_EQ(failureRow->data(0, Qt::UserRole + 3).toULongLong(), failure->location->line);
    tree->setCurrentItem(failureRow);
    EXPECT_FALSE(runSelected->isEnabled()); // Runtime children cannot be silently rerun as selectable scenes.
    const auto expectedPath = fs.physicalPath(project + "/" + failure->location->path);
    ASSERT_TRUE(expectedPath);
    tree->itemDoubleClicked(failureRow, 0);
    ASSERT_TRUE(waitFor(
        [&] {
            auto* view = rig.workbench->currentView();
            return view && fs.physicalPath(view->path()).value_or("") == *expectedPath &&
                   static_cast<std::uint64_t>(view->status().line) == failure->location->line;
        },
        10s));
}
#ifdef _WIN32
// A fixed fault in this test's own disposable editor, never an externally selected process.
// Retaining the native process handle and then proving the original launcher owner remains alive
// excludes PID reuse. No enumeration/name targeting or configurable executable payload exists.
frost::Result<void> faultOwnedEditor(frost::test::RealEditor& editor, frost::io::ProcessId expectedPid,
                                     const std::string& expectedImage) {
    using Handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)>;
    // CreateRemoteThread's documented required rights, not PROCESS_ALL_ACCESS:
    // https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-createremotethread
    constexpr DWORD rights =
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    Handle process(OpenProcess(rights, FALSE, expectedPid), &CloseHandle);
    if (!process)
        return std::unexpected(frost::io::lastWin32Error("owned editor fault: acquire process"));
    auto liveOwner = editor.requireLiveOwnedProcess(expectedPid);
    if (!liveOwner)
        return liveOwner;
    std::array<wchar_t, 32768> image{};
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &length))
        return std::unexpected(frost::io::lastWin32Error("owned editor fault: query image"));
    auto actualImage = frost::io::fromWide(std::wstring_view(image.data(), length));
    if (!actualImage)
        return std::unexpected(actualImage.error());
    auto actual = frost::io::realFileSystem().physicalPath(*actualImage);
    auto expected = frost::io::realFileSystem().physicalPath(expectedImage);
    if (!actual || !expected || *actual != *expected)
        return frost::fail(frost::ErrorCode::PermissionDenied,
                           "owned editor fault: actual image differs from pinned editor");

    // UD2 raises a genuine illegal-instruction exception through the installed engine CrashHandler.
    // Allocation is writable only during initialization, then executable/read-only. After thread
    // start it must remain until owned-process teardown; freeing it while executing is forbidden.
    constexpr std::array<unsigned char, 3> fault{0x0f, 0x0b, 0xc3};
    void* code = VirtualAllocEx(process.get(), nullptr, fault.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!code)
        return std::unexpected(frost::io::lastWin32Error("owned editor fault: allocate fixed stub"));
    struct Allocation {
        HANDLE process;
        void* code;
        bool started = false;
        ~Allocation() {
            if (!started)
                static_cast<void>(VirtualFreeEx(process, code, 0, MEM_RELEASE));
        }
    } allocation{process.get(), code};
    SIZE_T written = 0;
    if (!WriteProcessMemory(process.get(), code, fault.data(), fault.size(), &written))
        return std::unexpected(frost::io::lastWin32Error("owned editor fault: write fixed stub"));
    if (written != fault.size())
        return frost::fail(frost::ErrorCode::IoError, "owned editor fault: incomplete fixed stub write");
    DWORD previous = 0;
    if (!VirtualProtectEx(process.get(), code, fault.size(), PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(process.get(), code, fault.size()))
        return std::unexpected(frost::io::lastWin32Error("owned editor fault: finalize fixed stub"));
    Handle thread(CreateRemoteThread(process.get(), nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(code), nullptr,
                                     0, nullptr),
                  &CloseHandle);
    if (!thread)
        return std::unexpected(frost::io::lastWin32Error("owned editor fault: start fixed fault"));
    allocation.started = true;
    return {};
}

TEST_F(RealEngineWorkbench, ActualEditorCrashOffersAndOpensARealReadOnlyDumpSession) {
    auto& fs = frost::io::realFileSystem();
    const auto adapter = fs.physicalPath(FROST_DBGENG_ADAPTER_PATH);
    ASSERT_TRUE(adapter) << adapter.error().describe();
    auto configured = options();
    configured.workbench.engineDumpAdapterPath = *adapter;
    configured.workbench.engineDumpCacheDirectory = dir.join("private-dump-cache");
    ASSERT_TRUE(fs.createDirectories(configured.workbench.engineDumpCacheDirectory));
    WorkbenchRig rig(dir, configured);
    ASSERT_NO_FATAL_FAILURE(open(rig));
    frost::test::RealEditor editor;
    auto endpoint = editor.start(build, project, config);
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    ASSERT_NO_FATAL_FAILURE(pair(rig, *endpoint));
    const auto authenticatedPid = rig.workbench->engineState().integerAt("processId");
    ASSERT_TRUE(authenticatedPid);
    ASSERT_GT(*authenticatedPid, 0);
    ASSERT_LE(static_cast<std::uint64_t>(*authenticatedPid), std::numeric_limits<frost::io::ProcessId>::max());
    const auto ownedPid = editor.ownedPid();
    ASSERT_EQ(static_cast<std::uint64_t>(ownedPid), static_cast<std::uint64_t>(*authenticatedPid));
    auto* observer = rig.workbench->findChild<frost::app::EngineCrashObserver*>();
    ASSERT_NE(observer, nullptr);
    ASSERT_TRUE(
        waitFor([&] { return observer->status().phase == frost::app::EngineCrashObservationPhase::Armed; }, 30s));
    auto* sessions = rig.workbench->debugSessions();
    ASSERT_NE(sessions, nullptr);
    const auto sessionsBefore = sessions->sessions().size();
    EXPECT_FALSE(sessions->anyDebugging());
    auto fault = faultOwnedEditor(editor, ownedPid, build.program);
    ASSERT_TRUE(fault) << fault.error().describe();
    // Same actual crash completion deadline as the existing real engine crash-artifact oracle.
    auto exited = editor.waitForOwnedExit(120s);
    ASSERT_TRUE(exited) << exited.error().describe();
    ASSERT_NE(exited->exitCode, 0) << "the fixed fault did not crash the actual engine";
    std::optional<frost::ui::NotificationAction> openDump;
    ASSERT_TRUE(waitFor(
        [&] {
            for (const auto& entry : rig.window->notifications().log())
                if (entry.notification.title == QStringLiteral("FrostEngine crash dump"))
                    for (const auto& action : entry.notification.actions)
                        if (action.label == QStringLiteral("Open dump")) {
                            openDump = action;
                            return true;
                        }
            return false;
        },
        30s));
    EXPECT_EQ(observer->status().phase, frost::app::EngineCrashObservationPhase::Offered);
    EXPECT_EQ(sessions->sessions().size(), sessionsBefore);
    EXPECT_FALSE(sessions->anyDebugging()) << "a crash notification must not auto-launch a debugger";
    ASSERT_TRUE(openDump && openDump->run);
    openDump->run(); // The real Workbench action selects the actual private dump through real DbgEng.
    ASSERT_TRUE(waitFor(
        [&] {
            auto* selected = sessions->selected();
            return sessions->sessions().size() > sessionsBefore && selected && selected->readOnlySession() &&
                   selected->adapterStoppedEvidence() && selected->state() == frost::debug::DebugState::Stopped &&
                   !selected->frames().frames.empty();
        },
        30s));
    auto* dump = sessions->selected();
    ASSERT_NE(dump, nullptr);
    ASSERT_NE(dump->session(), nullptr);
    ASSERT_TRUE(dump->session()->stopInfo());
    EXPECT_TRUE(dump->session()->stopInfo()->isCrash());
    auto cached = fs.listDirectory(configured.workbench.engineDumpCacheDirectory);
    ASSERT_TRUE(cached);
    std::optional<std::string> actualDump;
    for (const auto& directory : *cached) {
        if (directory.type != frost::io::FileType::Directory)
            continue;
        auto privateDirectory = frost::io::joinPath(configured.workbench.engineDumpCacheDirectory, directory.name);
        ASSERT_TRUE(privateDirectory);
        auto files = fs.listDirectory(*privateDirectory);
        ASSERT_TRUE(files);
        for (const auto& file : *files)
            if (file.type == frost::io::FileType::Regular && file.name.ends_with(".dmp")) {
                auto path = frost::io::joinPath(*privateDirectory, file.name);
                ASSERT_TRUE(path);
                auto bytes = fs.readFile(*path);
                ASSERT_TRUE(bytes);
                auto validated = frost::engine::validateCrashMinidump(*bytes, ownedPid);
                ASSERT_TRUE(validated) << validated.error().describe();
                ASSERT_FALSE(actualDump) << "one action created more than one correlated private dump";
                actualDump = *path;
            }
    }
    ASSERT_TRUE(actualDump) << "the actual Workbench action produced no private correlated minidump";
    EXPECT_FALSE(dump->resume());
    EXPECT_FALSE(dump->pause());
    EXPECT_FALSE(dump->stepOver());
    EXPECT_FALSE(dump->stepInto());
    EXPECT_FALSE(dump->stepOut());
    std::optional<frost::ErrorCode> mutationError;
    dump->session()->evaluate("x = 7", dump->frames().frames.front().id, "repl", [&](auto result) {
        mutationError = result ? frost::ErrorCode::Unknown : result.error().code();
    });
    ASSERT_TRUE(waitFor([&] { return mutationError.has_value(); }, 10s));
    EXPECT_EQ(*mutationError, frost::ErrorCode::Unsupported);
    EXPECT_EQ(dump->state(), frost::debug::DebugState::Stopped);
    EXPECT_TRUE(dump->readOnlySession());
    dump->stop();
    ASSERT_TRUE(waitFor([&] { return !dump->debugging(); }, 10s));
}
#endif
} // namespace
