// SPDX-License-Identifier: Apache-2.0
// RUN-12 deferred real selected-configuration action -> genuine LLVM results -> source gutter journey.
#include "frost/io/path.hpp"
#include "frost/run/native_analysis.hpp"
#include "frost/ui/run_controller.hpp"

#include "workbench_rig.hpp"
#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QTimer>
namespace {
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
