// SPDX-License-Identifier: Apache-2.0
/*
 * RUN-2/RUN-3 in the composed workbench with real tools (label integration, tag cmake):
 *   - a CMake project's run configurations are its EXECUTABLE targets, as the File API reply the
 *     pinned cmake wrote says (the oracle reads the reply JSON itself), and edits made as the
 *     Run/Debug Configurations dialog's OK makes them persist in the workspace store and reach the
 *     built program (its arguments, environment and working directory, printed by the program);
 *     the gutter marker on a target's main() runs that target's configuration;
 *   - a native configuration runs from the toolbar's Run button through the RunManager: its output
 *     streams into its tab of the Run tool window, its exit code is the program's, and the toolbar's
 *     Stop ends its whole process tree (run_tree_child; every reported process is watched by an OS
 *     handle until it exits);
 *   - an untrusted workspace starts no native configuration (the trust dialog asks first).
 */

#include "frost/app/document_editor.hpp"
#include "frost/build/cmake_service.hpp"
#include "frost/io/path.hpp"
#include "frost/rpc/json.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/run/run_types.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/run_tool_window.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "io/process_env.hpp"
#include "workbench_rig.hpp"
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QToolButton>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
namespace io = frost::io;
namespace run = frost::run;

// The reply oracle: the names of the EXECUTABLE targets in the newest File API reply of `build`,
// read straight from cmake's JSON (index -> codemodel -> each target's file).
std::vector<std::string> executableTargetsInReply(const std::string& build) {
    auto& fs = io::realFileSystem();
    const std::string reply = io::joinPath(build, ".cmake/api/v1/reply").value_or("");
    auto entries = fs.listDirectory(reply);
    EXPECT_TRUE(entries.has_value()) << reply;
    if (!entries)
        return {};
    std::string index;
    for (const io::DirEntry& entry : *entries)
        if (entry.name.starts_with("index-") && entry.name.ends_with(".json") && entry.name > index)
            index = entry.name; // index-<timestamp>.json: the newest sorts last.
    const auto read = [&](const std::string& name) {
        auto text = fs.readFile(io::joinPath(reply, name).value_or(""));
        EXPECT_TRUE(text.has_value()) << name;
        auto json = frost::rpc::parseJson(text.value_or("{}"));
        EXPECT_TRUE(json.has_value()) << name;
        return json.value_or(frost::rpc::Json::object());
    };
    const auto indexJson = read(index);
    std::string codemodel;
    if (const auto* objects = indexJson.find("objects"))
        for (const auto& object : objects->items())
            if (object.stringAt("kind") == "codemodel")
                codemodel = std::string(object.stringAt("jsonFile"));
    EXPECT_FALSE(codemodel.empty()) << "the reply has no codemodel";
    const auto model = read(codemodel);
    std::vector<std::string> names;
    const auto* configurations = model.find("configurations");
    if (configurations == nullptr || configurations->items().empty())
        return names;
    if (const auto* targets = configurations->items().front().find("targets"))
        for (const auto& target : targets->items()) {
            const auto file = read(std::string(target.stringAt("jsonFile")));
            if (file.stringAt("type") == "EXECUTABLE")
                names.emplace_back(file.stringAt("name"));
        }
    std::ranges::sort(names);
    return names;
}

// The Run tool window tab of `key`, or null.
frost::ui::RunConsole* tabOf(WorkbenchRig& rig, const QString& key) {
    return rig.workbench->runToolWindow() ? rig.workbench->runToolWindow()->consoleFor(key) : nullptr;
}

QToolButton* toolbarButton(WorkbenchRig& rig, std::string_view id) { return rig.window->mainToolbar().button(id); }

TEST(WorkbenchRun, CMakeExecutablesAreRunConfigurationsAndTheirEditsReachTheProgram) {
    WorkDir dir("run-cmake-configurations");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                                    "\" CACHE FILEPATH \"\" FORCE)\nproject(Configurations C)\n"
                                    "add_library(gamma STATIC gamma.c)\nadd_executable(alpha alpha.c)\n"
                                    "target_link_libraries(alpha PRIVATE gamma)\nadd_executable(beta beta.c)\n");
    dir.write("gamma.c", "int gamma_value(void) { return 7; }\n");
    dir.write("alpha.c", "#include <stdio.h>\n#include <stdlib.h>\nint gamma_value(void);\n"
                         "int main(int argc, char** argv) {\n"
                         "  for (int i = 1; i < argc; ++i) printf(\"arg[%d]=%s\\n\", i, argv[i]);\n"
                         "  const char* value = getenv(\"FROST_RUN_TEST_VALUE\");\n"
                         "  printf(\"env=%s\\n\", value ? value : \"(unset)\");\n"
                         "  FILE* marker = fopen(\"cwd-marker.txt\", \"r\");\n"
                         "  printf(\"cwd-marker=%s\\n\", marker ? \"found\" : \"missing\");\n"
                         "  if (marker) fclose(marker);\n"
                         "  return gamma_value() - 4;\n}\n");
    dir.write("beta.c", "#include <stdio.h>\nint main(void) {\n  puts(\"beta ran\");\n  return 5;\n}\n");
    dir.write("work/cwd-marker.txt", "here\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->model() && service->state() == frost::build::BuildState::Idle; }, 240s));
    auto* controller = rig.workbench->runController();
    ASSERT_NE(controller, nullptr);

    // RUN-2: one cmake-target configuration per EXECUTABLE target of the reply cmake wrote.
    const auto profile = service->activeProfile();
    ASSERT_TRUE(profile.has_value());
    const std::vector<std::string> oracle = executableTargetsInReply(profile->buildDirectory);
    ASSERT_FALSE(oracle.empty());
    ASSERT_TRUE(waitFor([&] { return controller->configurations().generated().size() == oracle.size(); }, 60s))
        << controller->configurations().generated().size() << " generated, the reply has " << oracle.size();
    std::vector<std::string> generated;
    for (const run::RunConfiguration& configuration : controller->configurations().generated()) {
        EXPECT_EQ(configuration.type, run::RunType::CMakeTarget) << configuration.id;
        EXPECT_EQ(configuration.id, run::runConfigurationIdFor(run::kCMakeIdPrefix, configuration.target));
        generated.push_back(configuration.target);
    }
    std::ranges::sort(generated);
    EXPECT_EQ(generated, oracle);
    // The toolbar's combo lists them (and its Edit Configurations... entry).
    auto* combo = rig.window->mainToolbar().runConfigurations();
    for (const std::string& name : oracle)
        EXPECT_GE(combo->findText(QString::fromStdString(name)), 0) << name;

    // RUN-3: the dialog's OK with edits to alpha; they persist in the workspace store.
    std::vector<run::RunConfiguration> edited = controller->configurations().all();
    const auto alpha = std::ranges::find(edited, std::string("alpha"), &run::RunConfiguration::target);
    ASSERT_NE(alpha, edited.end());
    const std::string alphaId = alpha->id;
    alpha->arguments = {"one", "two words"};
    alpha->environment = {{"FROST_RUN_TEST_VALUE", std::string("from-configuration")}};
    alpha->workingDirectory = dir.join("work");
    ASSERT_TRUE(controller->replaceConfigurations(edited).has_value());
    const auto storePath = run::workspaceRunConfigurationsPath(dir.path());
    ASSERT_TRUE(storePath.has_value());
    auto stored = run::loadRunConfigurations(io::realFileSystem(), *storePath);
    ASSERT_TRUE(stored.has_value()) << stored.error().describe();
    ASSERT_EQ(stored->size(), 1U) << "only the edited generated configuration is stored";
    EXPECT_EQ(stored->front().id, alphaId);
    EXPECT_EQ(stored->front().arguments, (std::vector<std::string>{"one", "two words"}));

    // Run from the toolbar: the target builds, then its program runs with the edits.
    ASSERT_TRUE(controller->selectConfiguration(alphaId).has_value());
    EXPECT_EQ(combo->currentText(), QStringLiteral("alpha"));
    auto* runButton = toolbarButton(rig, frost::ui::MainToolbar::kRun);
    ASSERT_NE(runButton, nullptr);
    ASSERT_TRUE(waitFor([&] { return runButton->isEnabled(); }, 30s));
    runButton->click();
    frost::ui::RunConsole* tab = nullptr;
    ASSERT_TRUE(waitFor(
        [&] {
            tab = tabOf(rig, QStringLiteral("cmake:alpha"));
            return tab && tab->text().contains(QStringLiteral("Process finished with exit code"));
        },
        240s))
        << (tab ? tab->text().toStdString() : std::string("no alpha tab"));
    const QString text = tab->text();
    EXPECT_TRUE(text.contains(QStringLiteral("arg[1]=one\n"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("arg[2]=two words\n"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("env=from-configuration\n"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("cwd-marker=found\n"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("Process finished with exit code 3"))) << text.toStdString();
    EXPECT_TRUE(waitFor([&] { return !controller->running(); }, 30s));

    // The gutter marker on beta.c's main() runs beta's configuration (the target that builds the file).
    ASSERT_TRUE(rig.workbench->openPath(dir.join("beta.c")).has_value());
    ASSERT_NE(rig.view(), nullptr);
    std::optional<frost::ui::LineMarker> marker;
    ASSERT_TRUE(waitFor(
        [&] {
            const auto& markers = rig.view()->editor().lineMarkers();
            const auto found = std::ranges::find_if(
                markers, [](const auto& m) { return m.severity == frost::ui::MarkerSeverity::RunTest; });
            if (found != markers.end())
                marker = *found;
            return marker.has_value();
        },
        30s));
    EXPECT_EQ(marker->message, "Run 'beta'");
    emit rig.view()->editor().gutterRunMarkerClicked(static_cast<int>(marker->line.value()), QPoint());
    frost::ui::RunConsole* betaTab = nullptr;
    ASSERT_TRUE(waitFor(
        [&] {
            betaTab = tabOf(rig, QStringLiteral("cmake:beta"));
            return betaTab && betaTab->text().contains(QStringLiteral("Process finished with exit code"));
        },
        240s))
        << (betaTab ? betaTab->text().toStdString() : std::string("no beta tab"));
    EXPECT_TRUE(betaTab->text().contains(QStringLiteral("beta ran\n"))) << betaTab->text().toStdString();
    EXPECT_TRUE(betaTab->text().contains(QStringLiteral("Process finished with exit code 5")));
    EXPECT_TRUE(waitFor([&] { return !controller->running(); }, 30s));

    // A second workspace session reads the store back.
    ASSERT_TRUE(rig.workbench->closeProject());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(waitFor([&] { return controller->configurations().generated().size() == oracle.size(); }, 240s));
    const run::RunConfiguration* reloaded = controller->configurations().find(alphaId);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->arguments, (std::vector<std::string>{"one", "two words"}));
}

// run_tree_child "tree 1 2": 1 + 2 processes, each printing "pid <n>".
run::RunConfiguration treeConfiguration(std::span<const run::RunConfiguration> taken, std::vector<std::string> args) {
    run::RunConfiguration configuration =
        run::nativeRunConfiguration("Tree", io::normalizePath(FROST_RUN_TREE_CHILD).value(), taken);
    configuration.arguments = std::move(args);
    return configuration;
}

std::set<io::ProcessId> reportedPids(const QString& text) {
    std::set<io::ProcessId> pids;
    for (const QString& line : text.split(QLatin1Char('\n')))
        if (line.startsWith(QStringLiteral("pid ")))
            pids.insert(static_cast<io::ProcessId>(line.mid(4).trimmed().toULongLong()));
    return pids;
}

TEST(WorkbenchRun, ANativeConfigurationRunsFromTheToolbarAndStopEndsItsTree) {
    WorkDir dir("run-native");
    dir.write("readme.txt", "a workspace for native run configurations\n");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* controller = rig.workbench->runController();
    ASSERT_NE(controller, nullptr);
    std::vector<run::RunConfiguration> edited = controller->configurations().all();
    edited.push_back(treeConfiguration(edited, {"tree", "1", "2"}));
    const std::string treeId = edited.back().id;
    run::RunConfiguration usage = treeConfiguration(edited, {"bogus"});
    usage.name = "Usage";
    usage.id = run::uniqueRunConfigurationId(run::runConfigurationIdFor(run::kNativeIdPrefix, "Usage"), edited);
    edited.push_back(usage);
    ASSERT_TRUE(controller->replaceConfigurations(edited).has_value());
    ASSERT_TRUE(controller->selectConfiguration(treeId).has_value());
    EXPECT_EQ(rig.window->mainToolbar().runConfigurations()->currentText(), QStringLiteral("Tree"));

    auto* runButton = toolbarButton(rig, frost::ui::MainToolbar::kRun);
    auto* stopButton = toolbarButton(rig, frost::ui::MainToolbar::kStop);
    ASSERT_NE(runButton, nullptr);
    ASSERT_NE(stopButton, nullptr);
    ASSERT_TRUE(runButton->isEnabled());
    runButton->click();
    const QString key = QStringLiteral("config:") + QString::fromStdString(treeId);
    frost::ui::RunConsole* tab = nullptr;
    // Streamed: the three processes report while the run is still going.
    ASSERT_TRUE(waitFor(
        [&] {
            tab = tabOf(rig, key);
            return tab && reportedPids(tab->text()).size() == 3;
        },
        60s))
        << (tab ? tab->text().toStdString() : std::string("no tab"));
    EXPECT_TRUE(tab->active());
    EXPECT_TRUE(controller->configurationRunning(treeId));
    const auto active = rig.workbench->runManager()->activeRuns();
    ASSERT_EQ(active.size(), 1U);
    EXPECT_EQ(active.front().configuration.id, treeId);
    const std::set<io::ProcessId> pids = reportedPids(tab->text());
    EXPECT_TRUE(pids.contains(active.front().processId)) << "the program is the root of the tree";
    // OS handles on every reported process, so none of their pids is reused while the test looks.
    std::vector<std::unique_ptr<frost::test::ProcessWatch>> watches;
    for (const io::ProcessId pid : pids) {
        watches.push_back(std::make_unique<frost::test::ProcessWatch>(pid));
        ASSERT_TRUE(watches.back()->valid()) << pid;
    }

    ASSERT_TRUE(waitFor([&] { return stopButton->isEnabled(); }, 10s));
    stopButton->click();
    for (const auto& watch : watches) {
        const bool ended = watch->exitsWithin(30s);
        EXPECT_TRUE(ended) << "a process of the tree survived Stop";
        if (!ended)
            watch->kill();
    }
    ASSERT_TRUE(waitFor([&] { return !tab->active(); }, 30s));
    EXPECT_TRUE(tab->text().contains(QStringLiteral("(stopped)"))) << tab->text().toStdString();
    EXPECT_TRUE(rig.workbench->runManager()->waitForIdle(30s));

    // A second configuration gets its own tab; the program's own exit code (a usage error: 64).
    ASSERT_TRUE(controller->selectConfiguration(usage.id).has_value());
    runButton->click();
    frost::ui::RunConsole* usageTab = nullptr;
    ASSERT_TRUE(waitFor(
        [&] {
            usageTab = tabOf(rig, QStringLiteral("config:") + QString::fromStdString(usage.id));
            return usageTab && usageTab->text().contains(QStringLiteral("Process finished with exit code"));
        },
        60s));
    EXPECT_NE(usageTab, tab);
    EXPECT_TRUE(usageTab->text().contains(QStringLiteral("Process finished with exit code 64")))
        << usageTab->text().toStdString();
    EXPECT_EQ(rig.workbench->runToolWindow()->count(), 2);
    EXPECT_TRUE(rig.workbench->runManager()->waitForIdle(30s));
}

TEST(WorkbenchRun, AnUntrustedWorkspaceStartsNoNativeConfiguration) {
    WorkDir dir("run-native-untrusted");
    dir.write("readme.txt", "untrusted\n");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* controller = rig.workbench->runController();
    std::vector<run::RunConfiguration> edited = controller->configurations().all();
    edited.push_back(treeConfiguration(edited, {"tree", "0", "0"}));
    const std::string id = edited.back().id;
    ASSERT_TRUE(controller->replaceConfigurations(edited).has_value());
    auto started = controller->runConfiguration(id);
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), frost::ErrorCode::Untrusted);
    EXPECT_TRUE(rig.workbench->runManager()->activeRuns().empty());
    // The trust dialog asks; cancelling it starts nothing.
    frost::ui::TrustDialog* dialog = nullptr;
    ASSERT_TRUE(waitFor([&] {
        dialog = rig.window->findChild<frost::ui::TrustDialog*>();
        return dialog != nullptr && dialog->isVisible();
    }));
    dialog->reject();
    QApplication::processEvents();
    EXPECT_FALSE(rig.trust->isTrusted(dir.path()));
    EXPECT_TRUE(rig.workbench->runManager()->activeRuns().empty());
    EXPECT_EQ(tabOf(rig, QStringLiteral("config:") + QString::fromStdString(id)), nullptr);
}

TEST(WorkbenchRun, AStoreThatCouldNotBeReadIsNeverOverwritten) {
    /* A damaged store, or one a newer FrostIDE wrote (version 99), is not read when the folder opens.
       Before the fix, the next Run/Debug Configurations OK (or run.edit) replaced it with the list in
       memory: every saved configuration was lost. Now the save is refused, and the file stays byte
       for byte until a reopen reads it. */
    for (const std::string& damaged : {std::string("{\"version\": 1, \"configurations\": [7]}"),
                                       std::string("{\"version\": 99, \"configurations\": []}")}) {
        SCOPED_TRACE(damaged);
        WorkDir dir("run-unreadable-store");
        dir.write("readme.txt", "a workspace whose run configurations cannot be read\n");
        const auto storePath = run::workspaceRunConfigurationsPath(dir.path());
        ASSERT_TRUE(storePath.has_value());
        auto& fs = io::realFileSystem();
        ASSERT_TRUE(fs.createDirectories(io::parentPath(*storePath)).has_value());
        ASSERT_TRUE(fs.writeFileAtomic(*storePath, damaged).has_value());
        WorkbenchRig rig(dir);
        ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
        ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
        auto* controller = rig.workbench->runController();
        ASSERT_NE(controller, nullptr);
        std::vector<run::RunConfiguration> edited = controller->configurations().all();
        edited.push_back(treeConfiguration(edited, {"tree"}));
        auto saved = controller->replaceConfigurations(edited);
        ASSERT_FALSE(saved.has_value()) << "an unreadable store must not be overwritten";
        EXPECT_EQ(saved.error().code(), frost::ErrorCode::Corrupt);
        EXPECT_EQ(fs.readFile(*storePath).value_or("<unreadable>"), damaged);

        // Once the file is gone (or fixed) and the folder is reopened, edits are saved again.
        ASSERT_TRUE(fs.removeFile(*storePath).has_value());
        ASSERT_TRUE(rig.workbench->closeProject()); // Opening the open folder again reads nothing.
        ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
        controller = rig.workbench->runController();
        ASSERT_NE(controller, nullptr);
        edited = controller->configurations().all();
        edited.push_back(treeConfiguration(edited, {"tree"}));
        ASSERT_TRUE(controller->replaceConfigurations(edited).has_value());
        auto reloaded = run::loadRunConfigurations(fs, *storePath);
        ASSERT_TRUE(reloaded.has_value()) << reloaded.error().describe();
        EXPECT_EQ(reloaded->size(), 1U);
    }
}

} // namespace
