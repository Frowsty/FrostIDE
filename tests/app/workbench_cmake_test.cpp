// SPDX-License-Identifier: Apache-2.0
// The application composition over a real trusted CMake project.

#include "frost/build/cmake_service.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/ui/cmake/cmake_tree_model.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "workbench_rig.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

// The service publishes on a worker; the toolbar receives its immutable model on the UI queue.
bool displayedTarget(WorkbenchRig& rig, const QString& name) {
    return waitFor(
        [&] {
            auto* combo = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarTargets"));
            const auto* service = rig.workbench->cmakeService();
            return combo && combo->currentText() == name && service && rig.workbench->cmakeTargetsCurrent() &&
                   service->state() == frost::build::BuildState::Idle;
        },
        60s);
}

TEST(WorkbenchCMake, ReloadOffersTrustBeforeConfiguring) {
    WorkDir dir("cmake-trust-guidance");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Example C)\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    auto* guidance = rig.window->findChild<QLabel*>(QStringLiteral("cmakeConfigureGuidance"));
    auto* reload = rig.window->findChild<QToolButton*>(QStringLiteral("cmakeReload"));
    ASSERT_NE(guidance, nullptr);
    ASSERT_NE(reload, nullptr);
    EXPECT_TRUE(guidance->text().contains(QStringLiteral("not trusted")));
    auto* targets = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarTargets"));
    ASSERT_NE(targets, nullptr);
    EXPECT_EQ(targets->placeholderText(), QStringLiteral("Configure CMake to select a target"));
    reload->click();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->build().state() == frost::build::BuildState::NeedsTrust; }, 30s));
    EXPECT_FALSE(rig.trust->isTrusted(dir.path()));
    rig.workbench->cmakeService()->cancel();
}

TEST(WorkbenchCMake, PersonalBuildWrapperDialogPersistsTypedArguments) {
    WorkDir dir("cmake-wrapper-settings");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Example C)\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    auto* action = rig.window->findChild<QAction*>(QStringLiteral("cmakeBuildWrapperAction"));
    ASSERT_NE(action, nullptr);
    const std::string executable = dir.join("python.exe");
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        QTimer::singleShot(2000, dialog, &QDialog::reject);
        auto* program = dialog->findChild<QLineEdit*>(QStringLiteral("cmakeWrapperExecutable"));
        auto* target = dialog->findChild<QLineEdit*>(QStringLiteral("cmakeWrapperDefaultTarget"));
        auto* args = dialog->findChild<QPlainTextEdit*>(QStringLiteral("cmakeWrapperBuildArguments"));
        ASSERT_NE(program, nullptr);
        ASSERT_NE(target, nullptr);
        ASSERT_NE(args, nullptr);
        program->setText(QString::fromStdString(executable));
        target->setText(QStringLiteral("FrostTests"));
        args->setPlainText(QStringLiteral("Scripts/Build.py\n--target\n{target}\na b;$(literal)"));
        dialog->accept();
    });
    action->trigger();
    const auto records = rig.settings->snapshot()->get(frost::settings::key::cmakeBuildWrappers);
    const auto wrappers = frost::build::cmakeBuildWrappers(records);
    ASSERT_EQ(wrappers.size(), 1u);
    EXPECT_EQ(wrappers.front().root, dir.path());
    EXPECT_EQ(wrappers.front().profile, "Debug");
    EXPECT_EQ(wrappers.front().executable, executable);
    EXPECT_EQ(wrappers.front().buildArguments.back(), "a b;$(literal)");
}

TEST(WorkbenchCMake, TargetSurvivesRefreshAndReopenAndFallsBackWhenRemoved) {
    WorkDir dir("cmake-target-session");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    const std::string prefix = "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                               "\" CACHE FILEPATH \"\" FORCE)\nproject(TargetSession C)\n";
    dir.write("CMakeLists.txt", prefix + "add_executable(first main.c)\nadd_executable(second main.c)\n"
                                         "add_library(helper STATIC main.c)\n");
    dir.write("main.c", "int main(void) { return 0; }\n");
    dir.write("CMakePresets.json", R"({"version":3,"configurePresets":[
        {"name":"Debug","generator":"Ninja","binaryDir":"${sourceDir}/build-debug"},
        {"name":"Release","generator":"Ninja","binaryDir":"${sourceDir}/build-release"}]})");
    {
        WorkbenchRig rig(dir);
        ASSERT_TRUE(rig.trust->trust(dir.path()));
        ASSERT_TRUE(rig.workbench->openPath(dir.path()));
        ASSERT_TRUE(displayedTarget(rig, QStringLiteral("first")));
        auto* combo = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarTargets"));
        ASSERT_NE(combo, nullptr);
        combo->setCurrentIndex(combo->findText(QStringLiteral("second")));
        emit combo->activated(combo->currentIndex());
        ASSERT_EQ(combo->currentText(), QStringLiteral("second"));
        auto* arguments = rig.window->findChild<QLineEdit*>(QStringLiteral("cmakeRunArguments"));
        ASSERT_NE(arguments, nullptr);
        ASSERT_TRUE(arguments->isEnabled());
        arguments->setText(QStringLiteral("-ts=Core -tc=HMAC-SHA256*"));
        auto* service = rig.workbench->cmakeService();
        ASSERT_TRUE(service->configure());
        ASSERT_TRUE(displayedTarget(rig, QStringLiteral("second")));
        ASSERT_TRUE(service->setActiveProfile(1));
        ASSERT_TRUE(displayedTarget(rig, QStringLiteral("first")));
        EXPECT_TRUE(arguments->text().isEmpty());
        arguments->setText(QStringLiteral("--release"));
        ASSERT_TRUE(service->setActiveProfile(0));
        ASSERT_TRUE(displayedTarget(rig, QStringLiteral("second")));
        EXPECT_EQ(arguments->text(), QStringLiteral("-ts=Core -tc=HMAC-SHA256*"));
        // The service switches synchronously, while its new model is still queued for the UI.
        // Selecting the old tree during that gap must not overwrite Release's preference.
        auto* tree = rig.window->findChild<QTreeView*>(QStringLiteral("cmakeTargets"));
        ASSERT_NE(tree, nullptr);
        const auto findTarget = [&](auto&& self, const QModelIndex& parent, const QString& name) -> QModelIndex {
            for (int row = 0; row < tree->model()->rowCount(parent); ++row) {
                const QModelIndex index = tree->model()->index(row, 0, parent);
                if (index.data().toString() == name && index.data(frost::ui::cmake::CMakeTreeModel::KindRole).toInt() ==
                                                           static_cast<int>(frost::project::CMakeViewNodeKind::Target))
                    return index;
                if (const QModelIndex found = self(self, index, name); found.isValid())
                    return found;
            }
            return {};
        };
        const QModelIndex first = findTarget(findTarget, {}, QStringLiteral("first"));
        const QModelIndex second = findTarget(findTarget, {}, QStringLiteral("second"));
        const QModelIndex helper = findTarget(findTarget, {}, QStringLiteral("helper"));
        ASSERT_TRUE(first.isValid());
        ASSERT_TRUE(second.isValid());
        ASSERT_TRUE(helper.isValid());
        tree->setCurrentIndex(helper);
        EXPECT_EQ(tree->currentIndex(), helper);
        EXPECT_EQ(combo->currentText(), QStringLiteral("second"));
        ASSERT_TRUE(service->setActiveProfile(1));
        tree->setCurrentIndex(first);
        tree->setCurrentIndex(second);
        ASSERT_TRUE(displayedTarget(rig, QStringLiteral("first")));
        ASSERT_TRUE(service->setActiveProfile(0));
        ASSERT_TRUE(displayedTarget(rig, QStringLiteral("second")));
        ASSERT_TRUE(rig.workbench->closeProject());
    }
    {
        WorkbenchRig rig(dir);
        ASSERT_TRUE(rig.workbench->openPath(dir.path()));
        ASSERT_TRUE(displayedTarget(rig, QStringLiteral("second")));
        auto* arguments = rig.window->findChild<QLineEdit*>(QStringLiteral("cmakeRunArguments"));
        ASSERT_NE(arguments, nullptr);
        EXPECT_EQ(arguments->text(), QStringLiteral("-ts=Core -tc=HMAC-SHA256*"));
        dir.write("CMakeLists.txt", prefix + "add_executable(first main.c)\n");
        ASSERT_TRUE(rig.workbench->cmakeService()->configure());
        ASSERT_TRUE(displayedTarget(rig, QStringLiteral("first")));
        auto* combo = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarTargets"));
        ASSERT_NE(combo, nullptr);
        emit combo->activated(combo->currentIndex()); // Explicitly accept the already-selected fallback.
        dir.write("CMakeLists.txt", prefix + "add_executable(first main.c)\nadd_executable(second main.c)\n");
        ASSERT_TRUE(rig.workbench->cmakeService()->configure());
        ASSERT_TRUE(displayedTarget(rig, QStringLiteral("first")));
        EXPECT_TRUE(rig.workbench->closeProject());
    }
}

TEST(WorkbenchCMake, RunPassesSelectedTargetArgumentsWithoutAShell) {
    WorkDir dir("cmake-run-arguments");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                  "\" CACHE FILEPATH \"\" FORCE)\nproject(Arguments C)\nadd_executable(args main.c)\n");
    dir.write("main.c", "#include <stdio.h>\nint main(int argc, char** argv) { "
                        "for (int i=1;i<argc;++i) printf(\"ARG[%s]\\n\",argv[i]); return 0; }\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(displayedTarget(rig, QStringLiteral("args")));
    auto* arguments = rig.window->findChild<QLineEdit*>(QStringLiteral("cmakeRunArguments"));
    ASSERT_NE(arguments, nullptr);
    arguments->setText(QStringLiteral("--first \"two words\""));
    ASSERT_TRUE(rig.workbench->runController()->run());
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runController()->running(); }, 240s));
    EXPECT_TRUE(rig.workbench->runConsole()->text().contains(QStringLiteral("ARG[--first]")));
    EXPECT_TRUE(rig.workbench->runConsole()->text().contains(QStringLiteral("ARG[two words]")));
}

TEST(WorkbenchCMake, TrustConfigureBuildDiagnoseAndRun) {
    WorkDir dir("cmake-app");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n"
                                "set(CMAKE_C_COMPILER \"" +
                                    compiler +
                                    "\" CACHE FILEPATH \"\" FORCE)\n"
                                    "project(Integration C)\nadd_executable(hello main.c)\n"
                                    "add_executable(other other.c)\n");
    dir.write("main.c", "#include <stdio.h>\nint main(void) { puts(\"cmake works\"); return 0; }\n");
    dir.write("other.c", "int main(void) { return 0; }\n");
    dir.write("CMakePresets.json", R"({"version":3,"configurePresets":[
        {"name":"Debug","generator":"Ninja","binaryDir":"${sourceDir}/cmake-build-debug"},
        {"name":"Release","generator":"Ninja","binaryDir":"${sourceDir}/cmake-build-release"}]})");
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    EXPECT_FALSE(rig.trust->isTrusted(dir.path()));
    auto* guidance = rig.window->findChild<QLabel*>(QStringLiteral("cmakeConfigureGuidance"));
    ASSERT_NE(guidance, nullptr);
    EXPECT_TRUE(guidance->text().contains(QStringLiteral("not trusted")));
    EXPECT_TRUE(guidance->text().contains(QStringLiteral("Reload CMake Project")));
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
            // CPP-3: the IDE's own directory for the configuration, holding its database.
            return location && location->origin == dir.join("cmake-build-debug") &&
                   location->directory != location->origin;
        },
        60s));
    auto* tree = rig.window->findChild<QTreeView*>(QStringLiteral("cmakeTargets"));
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(waitFor([&] { return tree->model()->rowCount(tree->model()->index(0, 0)) > 0; }, 30s));
    EXPECT_TRUE(guidance->text().isEmpty());
    auto* targetLabel = rig.window->findChild<QLabel*>(QStringLiteral("cmakeRunTarget"));
    ASSERT_NE(targetLabel, nullptr);
    auto* toolbarTargets = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarTargets"));
    auto* toolbarProfiles = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeToolbarProfiles"));
    ASSERT_NE(toolbarTargets, nullptr);
    ASSERT_NE(toolbarProfiles, nullptr);
    ASSERT_TRUE(waitFor([&] { return toolbarTargets->count() == 2 && toolbarProfiles->count() == 2; }, 30s));
    // RUN-3: the toolbar's run configuration combo (one per executable target) replaces the
    // CMake target combo, which stays hidden and in step.
    // (The toolbar's own actions say it: its layout may still fold a widget that does not fit.)
    const auto onToolbar = [&](QWidget* widget) {
        auto& toolbar = rig.window->mainToolbar();
        for (QAction* action : toolbar.actions())
            if (toolbar.widgetForAction(action) == widget)
                return action->isVisible();
        return false;
    };
    EXPECT_TRUE(onToolbar(rig.window->mainToolbar().runConfigurations()));
    EXPECT_FALSE(onToolbar(toolbarTargets));
    const auto findTarget = [&](auto&& self, const QModelIndex& parent, const QString& name) -> QModelIndex {
        for (int row = 0; row < tree->model()->rowCount(parent); ++row) {
            const QModelIndex index = tree->model()->index(row, 0, parent);
            const QVariant target = index.data(frost::ui::cmake::CMakeTreeModel::TargetIndexRole);
            if (target.isValid() && target.toInt() >= 0 && index.data().toString() == name)
                return index;
            if (const QModelIndex found = self(self, index, name); found.isValid())
                return found;
        }
        return {};
    };
    const QModelIndex other = findTarget(findTarget, {}, QStringLiteral("other"));
    ASSERT_TRUE(other.isValid());
    toolbarTargets->setCurrentIndex(toolbarTargets->findText(QStringLiteral("other")));
    emit toolbarTargets->activated(toolbarTargets->currentIndex());
    EXPECT_EQ(targetLabel->text(), QStringLiteral("Run/Debug: other"));
    EXPECT_EQ(tree->currentIndex(), other);
    tree->setCurrentIndex(findTarget(findTarget, {}, QStringLiteral("hello")));
    EXPECT_EQ(targetLabel->text(), QStringLiteral("Run/Debug: hello"));
    EXPECT_EQ(toolbarTargets->currentText(), QStringLiteral("hello"));
    const auto findSource = [&](auto&& self, const QModelIndex& parent) -> QModelIndex {
        for (int row = 0; row < tree->model()->rowCount(parent); ++row) {
            const QModelIndex index = tree->model()->index(row, 0, parent);
            if (index.data(frost::ui::cmake::CMakeTreeModel::PathRole).toString().endsWith(QStringLiteral("main.c")))
                return index;
            if (const QModelIndex found = self(self, index); found.isValid())
                return found;
        }
        return {};
    };
    const QModelIndex source = findSource(findSource, {});
    ASSERT_TRUE(source.isValid());
    tree->setCurrentIndex(source);
    EXPECT_EQ(targetLabel->text(), QStringLiteral("Run/Debug: hello"));

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
    toolbarProfiles->setCurrentIndex(1);
    emit toolbarProfiles->activated(1);
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runController()->running(); }, 30s));
    EXPECT_FALSE(rig.workbench->runConsole()->text().contains(QStringLiteral("cmake works")));
    // Switching profile configures its own build tree and clangd follows its database.
    auto* profiles = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeProfiles"));
    ASSERT_NE(profiles, nullptr);
    ASSERT_TRUE(waitFor([&] { return profiles->count() == 2; }, 30s));
    ASSERT_TRUE(waitFor([&] { return profiles->currentIndex() == 1 && toolbarProfiles->currentIndex() == 1; }, 30s));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto* clangd = rig.workbench->clangd();
            const auto location = clangd ? clangd->compileCommands() : std::nullopt;
            return location && location->origin == dir.join("cmake-build-release") &&
                   location->directory != location->origin;
        },
        120s))
        << "profile=" << profiles->currentText().toStdString()
        << " banner=" << rig.window->findChild<QLabel*>(QStringLiteral("cmakeErrorBanner"))->text().toStdString()
        << " output="
        << rig.window->findChild<QPlainTextEdit*>(QStringLiteral("cmakeOutput"))->toPlainText().toStdString();
    ASSERT_TRUE(waitFor(
        [&] {
            const auto model = service->model();
            if (!model)
                return false;
            const auto first = std::ranges::find_if(model->targets, [](const auto& target) {
                return target.type == frost::project::CMakeTargetType::Executable;
            });
            return first != model->targets.end() &&
                   targetLabel->text() == QStringLiteral("Run/Debug: %1").arg(QString::fromStdString(first->name));
        },
        30s));
    EXPECT_TRUE(rig.workbench->closeProject());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    profiles = rig.window->findChild<QComboBox*>(QStringLiteral("cmakeProfiles"));
    ASSERT_NE(profiles, nullptr);
    ASSERT_TRUE(waitFor([&] { return profiles->count() == 2 && profiles->currentIndex() == 1; }, 30s));
    service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->state() == frost::build::BuildState::Idle; }, 60s));
    ASSERT_TRUE(displayedTarget(rig, QStringLiteral("hello")));
    service->setSaveAllHook([](frost::build::SaveAllCompletion done) { done({}); });
    const auto reopenedRun = rig.workbench->runController()->run();
    ASSERT_TRUE(reopenedRun.has_value()) << reopenedRun.error().describe();
    ASSERT_TRUE(service->waitForIdle(60s)); // The Run completion is still queued on the GUI thread.
    EXPECT_FALSE(rig.workbench->buildService().reserveExternalLaunch());
    rig.workbench->shutdown();
    auto afterShutdown = rig.workbench->buildService().reserveExternalLaunch();
    ASSERT_TRUE(afterShutdown);
    rig.workbench->buildService().releaseExternalLaunch(*afterShutdown);
    QCoreApplication::processEvents();
    EXPECT_FALSE(rig.workbench->runController()->running());
    EXPECT_FALSE(rig.workbench->runConsole()->text().contains(QStringLiteral("cmake works")));
    auto refused = rig.workbench->runController()->run();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Cancelled);
}

TEST(WorkbenchCMake, DebugUsesConfiguredWrapperAndRetainedTarget) {
    auto& fs = frost::io::realFileSystem();
    const auto lldb = frost::debug::findLldbDap(fs, frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(lldb.has_value()) << "the pinned lldb-dap is required (FND-6): " << lldb.error().describe();
    WorkDir dir("cmake-wrapper-debug");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                  "\" CACHE FILEPATH \"\" FORCE)\nset(CMAKE_BUILD_TYPE Debug CACHE STRING \"\" FORCE)\n"
                  "project(WrapperDebug C)\nadd_executable(debuggee main.c)\n");
    dir.write("main.c", "int main(void) {\n  int value = 41;\n  return value == 41 ? 0 : 1;\n}\n");
    RigOptions options;
    options.workbench.debugAdapterFactory =
        frost::debug::lldbDapAdapterFactory(frost::io::realProcessLauncher(), *lldb);
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(displayedTarget(rig, QStringLiteral("debuggee")));
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->build(frost::build::BuildAction::Build));
    ASSERT_TRUE(waitFor(
        [&] { return service->state() == frost::build::BuildState::Idle && service->lastOutcome().has_value(); }, 60s));
    ASSERT_TRUE(service->lastOutcome());
    ASSERT_TRUE(service->lastOutcome()->succeeded());
    const auto profile = service->activeProfile();
    ASSERT_TRUE(profile);
    frost::build::CMakeBuildWrapper wrapper{.root = dir.path(),
                                            .profile = profile->name,
                                            .executable = FROST_TEST_CMAKE,
                                            .buildArguments = {"-E", "echo", "WRAPPER_DEBUG_TARGET", "{target}"}};
    ASSERT_TRUE(service->setBuildWrappers({wrapper}));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.c")));
    auto* debug = rig.workbench->debugController();
    ASSERT_NE(debug, nullptr);
    debug->toggleBreakpoint(dir.join("main.c"), 2);
    ASSERT_EQ(rig.window->invoke("run.debug"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return debug->paused() && debug->selectedFrame().has_value(); }, 30s))
        << "build output: " << rig.workbench->buildOutput()->editor()->toPlainText().toStdString()
        << " run output: " << rig.workbench->runConsole()->text().toStdString();
    ASSERT_NE(rig.workbench->buildOutput(), nullptr);
    EXPECT_TRUE(rig.workbench->buildOutput()->editor()->toPlainText().contains("WRAPPER_DEBUG_TARGET debuggee"));
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, 60s));
}

TEST(WorkbenchCMake, DebugBuildsStopsInspectsStepsAndStops) {
    auto& fs = frost::io::realFileSystem();
    const auto lldb = frost::debug::findLldbDap(fs, frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(lldb.has_value()) << "the pinned lldb-dap is required (FND-6): " << lldb.error().describe();
    WorkDir dir("cmake-debug-app");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                  "\" CACHE FILEPATH \"\" FORCE)\nset(CMAKE_BUILD_TYPE Debug CACHE STRING \"\" FORCE)\n"
                  "project(DebugApp C)\nadd_executable(debuggee main.c)\n");
    dir.write("main.c", "int main(int argc, char** argv) {\n  int value = 41;\n  value += 1;\n  return value == 42 && "
                        "argc > 0 && argv ? 0 : 1;\n}\n");
    int adaptersStarted = 0;
    auto factory = frost::debug::lldbDapAdapterFactory(frost::io::realProcessLauncher(), *lldb);
    RigOptions options;
    options.workbench.debugAdapterFactory = [&](frost::debug::DapHandlers handlers) {
        ++adaptersStarted;
        return factory(std::move(handlers));
    };
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->model() && service->state() == frost::build::BuildState::Idle; }, 240s));
    ASSERT_TRUE(displayedTarget(rig, QStringLiteral("debuggee")));
    auto* debug = rig.workbench->debugController();
    ASSERT_NE(debug, nullptr);
    // Before the Debug tool window exists, the Run console reruns only a normal process.
    emit rig.workbench->runConsole()->rerunRequested();
    ASSERT_TRUE(rig.workbench->runController()->running());
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->runController()->running(); }, 240s));
    EXPECT_FALSE(debug->debugging());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.c")).has_value());
    auto* arguments = rig.window->findChild<QLineEdit*>(QStringLiteral("cmakeRunArguments"));
    ASSERT_NE(arguments, nullptr);
    arguments->setText(QStringLiteral("--first \"two words\""));
    ASSERT_TRUE(rig.view()->editor().typeText("// saved before debug\n"));
    debug->toggleBreakpoint(dir.join("main.c"), 3);
    ASSERT_EQ(rig.window->invoke("run.debug"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return debug->paused() && debug->selectedFrame().has_value(); }, 240s));
    ASSERT_EQ(debug->selectedFrame()->line, 4);
    EXPECT_TRUE(dir.read("main.c").starts_with("// saved before debug\n"));
    ASSERT_TRUE(rig.view());
    EXPECT_EQ(rig.view()->path(), dir.join("main.c"));
    auto value = std::make_shared<std::optional<frost::Result<frost::debug::dap::Variable>>>();
    debug->evaluate(QStringLiteral("value"), QStringLiteral("watch"),
                    [value](auto result) { *value = std::move(result); });
    ASSERT_TRUE(waitFor([&] { return value->has_value(); }, 60s));
    ASSERT_TRUE(value->value().has_value());
    EXPECT_EQ(value->value()->value, "41");
    auto argc = std::make_shared<std::optional<frost::Result<frost::debug::dap::Variable>>>();
    debug->evaluate(QStringLiteral("argc"), QStringLiteral("watch"),
                    [argc](auto result) { *argc = std::move(result); });
    ASSERT_TRUE(waitFor([&] { return argc->has_value(); }, 60s));
    ASSERT_TRUE(argc->value().has_value());
    EXPECT_EQ(argc->value()->value, "3");
    ASSERT_TRUE(debug->stepOver());
    ASSERT_TRUE(
        waitFor([&] { return debug->paused() && debug->selectedFrame() && debug->selectedFrame()->line == 5; }, 60s));
    EXPECT_FALSE(rig.workbench->closeProject());
    ASSERT_TRUE(rig.view()->editor().typeText("// unsaved change\n"));
    ASSERT_TRUE(fs.removeFile(dir.join("main.c")).has_value());
    ASSERT_TRUE(fs.createDirectories(dir.join("main.c/blocked")).has_value());
    bool launchedDuringPrompt = false;
    bool pendingDuringPrompt = true;
    rig.prompts.discard = true;
    rig.prompts.onDiscard = [&] {
        QDeadlineTimer timer(250); // The rerun timer is 50 ms; exercise a nested prompt loop.
        while (!timer.hasExpired()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            launchedDuringPrompt = launchedDuringPrompt || debug->debugging() || adaptersStarted != 1;
        }
        pendingDuringPrompt = debug->rerunPending();
    };
    WorkDir other("cmake-debug-switch");
    bool switched = false;
    bool wasPending = false;
    bool pendingAfterSwitch = true;
    QObject::connect(debug, &frost::ui::DebugController::sessionEnded, [&] {
        wasPending = debug->rerunPending();
        switched = rig.workbench->closeProject() && rig.workbench->openPath(other.path()).has_value();
        pendingAfterSwitch = debug->rerunPending();
    });
    auto* debugWindow = rig.window->findChild<frost::ui::DebugToolWindow*>();
    ASSERT_NE(debugWindow, nullptr);
    emit debugWindow->console()->rerunRequested();
    ASSERT_TRUE(waitFor([&] { return switched; }, 60s));
    EXPECT_TRUE(wasPending);
    EXPECT_EQ(rig.prompts.discardCalls, 1);
    EXPECT_FALSE(launchedDuringPrompt);
    EXPECT_FALSE(pendingDuringPrompt);
    EXPECT_EQ(adaptersStarted, 1);
    EXPECT_FALSE(pendingAfterSwitch);
    QDeadlineTimer timer(250);
    while (!timer.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_FALSE(debug->debugging());
    EXPECT_FALSE(debug->rerunPending());
}

TEST(WorkbenchCMake, DebugRejectsCompletionAfterProfileSwitch) {
    WorkDir dir("cmake-debug-profile");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                  "\" CACHE FILEPATH \"\" FORCE)\nproject(Profile C)\nadd_executable(debuggee main.c)\n");
    dir.write("main.c", "int main(void) { return 0; }\n");
    dir.write("CMakePresets.json", R"({"version":3,"configurePresets":[
        {"name":"Debug","generator":"Ninja","binaryDir":"${sourceDir}/build-one"},
        {"name":"Release","generator":"Ninja","binaryDir":"${sourceDir}/build-two"}]})");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->model() && service->state() == frost::build::BuildState::Idle; }, 240s));
    ASSERT_TRUE(displayedTarget(rig, QStringLiteral("debuggee")));
    service->setSaveAllHook([](frost::build::SaveAllCompletion done) { done({}); });
    auto* debug = rig.workbench->debugController();
    ASSERT_TRUE(debug->debug().has_value());
    ASSERT_TRUE(service->waitForIdle(120s)); // Keep the completion queued on the GUI thread.
    ASSERT_TRUE(service->setActiveProfile(1).has_value());
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, 60s));
    EXPECT_EQ(debug->session(), nullptr);
}

TEST(WorkbenchCMake, ShutdownWhileDebugBuildWaitsForSaveDoesNotLaunch) {
    WorkDir dir("cmake-debug-shutdown");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                  "\" CACHE FILEPATH \"\" FORCE)\nproject(Shutdown C)\nadd_executable(debuggee main.c)\n");
    dir.write("main.c", "int main(void) { return 0; }\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->model() && service->state() == frost::build::BuildState::Idle; }, 240s));
    ASSERT_TRUE(displayedTarget(rig, QStringLiteral("debuggee")));
    std::mutex mutex;
    frost::build::SaveAllCompletion held;
    service->setSaveAllHook([&](frost::build::SaveAllCompletion done) {
        std::lock_guard guard(mutex);
        held = std::move(done);
    });
    auto* debug = rig.workbench->debugController();
    ASSERT_TRUE(debug->debug().has_value());
    ASSERT_TRUE(waitFor(
        [&] {
            std::lock_guard guard(mutex);
            return static_cast<bool>(held);
        },
        30s));
    EXPECT_FALSE(rig.workbench->buildService().reserveExternalLaunch());
    EXPECT_FALSE(rig.workbench->closeProject());
    rig.workbench->shutdown();
    auto admission = rig.workbench->buildService().reserveExternalLaunch();
    ASSERT_TRUE(admission);
    rig.workbench->buildService().releaseExternalLaunch(*admission);
    {
        std::lock_guard guard(mutex);
        held({});
    }
    QCoreApplication::processEvents();
    EXPECT_FALSE(debug->debugging());
    EXPECT_EQ(debug->session(), nullptr);
    EXPECT_EQ(debug->debug().error().code(), frost::ErrorCode::Cancelled);
}

TEST(WorkbenchCMake, ShutdownStopsARunningTarget) {
    WorkDir dir("cmake-shutdown");
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                                    "\" CACHE FILEPATH \"\" FORCE)\nproject(Stop C)\nadd_executable(stop main.c)\n");
    dir.write("main.c", "#include <stdio.h>\nint main(void) { puts(\"shutdown-target-ready\"); fflush(stdout); "
                        "getchar(); }\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->model() && service->state() == frost::build::BuildState::Idle; }, 240s));
    ASSERT_TRUE(displayedTarget(rig, QStringLiteral("stop")));
    ASSERT_TRUE(rig.workbench->runController()->run().has_value());
    EXPECT_FALSE(rig.workbench->buildService().reserveExternalLaunch()); // Pending build and handoff.
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->runConsole()->text().contains(QStringLiteral("shutdown-target-ready")) ||
                   !rig.workbench->runController()->running();
        },
        240s));
    ASSERT_TRUE(rig.workbench->runConsole()->text().contains(QStringLiteral("shutdown-target-ready")))
        << "run=" << rig.workbench->runConsole()->text().toStdString()
        << " build=" << rig.workbench->buildOutput()->editor()->toPlainText().toStdString();
    EXPECT_FALSE(rig.workbench->buildService().reserveExternalLaunch()); // Live target process.
    ASSERT_TRUE(rig.workbench->runController()->running());
    // RUN-1: the target's program runs through the workbench's RunManager.
    auto* runs = rig.workbench->runManager();
    ASSERT_NE(runs, nullptr);
    const auto active = runs->activeRuns();
    ASSERT_EQ(active.size(), 1U);
    EXPECT_EQ(active.front().configuration.name, "stop");
    EXPECT_EQ(active.front().configuration.type, frost::run::RunType::CMakeTarget);
    EXPECT_EQ(active.front().phase, frost::run::RunPhase::Running);
    EXPECT_NE(active.front().processId, 0U);
    rig.workbench->shutdown();
    EXPECT_FALSE(rig.workbench->runController()->running());
    EXPECT_TRUE(runs->waitForIdle(60s)) << "Stop ended the target's process tree";
    EXPECT_TRUE(runs->activeRuns().empty());
    auto admission = rig.workbench->buildService().reserveExternalLaunch();
    ASSERT_TRUE(admission);
    rig.workbench->buildService().releaseExternalLaunch(*admission);
    auto refused = rig.workbench->runController()->run();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Cancelled);
}
} // namespace
