// SPDX-License-Identifier: Apache-2.0
// RUN-12 deferred real selected-configuration action -> genuine LLVM results -> source gutter journey.
#include "frost/build/wsl_cmake.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/run/native_analysis.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/notifications.hpp"

#include "workbench_rig.hpp"
#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QTimer>
namespace {
TEST(WorkbenchNativeCoverage, ChangingSelectedConfigurationBeforeWorkerQueueStartsNoChild) {
    using namespace frost;
    app::test::WorkDir dir("coverage-queued-revocation");
    auto& fs = io::realFileSystem();
    const auto root = dir.join("project");
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.createDirectories(dir.join("artifacts")));
#ifdef _WIN32
    const std::string suffix = ".exe";
#else
    const std::string suffix;
#endif
    const auto program = dir.join("project/program" + suffix);
    dir.write("project/program" + suffix, "unexecuted image fixture");
    dir.write("sdk/bin/llvm-cov" + suffix, "unexecuted tool fixture");
    dir.write("sdk/bin/llvm-profdata" + suffix, "unexecuted tool fixture");
    const auto oldRoot = qgetenv("FROST_LLVM_ROOT");
    qputenv("FROST_LLVM_ROOT", QByteArray::fromStdString(dir.join("sdk")));
    const auto restore = qScopeGuard([&] {
        if (oldRoot.isNull())
            qunsetenv("FROST_LLVM_ROOT");
        else
            qputenv("FROST_LLVM_ROOT", oldRoot);
    });
    io::ScriptedProcessLauncher launcher;
    app::test::RigOptions options;
    options.launcher = &launcher;
    app::test::WorkbenchRig* owner = nullptr;
    bool queued = false;
    const input::ActionContext debugContext{{}, input::AvailabilityLevel::L4, true};
    QPointer<QProgressDialog> progress;
    options.workbench.nativeAnalysisBeforeQueue = [&] {
        queued = true;
        progress = owner->window->findChild<QProgressDialog*>("nativeAnalysisProgress");
        auto& registry = owner->workbench->registry();
        const auto attach = registry.find("debug.attachUnity");
        ASSERT_TRUE(attach);
        EXPECT_FALSE(registry.isEnabled(*attach, debugContext));
        EXPECT_TRUE(owner->workbench->runController()->selectConfiguration("coverage.second"));
    };
    app::test::WorkbenchRig rig(dir, std::move(options));
    owner = &rig;
    ASSERT_TRUE(rig.trust->trust(root));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(root));
    run::RunConfiguration first;
    first.id = "coverage.first";
    first.name = "First";
    first.program = program;
    first.workingDirectory = root;
    auto second = first;
    second.id = "coverage.second";
    second.name = "Second";
    rig.workbench->runController()->setStoredConfigurations({first, second});
    ASSERT_TRUE(rig.workbench->runController()->selectConfiguration(first.id));
    const auto attach = rig.workbench->registry().find("debug.attachUnity");
    ASSERT_TRUE(attach);
    ASSERT_TRUE(rig.workbench->registry().isEnabled(*attach, debugContext));
    const bool oldNative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs =
        qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, oldNative); });
    QTimer dialogs;
    dialogs.setInterval(20);
    QObject::connect(&dialogs, &QTimer::timeout, [&] {
        if (auto* choose = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
            choose->setDirectory(QString::fromStdString(dir.join("artifacts")));
            choose->selectFile(QString::fromStdString(dir.join("artifacts")));
            static_cast<QDialog*>(choose)->accept();
        }
    });
    dialogs.start();
    EXPECT_EQ(rig.window->invoke("run.coverage"), input::InvokeResult::Ran);
    ASSERT_TRUE(
        app::test::waitFor([&] { return queued && (!progress || !progress->isVisible()); }, std::chrono::seconds(30)));
    EXPECT_TRUE(launcher.launches().empty());
}

#ifdef _WIN32
TEST(WorkbenchWslCMake, RefusesBuildWhenDebugPreparationStartsInsideProfileDialog) {
    using namespace frost;
    app::test::WorkDir dir("wsl-build-pending-debug");
    const auto root = dir.join("project");
    ASSERT_TRUE(io::realFileSystem().createDirectories(root));
    build::RemoteTarget target;
    target.id = "wsl.test";
    target.name = "Test distribution";
    target.transportExecutable = dir.join("wsl.exe");
    target.distribution = "TestDistribution";
    target.mappings = {{root, "/src"}};
    target.executionApproved = true;
    dir.write("remote.json", build::remoteTargetsJson({target}));
    build::WslCMakeProfile profile;
    profile.id = "wsl.profile";
    profile.name = "Debug";
    profile.targetId = target.id;
    profile.localSourceDirectory = root;
    profile.buildDirectory = "/build";
    ASSERT_TRUE(build::wslCMakeCommand(profile, target, build::WslCMakeOperation::Build, 1));
    dir.write("wsl.json", build::wslCMakeProfilesJson({profile}));

    io::ScriptedProcessLauncher launcher;
    app::test::RigOptions options;
    options.launcher = &launcher;
    options.workbench.remoteTargetsPath = dir.join("remote.json");
    options.workbench.wslCMakeProfilesPath = dir.join("wsl.json");
    app::test::WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(root));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(root));
    auto* sessions = rig.workbench->debugSessions();
    ASSERT_NE(sessions, nullptr);
    ASSERT_FALSE(sessions->hasPendingWork());

    ui::DebugGroupId preparing = 0;
    bool acceptedBuild = false;
    QTimer dialogs;
    dialogs.setInterval(10);
    QObject::connect(&dialogs, &QTimer::timeout, [&] {
        auto* active = QApplication::activeModalWidget();
        if (auto* choice = qobject_cast<QInputDialog*>(active)) {
            static_cast<QDialog*>(choice)->accept();
            return;
        }
        auto* dialog = qobject_cast<QDialog*>(active);
        if (!dialog || dialog->objectName() != "wslCMakeProfileDialog")
            return;
        preparing = sessions->createGroup();
        for (auto* button : dialog->findChildren<QPushButton*>()) {
            if (button->text() != "Build")
                continue;
            button->click();
            acceptedBuild = dialog->result() == QDialog::Accepted;
            break;
        }
        if (!acceptedBuild)
            dialog->reject();
    });
    dialogs.start();
    EXPECT_EQ(rig.window->invoke("cmake.wsl"), input::InvokeResult::Ran);
    dialogs.stop();

    EXPECT_NE(preparing, 0U);
    EXPECT_TRUE(acceptedBuild);
    EXPECT_TRUE(sessions->hasPendingWork());
    EXPECT_EQ(rig.window->findChild<QDialog*>("wslCMakeOutputDialog"), nullptr);
    EXPECT_TRUE(launcher.launches().empty());
    const auto& entries = rig.window->notifications().log();
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries.back().notification.title, "WSL CMake");
    EXPECT_TRUE(entries.back().notification.text.contains("debugger"));
    sessions->finishPreparing(preparing);
}
#endif

TEST(WorkbenchNativeCoverage, ActualSelectedNativeRunPublishesCountsToGutterAndEditExpiresThem) {
    using namespace frost;
    app::test::WorkDir dir("coverage-normal-action");
    auto& fs = io::realFileSystem();
    const auto root = dir.join("project"), source = dir.join("project/coverage.cpp");
    const auto binary = dir.join(
#ifdef _WIN32
        "project/coverage.exe"
#else
        "project/coverage"
#endif
    );
    dir.write("project/coverage.cpp", "int cold(){return 9;}\nint hot(){return 0;}\nint main(){return hot();}\n");
    ASSERT_TRUE(fs.createDirectories(dir.join("artifacts")));
    io::ProcessSpec compile;
    compile.executable = io::normalizePath(FROST_TEST_CXX_COMPILER).value();
    compile.workingDirectory = root;
    compile.arguments = {"--driver-mode=g++",  "-std=c++20", "-O0", "-g",  "-fprofile-instr-generate",
                         "-fcoverage-mapping", source,       "-o",  binary};
    auto built = run::runAnalysisCommand(io::realProcessLauncher(), compile, {});
    ASSERT_TRUE(built);
    ASSERT_EQ(built->exitCode, 0) << built->error;
    const auto oldRoot = qgetenv("FROST_LLVM_ROOT");
    const auto restore = qScopeGuard([&] {
        if (oldRoot.isNull())
            qunsetenv("FROST_LLVM_ROOT");
        else
            qputenv("FROST_LLVM_ROOT", oldRoot);
    });
    qputenv("FROST_LLVM_ROOT",
            QByteArray::fromStdString(std::string(io::parentPath(io::parentPath(compile.executable)))));
    app::test::WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(root));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(root));
    ASSERT_TRUE(rig.workbench->openPath(source));
    run::RunConfiguration configuration;
    configuration.id = "coverage.actual";
    configuration.name = "Actual coverage";
    configuration.type = run::RunType::Native;
    configuration.program = binary;
    configuration.workingDirectory = root;
    rig.workbench->runController()->setStoredConfigurations({configuration});
    ASSERT_TRUE(rig.workbench->runController()->selectConfiguration(configuration.id));
    const bool nativeDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs =
        qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDisabled); });
    QString error;
    QTimer dialogs;
    dialogs.setInterval(20);
    QObject::connect(&dialogs, &QTimer::timeout, [&] {
        if (auto* choose = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
            choose->setDirectory(QString::fromStdString(dir.join("artifacts")));
            choose->selectFile(QString::fromStdString(dir.join("artifacts")));
            static_cast<QDialog*>(choose)->accept();
        } else if (auto* problem = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            error = problem->text();
            problem->accept();
        }
    });
    dialogs.start();
    EXPECT_EQ(rig.window->invoke("run.coverage"), input::InvokeResult::Ran);
    QTableWidget* files = nullptr;
    ASSERT_TRUE(app::test::waitFor(
        [&] {
            files = rig.window->findChild<QTableWidget*>("nativeCoverageFiles");
            return files || !error.isEmpty();
        },
        std::chrono::seconds(120)));
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_NE(files, nullptr);
    ASSERT_EQ(files->rowCount(), 1);
    files->setCurrentCell(0, 0);
    auto* apply = rig.window->findChild<QPushButton*>("nativeCoverageApplyGutter");
    ASSERT_NE(apply, nullptr);
    apply->click();
    ASSERT_TRUE(app::test::waitFor(
        [&] {
            auto* view = rig.workbench->currentView();
            return view && !view->editor().coverageMarkers().empty();
        },
        std::chrono::seconds(30)));
    const auto& markers = rig.workbench->currentView()->editor().coverageMarkers();
    ASSERT_GE(markers.size(), 3u);
    EXPECT_EQ(markers[0].hits, 0u);
    EXPECT_EQ(markers[1].hits, 1u);
    QKeyEvent edit(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCoreApplication::sendEvent(&rig.workbench->currentView()->editor(), &edit);
    EXPECT_TRUE(rig.workbench->currentView()->editor().coverageMarkers().empty());
    const auto identity = run::captureAnalysisIdentity(fs, source);
    ASSERT_TRUE(identity);
    ASSERT_TRUE(fs.writeFileAtomic(source, "int externally_changed;\n"));
    EXPECT_FALSE(run::validateAnalysisIdentity(fs, *identity));
}
} // namespace
