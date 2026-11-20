// SPDX-License-Identifier: Apache-2.0
// Normal action journey uses the real compiler orchestration with scripted process bytes.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "workbench_rig.hpp"
#include <QDialog>
#include <QLineEdit>
#include <QTimer>
namespace {
using namespace frost::app::test;
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
        auto* dialog = rig.window->findChild<QDialog*>("dxcDiagnosticsDialog");
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
} // namespace
