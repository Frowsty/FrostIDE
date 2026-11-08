// SPDX-License-Identifier: Apache-2.0
//
// Offscreen tests of frost::ui_cmake (ADR 0046): CMakeTreeModel over a CMake view model (rows,
// parents, roles, keys, reset), CMakeController and CMakeToolWindow driven by service events from
// another thread with a fake backend (profiles, output, the error banner, Reload, Stop, a refused
// profile switch, events after the controller is gone, the progress row and the Presets tab), and
// the real CMakeService behind CMakeServiceBackend over a MemFs folder with a recorded File API
// reply (untrusted, so nothing runs).

#include "frost/build/cmake_service.hpp"
#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/project/cmake_view_model.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/cmake/cmake_controller.hpp"
#include "frost/ui/cmake/cmake_tool_window.hpp"
#include "frost/ui/cmake/cmake_tree_model.hpp"

#include "project/cmake_reply_fixture.hpp"
#include <QApplication>
#include <QComboBox>
#include <QFont>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace frost::ui::cmake {
namespace {

using namespace std::chrono_literals;

constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;

std::string root() { return kWindows ? std::string("C:\\w\\tiny") : std::string("/w/tiny"); }
std::string at(std::string_view relative) {
    std::string out(relative);
    if constexpr (kWindows) {
        std::ranges::replace(out, '/', '\\');
    }
    return io::joinPath(root(), out).value();
}

bool pumpUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A small project: a library with a source, a header and a generated source, and an executable.
std::shared_ptr<const project::CMakeProject> sampleProject() {
    auto project = std::make_shared<project::CMakeProject>();
    project->name = "Tiny";
    project->sourceDirectory = root();
    project->buildDirectory = at("build");
    project::CMakeTarget core;
    core.name = "core";
    core.type = project::CMakeTargetType::StaticLibrary;
    core.sources = {project::CMakeSource{.path = at("src/core.c")}, project::CMakeSource{.path = at("include/core.h")},
                    project::CMakeSource{.path = at("build/gen/version.c"), .generated = true}};
    project::CMakeTarget app;
    app.name = "app";
    app.type = project::CMakeTargetType::Executable;
    app.sources = {project::CMakeSource{.path = at("app/main.c")}};
    project->targets = {core, app};
    project->inputs = {project::CMakeInputFile{.path = at("CMakeLists.txt")}};
    return project;
}

QStringList childLabels(const QAbstractItemModel& model, const QModelIndex& parent) {
    QStringList labels;
    for (int row = 0; row < model.rowCount(parent); ++row) {
        labels.push_back(model.index(row, 0, parent).data().toString());
    }
    return labels;
}

TEST(CMakeTreeModel, ShowsTheViewModel) {
    CMakeTreeModel model;
    EXPECT_EQ(model.rowCount(), 0);
    auto view = std::make_shared<const project::CMakeViewModel>(project::CMakeViewModel::build(*sampleProject()));
    model.setViewModel(view);
    ASSERT_EQ(model.rowCount(), 1);
    const QModelIndex project = model.index(0, 0);
    EXPECT_EQ(project.data().toString(), QStringLiteral("Tiny"));
    EXPECT_FALSE(model.parent(project).isValid());
    EXPECT_EQ(childLabels(model, project), (QStringList{"app", "core", "CMake"}));

    const QModelIndex core = model.indexForKey("target:core");
    ASSERT_TRUE(core.isValid());
    EXPECT_EQ(core.row(), 1);
    EXPECT_EQ(model.parent(core), project);
    EXPECT_EQ(core.data(CMakeTreeModel::KindRole).toInt(), static_cast<int>(project::CMakeViewNodeKind::Target));
    EXPECT_EQ(core.data(CMakeTreeModel::TargetIndexRole).toInt(), 0);
    EXPECT_EQ(childLabels(model, core), (QStringList{"include", "src", "Generated"}));
    const QModelIndex generated = model.index(0, 0, model.index(2, 0, core));
    EXPECT_EQ(generated.data().toString(), QStringLiteral("version.c"));
    EXPECT_TRUE(generated.data(CMakeTreeModel::GeneratedRole).toBool());
    EXPECT_TRUE(generated.data(Qt::FontRole).value<QFont>().italic());
    EXPECT_TRUE(generated.data(Qt::ToolTipRole).toString().contains(QStringLiteral("generated")));
    const QModelIndex source = model.index(0, 0, model.index(1, 0, core));
    EXPECT_EQ(source.data(CMakeTreeModel::PathRole).toString(), QString::fromStdString(at("src/core.c")));
    EXPECT_EQ(model.parent(model.parent(source)), core);
    EXPECT_FALSE(model.index(5, 0, project).isValid());
    EXPECT_FALSE(model.index(0, 1, project).isValid());

    QTreeView tree;
    tree.setModel(&model);
    tree.expandAll();
    tree.resize(300, 400);
    EXPECT_TRUE(tree.visualRect(source).isValid());

    model.setViewModel(nullptr);
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_FALSE(model.indexForKey("target:core").isValid());
}

class FakeBackend final : public ICMakeBackend {
public:
    Result<void> reload() override {
        ++reloads;
        return {};
    }
    Result<void> selectProfile(std::size_t index) override {
        selected.push_back(index);
        return fail(ErrorCode::Busy, "the profile cannot change while a CMake job is in progress");
    }
    void cancel() override { ++cancels; }
    std::vector<std::string> presetFiles() const override { return {at("CMakePresets.json")}; }
    std::optional<std::string> presetsError() const override { return std::nullopt; }
    project::CMakePresetListing presetListing() const override { return listing; }

    project::CMakePresetListing listing;
    int reloads = 0;
    int cancels = 0;
    std::vector<std::size_t> selected;
};

TEST(CMakeToolWindow, FollowsTheServiceEvents) {
    FakeBackend backend;
    auto controller = std::make_unique<CMakeController>();
    controller->setBackend(&backend);
    const build::CMakeServiceCallbacks callbacks = controller->callbacks();
    auto window = std::make_unique<CMakeToolWindow>(*controller);
    window->resize(500, 300);
    EXPECT_FALSE(window->errorBanner()->isVisibleTo(window.get()));

    // The events of a failing configure, from a thread like the service's.
    std::thread service([&] {
        project::CMakeProfile debug{.name = "Debug", .displayName = "Debug"};
        project::CMakeProfile release{.name = "Release", .displayName = "Release (Ninja)"};
        callbacks.onProfilesChanged({debug, release}, 0);
        callbacks.onStateChanged(build::BuildState::Running);
        callbacks.onConfigureStarted("Debug");
        callbacks.onConfigureOutput(build::BuildOutputKind::Status, "Configure started: profile Debug");
        callbacks.onConfigureOutput(build::BuildOutputKind::Stderr, "CMake Error at CMakeLists.txt:3 (message):");
        build::CMakeConfigureOutcome outcome;
        outcome.profile = "Debug";
        outcome.counts.errors = 1;
        callbacks.onConfigureFinished(outcome);
        callbacks.onModelChanged(sampleProject());
        callbacks.onStateChanged(build::BuildState::Idle);
    });
    service.join();
    ASSERT_TRUE(pumpUntil([&] { return controller->treeModel()->rowCount() == 1 && !controller->busy(); }));

    EXPECT_EQ(window->profileSelector()->count(), 2);
    EXPECT_EQ(window->profileSelector()->currentIndex(), 0);
    EXPECT_EQ(window->profileSelector()->itemText(1), QStringLiteral("Release (Ninja)"));
    const QString output = window->output()->toPlainText();
    EXPECT_TRUE(output.contains(QStringLiteral("Configure started: profile Debug")));
    EXPECT_TRUE(output.contains(QStringLiteral("CMake Error at CMakeLists.txt:3")));
    EXPECT_TRUE(window->errorBanner()->isVisibleTo(window.get()));
    EXPECT_TRUE(window->errorBanner()->text().contains(QStringLiteral("1 error")));
    EXPECT_TRUE(window->reloadButton()->isEnabled());
    EXPECT_FALSE(window->stopButton()->isEnabled());
    // The presets file sits under the CMake node.
    const QModelIndex cmake = controller->treeModel()->indexForKey("cmake");
    EXPECT_EQ(childLabels(*controller->treeModel(), cmake), (QStringList{"CMakeLists.txt", "CMakePresets.json"}));

    window->reloadButton()->click();
    EXPECT_EQ(backend.reloads, 1);
    emit window->profileSelector()->activated(1); // As if the user picked Release.
    ASSERT_EQ(backend.selected.size(), 1U);
    EXPECT_EQ(backend.selected[0], 1U);
    EXPECT_EQ(window->profileSelector()->currentIndex(), 0); // Refused: back to the active one.
    EXPECT_TRUE(window->errorBanner()->text().contains(QStringLiteral("in progress")));

    // Busy: Reload off, Stop on.
    callbacks.onStateChanged(build::BuildState::Running);
    ASSERT_TRUE(pumpUntil([&] { return controller->busy(); }));
    EXPECT_FALSE(window->reloadButton()->isEnabled());
    window->stopButton()->click();
    EXPECT_EQ(backend.cancels, 1);

    // A successful configure clears the banner and the output.
    callbacks.onConfigureStarted("Debug");
    build::CMakeConfigureOutcome success;
    success.result.success = true;
    callbacks.onConfigureFinished(success);
    callbacks.onStateChanged(build::BuildState::Idle);
    ASSERT_TRUE(pumpUntil([&] { return !controller->busy(); }));
    EXPECT_FALSE(window->errorBanner()->isVisibleTo(window.get()));
    EXPECT_TRUE(window->output()->toPlainText().isEmpty());

    // Events after the controller is gone do nothing.
    window.reset();
    controller.reset();
    std::thread late([&] { callbacks.onConfigureOutput(build::BuildOutputKind::Stdout, "late"); });
    late.join();
    QApplication::processEvents();
}

/*
 * CMK-15: the progress row follows the service's progress events (newest sequence wins, so a
 * late older event is ignored), shows the elapsed seconds and an indeterminate bar, and Stop
 * follows the activity's cancellable flag; the Presets tab lists the backend's presets per kind.
 */
TEST(CMakeToolWindow, ShowsProgressAndPresets) {
    FakeBackend backend;
    backend.listing.configure = {{"windows-debug", "Windows Debug"}, {"linux"}};
    backend.listing.build = {{"windows-debug"}};
    backend.listing.workflow = {{"ci", "CI"}};
    auto controller = std::make_unique<CMakeController>();
    controller->setBackend(&backend);
    const build::CMakeServiceCallbacks callbacks = controller->callbacks();
    CMakeToolWindow window(*controller);
    window.resize(500, 300);
    EXPECT_FALSE(window.progressRow()->isVisibleTo(&window));

    const auto since = std::chrono::steady_clock::now() - 3s;
    std::thread service([&] {
        callbacks.onProfilesChanged({project::CMakeProfile{.name = "windows-debug"}}, 0);
        callbacks.onStateChanged(build::BuildState::Running);
        callbacks.onProgress({.activity = build::CMakeActivity::Configuring,
                              .profile = "windows-debug",
                              .since = since,
                              .cancellable = true,
                              .sequence = 2});
        // An older report delivered late (a direct delivery racing the queue) changes nothing.
        callbacks.onProgress({.activity = build::CMakeActivity::Preparing, .profile = "windows-debug", .sequence = 1});
    });
    service.join();
    ASSERT_TRUE(pumpUntil([&] { return controller->progress().sequence == 2 && controller->busy(); }));
    QApplication::processEvents();
    EXPECT_EQ(controller->progress().activity, build::CMakeActivity::Configuring);
    EXPECT_TRUE(window.progressRow()->isVisibleTo(&window));
    const QString label = window.progressLabel()->text();
    EXPECT_TRUE(label.startsWith(QStringLiteral("Configuring windows-debug ("))) << label.toStdString();
    EXPECT_TRUE(label.endsWith(QStringLiteral(" s)"))) << label.toStdString();
    EXPECT_FALSE(label.contains(QStringLiteral("(0 s)"))) << label.toStdString(); // Began 3 s ago.
    EXPECT_EQ(window.progressBar()->minimum(), 0);
    EXPECT_EQ(window.progressBar()->maximum(), 0); // Indeterminate.
    EXPECT_TRUE(window.stopButton()->isEnabled());

    // Reading the reply cannot be stopped.
    callbacks.onProgress({.activity = build::CMakeActivity::ReadingModel, .profile = "windows-debug", .sequence = 3});
    ASSERT_TRUE(pumpUntil([&] { return controller->progress().sequence == 3; }));
    EXPECT_TRUE(window.progressLabel()->text().startsWith(QStringLiteral("Reading the CMake project model")));
    EXPECT_FALSE(window.stopButton()->isEnabled());

    callbacks.onProgress({.activity = build::CMakeActivity::Idle, .sequence = 4});
    callbacks.onStateChanged(build::BuildState::Idle);
    ASSERT_TRUE(pumpUntil([&] { return controller->progress().sequence == 4 && !controller->busy(); }));
    EXPECT_FALSE(window.progressRow()->isVisibleTo(&window));
    EXPECT_TRUE(controller->progressText().isEmpty());

    // The Presets tab: the listed kinds only, in cmake's order, with display names.
    QTreeWidget* presets = window.presets();
    ASSERT_EQ(presets->topLevelItemCount(), 3);
    EXPECT_EQ(presets->topLevelItem(0)->text(0), QStringLiteral("Configure presets"));
    ASSERT_EQ(presets->topLevelItem(0)->childCount(), 2);
    EXPECT_EQ(presets->topLevelItem(0)->child(0)->text(0), QStringLiteral("windows-debug"));
    EXPECT_EQ(presets->topLevelItem(0)->child(0)->text(1), QStringLiteral("Windows Debug"));
    EXPECT_EQ(presets->topLevelItem(0)->child(1)->text(0), QStringLiteral("linux"));
    EXPECT_EQ(presets->topLevelItem(1)->text(0), QStringLiteral("Build presets"));
    EXPECT_EQ(presets->topLevelItem(2)->text(0), QStringLiteral("Workflow presets"));
    EXPECT_EQ(presets->topLevelItem(2)->child(0)->text(1), QStringLiteral("CI"));
    controller->setBackend(nullptr);
}

TEST(CMakeToolWindow, BindsToTheRealService) {
    ManualClock clock;
    io::MemFs fs(clock);
    const std::string folder = root();
    ASSERT_TRUE(fs.createDirectories(folder).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(at("CMakeLists.txt"), "project(Tiny C)\n").has_value());
    frost::test::installCMakeReply(fs, FROST_SOURCE_DIR "/tests/project/fixtures/cmake/reply", folder,
                                   at("cmake-build-debug"), kWindows ? "C:\\w\\other" : "/w/other");
    auto trust = project::TrustStore::open(fs, kWindows ? "C:\\w\\trusted-roots.txt" : "/w/trusted-roots.txt");
    ASSERT_TRUE(trust.has_value()) << trust.error().describe();
    io::ScriptedProcessLauncher launcher;

    CMakeController controller;
    build::CMakeServiceOptions options;
    options.cmakePath = kWindows ? "C:\\tools\\cmake.exe" : "/tools/cmake";
    options.useVisualStudio = false;
    options.captureMsvcEnvironment = false;
    auto service = build::CMakeService::create(fs, launcher, **trust, options, controller.callbacks());
    CMakeServiceBackend backend(*service);
    controller.setBackend(&backend);
    CMakeToolWindow window(controller);

    ASSERT_TRUE(service->openFolder(folder).has_value());
    ASSERT_TRUE(pumpUntil([&] { return controller.treeModel()->rowCount() == 1 && controller.activeProfile() == 0; }));
    EXPECT_EQ(controller.treeModel()->index(0, 0).data().toString(), QStringLiteral("Tiny"));
    EXPECT_TRUE(controller.treeModel()->indexForKey("target:core").isValid());
    EXPECT_EQ(window.profileSelector()->itemText(0), QStringLiteral("Debug"));

    // Reload of the untrusted folder waits for trust (busy); Stop ends the wait. Nothing ran.
    window.reloadButton()->click();
    ASSERT_TRUE(pumpUntil([&] { return controller.busy(); }));
    EXPECT_EQ(service->state(), build::BuildState::NeedsTrust);
    window.stopButton()->click();
    ASSERT_TRUE(pumpUntil([&] { return !controller.busy(); }));
    EXPECT_TRUE(launcher.launches().empty());

    controller.setBackend(nullptr);
    service.reset();
}

} // namespace
} // namespace frost::ui::cmake
