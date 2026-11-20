// SPDX-License-Identifier: Apache-2.0
// Normal action journey uses the real compiler orchestration with scripted process bytes.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "workbench_rig.hpp"
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
namespace {
using namespace frost::app::test;
QDialog* dxcDialog() {
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "dxcDiagnosticsDialog")
            return qobject_cast<QDialog*>(widget);
    return nullptr;
}
TEST(WorkbenchDxc, NormalProfileActionPublishesActualDiagnosticIntoIndependentProblemsSource) {
    WorkDir dir("dxc-action");
    dir.write("project/test.hlsl", "float4 main():SV_Target { return missing; }\n");
    dir.write("installed/dxc.exe", "fixture image is never executed");
    frost::io::ScriptedProcessLauncher launcher;
    frost::io::ScriptedProcess script;
    script.expectExecutable = dir.join("installed/dxc.exe");
    script.exitCode = 1;
    script.stderrBytes = "test.hlsl(1,33): error: use of undeclared identifier 'missing'\n";
    launcher.expect(std::move(script));
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project/test.hlsl")));
    QTimer::singleShot(0, rig.window.get(), [&] {
        auto* dialog = dxcDialog();
        ASSERT_NE(dialog, nullptr);
        dialog->findChild<QLineEdit*>("dxcExecutable")->setText(QString::fromStdString(dir.join("installed/dxc.exe")));
        dialog->findChild<QLineEdit*>("dxcSource")->setText(QString::fromStdString(dir.join("project/test.hlsl")));
        dialog->findChild<QLineEdit*>("dxcEntry")->setText("main");
        dialog->findChild<QLineEdit*>("dxcTarget")->setText("ps_6_0");
        dialog->accept();
    });
    EXPECT_EQ(rig.window->invoke("code.dxcDiagnostics"), frost::input::InvokeResult::Ran);
    auto* problems = rig.window->findChild<frost::ui::ProblemsView*>();
    ASSERT_NE(problems, nullptr);
    ASSERT_TRUE(waitFor([&] { return !problems->model()->problems().empty(); }));
    const auto& problem = problems->model()->problems().front();
    EXPECT_EQ(problem.source, frost::ui::DiagnosticSource::Dxc);
    EXPECT_EQ(problem.diagnostic.path, dir.join("project/test.hlsl"));
    EXPECT_EQ(problem.diagnostic.diagnostic.line, 1);
    EXPECT_EQ(problem.diagnostic.diagnostic.column, 33);
    EXPECT_EQ(problem.diagnostic.diagnostic.message, "use of undeclared identifier 'missing'");
    ASSERT_EQ(launcher.launches().size(), 1u);
    EXPECT_EQ(launcher.launches()[0].spec.arguments.back(), dir.join("project/test.hlsl"));
}
TEST(WorkbenchDxc, ShellDestructionDuringNestedCompilerPickerNeverDeletesStackModalOrLaunches) {
    WorkDir dir("dxc-destroy");
    dir.write("project/test.hlsl", "float4 main():SV_Target{return 0;}\n");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    bool pickerSeen = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = dxcDialog();
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->parent(), nullptr);
        QTimer::singleShot(0, [&] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (widget->objectName() == "dxcCompilerPicker")
                    pickerSeen = true;
            rig.window.reset();
        });
        dialog->findChild<QPushButton*>("dxcBrowse")->click();
    });
    rig.workbench->showDxcDiagnostics();
    EXPECT_TRUE(pickerSeen);
    EXPECT_TRUE(launcher.launches().empty());
}
TEST(WorkbenchDxc, ProjectReplacementDuringSaveAllRefusesBeforeLaunchingOldProfile) {
    WorkDir dir("dxc-save-switch");
    dir.write("project/test.hlsl", "float4 main():SV_Target{return 0;}\n");
    dir.write("other/marker.txt", "other");
    dir.write("installed/dxc.exe", "never executed");
    frost::io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.trust->trust(dir.join("other")));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project/test.hlsl")));
    ASSERT_TRUE(rig.view()->editor().document().replace({0, 0}, "// dirty before Save All\n", {}));
    bool switched = false;
    // Opening another plain folder changes project admission without saving or closing documents.
    rig.workbench->documents().setSaveTransform([&](const frost::document::FileDocument&, frost::document::SaveReason) {
        if (!switched) {
            switched = true;
            EXPECT_TRUE(rig.workbench->openPath(dir.join("other")));
        }
        return frost::document::SaveTransform{};
    });
    QTimer::singleShot(0, [&] {
        auto* dialog = dxcDialog();
        ASSERT_NE(dialog, nullptr);
        dialog->findChild<QLineEdit*>("dxcExecutable")->setText(QString::fromStdString(dir.join("installed/dxc.exe")));
        dialog->findChild<QLineEdit*>("dxcSource")->setText(QString::fromStdString(dir.join("project/test.hlsl")));
        dialog->accept();
    });
    rig.workbench->showDxcDiagnostics();
    EXPECT_TRUE(switched);
    EXPECT_TRUE(launcher.launches().empty());
    rig.workbench->documents().setSaveTransform({});
}
} // namespace
