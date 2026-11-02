// SPDX-License-Identifier: Apache-2.0
// The application composition over a real trusted CMake project.

#include "frost/build/cmake_service.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "workbench_rig.hpp"
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTreeView>

#include <algorithm>
#include <chrono>
#include <string>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

TEST(WorkbenchCMake, TrustConfigureBuildDiagnoseAndRun) {
    WorkDir dir("cmake-app");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n"
                                "set(CMAKE_C_COMPILER \"" +
                                    compiler +
                                    "\" CACHE FILEPATH \"\" FORCE)\n"
                                    "project(Integration C)\nadd_executable(hello main.c)\n");
    dir.write("main.c", "#include <stdio.h>\nint main(void) { puts(\"cmake works\"); return 0; }\n");
    dir.write("CMakePresets.json", R"({"version":3,"configurePresets":[
        {"name":"Debug","generator":"Ninja","binaryDir":"${sourceDir}/cmake-build-debug"},
        {"name":"Release","generator":"Ninja","binaryDir":"${sourceDir}/cmake-build-release"}]})");
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    EXPECT_FALSE(rig.trust->isTrusted(dir.path()));
    EXPECT_FALSE(frost::io::realFileSystem().exists(dir.join("cmake-build-debug/CMakeCache.txt")).value_or(false));
    ASSERT_TRUE(rig.workbench->build().build(frost::build::BuildAction::Build).has_value());
    ASSERT_TRUE(waitFor([&] { return rig.workbench->build().state() == frost::build::BuildState::NeedsTrust; }, 30s));
    ASSERT_TRUE(rig.workbench->build().trust(dir.path()).has_value());
    ASSERT_TRUE(waitFor([&] { return rig.workbench->build().lastOutcome().has_value(); }, 240s));
    ASSERT_TRUE(rig.workbench->build().lastOutcome()->succeeded());
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }, 30s));
    EXPECT_TRUE(
        frost::io::realFileSystem().exists(dir.join("cmake-build-debug/compile_commands.json")).value_or(false));
    ASSERT_NE(rig.workbench->clangd(), nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            const auto location = rig.workbench->clangd()->compileCommands();
            return location && location->directory == dir.join("cmake-build-debug");
        },
        60s));
    auto* tree = rig.window->findChild<QTreeView*>(QStringLiteral("cmakeTargets"));
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(waitFor([&] { return tree->model()->rowCount(tree->model()->index(0, 0)) > 0; }, 30s));

    // Keep a successful ordinary build's GUI completion queued, then start a failing Run build.
    // The old completion must not launch its already-built executable or end the pending Run.
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    service->setSaveAllHook([](frost::build::SaveAllCompletion done) { done({}); });
    ASSERT_TRUE(service->build(frost::build::BuildAction::Build).has_value());
    ASSERT_TRUE(service->waitForIdle(60s)); // No Qt events are processed here.
    dir.write("main.c", "int main(void) { return missing_symbol; }\n");
    ASSERT_TRUE(rig.workbench->runController()->run().has_value());
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runController()->running(); }, 240s));
    EXPECT_FALSE(rig.workbench->runConsole()->text().contains(QStringLiteral("cmake works")));
    EXPECT_TRUE(rig.workbench->runConsole()->text().contains(QStringLiteral("Build failed")));
    // A compiler error appears in Problems and can be opened by the normal diagnostic path.
    ASSERT_TRUE(waitFor(
        [&] {
            const auto& outcome = rig.workbench->build().lastOutcome();
            return outcome && !outcome->succeeded();
        },
        240s))
        << "state=" << static_cast<int>(rig.workbench->build().state())
        << " output=" << rig.workbench->buildOutput()->editor()->toPlainText().toStdString();
    ASSERT_FALSE(rig.workbench->problems()->model()->problems().empty());
    const auto& problem = rig.workbench->problems()->model()->problems().front();
    EXPECT_TRUE(problem.diagnostic.jumpable);
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }, 30s));
    ASSERT_TRUE(rig.workbench->problems()->activate(rig.workbench->problems()->view()->model()->index(0, 0)));
    ASSERT_TRUE(waitFor([&] { return rig.view() != nullptr; }));
    EXPECT_EQ(rig.view()->path(), dir.join("main.c"));

    dir.write("main.c", "#include <stdio.h>\nint main(void) { puts(\"cmake works\"); fflush(stdout); "
                        "getchar(); return 0; }\n");
    ASSERT_EQ(rig.window->invoke("run.run"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(
        waitFor([&] { return rig.workbench->runConsole()->text().contains(QStringLiteral("cmake works")); }, 240s));
    EXPECT_TRUE(rig.workbench->runController()->running());
    EXPECT_FALSE(rig.workbench->closeProject());
    auto* input = rig.workbench->runConsole()->input();
    ASSERT_TRUE(input->isEnabled());
    input->setText(QStringLiteral("continue"));
    emit input->returnPressed();
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runController()->running(); }, 30s));

    // Finish a Run's build without delivering its callback, then change profiles. The completed
    // build's old-profile artifact must not launch after the new profile becomes active.
    ASSERT_TRUE(rig.workbench->runController()->run().has_value());
    ASSERT_TRUE(service->waitForIdle(60s));
    ASSERT_TRUE(service->setActiveProfile(1).has_value());
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runController()->running(); }, 30s));
    EXPECT_FALSE(rig.workbench->runConsole()->text().contains(QStringLiteral("cmake works")));
    // Switching profile configures its own build tree and clangd follows its database.
    auto* profiles = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeProfiles"));
    ASSERT_NE(profiles, nullptr);
    ASSERT_TRUE(waitFor([&] { return profiles->count() == 2; }, 30s));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto* clangd = rig.workbench->clangd();
            const auto location = clangd ? clangd->compileCommands() : std::nullopt;
            return location && location->directory == dir.join("cmake-build-release");
        },
        120s))
        << "profile=" << profiles->currentText().toStdString()
        << " banner=" << rig.window->findChild<QLabel*>(QStringLiteral("cmakeErrorBanner"))->text().toStdString()
        << " output="
        << rig.window->findChild<QPlainTextEdit*>(QStringLiteral("cmakeOutput"))->toPlainText().toStdString();
    EXPECT_TRUE(rig.workbench->closeProject());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    profiles = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeProfiles"));
    ASSERT_NE(profiles, nullptr);
    ASSERT_TRUE(waitFor([&] { return profiles->count() == 2 && profiles->currentIndex() == 1; }, 30s));
    service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->state() == frost::build::BuildState::Idle; }, 60s));
    service->setSaveAllHook([](frost::build::SaveAllCompletion done) { done({}); });
    ASSERT_TRUE(rig.workbench->runController()->run().has_value());
    ASSERT_TRUE(service->waitForIdle(60s)); // The Run completion is still queued on the GUI thread.
    rig.workbench->shutdown();
    QCoreApplication::processEvents();
    EXPECT_FALSE(rig.workbench->runController()->running());
    EXPECT_FALSE(rig.workbench->runConsole()->text().contains(QStringLiteral("cmake works")));
    auto refused = rig.workbench->runController()->run();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Cancelled);
}

TEST(WorkbenchCMake, ShutdownStopsARunningTarget) {
    WorkDir dir("cmake-shutdown");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                                    "\" CACHE FILEPATH \"\" FORCE)\nproject(Stop C)\nadd_executable(stop main.c)\n");
    dir.write("main.c", "#include <stdio.h>\nint main(void) { puts(\"started\"); fflush(stdout); getchar(); }\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->model() && service->state() == frost::build::BuildState::Idle; }, 240s));
    ASSERT_TRUE(rig.workbench->runController()->run().has_value());
    ASSERT_TRUE(waitFor([&] { return rig.workbench->runConsole()->text().contains(QStringLiteral("started")); }, 240s));
    ASSERT_TRUE(rig.workbench->runController()->running());
    rig.workbench->shutdown();
    EXPECT_FALSE(rig.workbench->runController()->running());
    auto refused = rig.workbench->runController()->run();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Cancelled);
}
} // namespace
