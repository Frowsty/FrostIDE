// SPDX-License-Identifier: Apache-2.0
//
// CPP-10 application integration: Inspect Code runs the pinned clang-tidy against the current
// module, publishes only its reported findings to the dedicated Problems source, and exports valid
// SARIF on explicit request. This real-tool journey runs off the GUI thread; no result is seeded.

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/inspection.hpp"
#include "frost/rpc/json.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "support/crash_dialogs.hpp"
#include "trusted_project_launcher.hpp"
#include "workbench_rig.hpp"
#include <QPushButton>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fio = frost::io;
namespace fl = frost::lang;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
using namespace std::chrono_literals;

TEST(WorkbenchInspection, DelayedToolLaunchRequiresTheOriginalTrustAdmission) {
    WorkDir dir("cpp10-trust-admission");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    frost::io::ScriptedProcessLauncher launcher;
    const auto admission = rig.trust->snapshot(dir.path());
    frost::app::TrustedProjectLauncher admitted(launcher, *rig.trust, dir.path(), admission.revision);
    frost::io::ProcessSpec spec;
    spec.executable = dir.join("clang-tidy");
    launcher.expect(frost::io::ScriptedProcess{.expectExecutable = spec.executable});
    {
        auto child = admitted.launch(spec);
        ASSERT_TRUE(child.has_value()) << child.error().describe();
    }
    ASSERT_EQ(launcher.launches().size(), 1u);

    ASSERT_TRUE(rig.trust->untrust(dir.path()).has_value());
    auto revoked = admitted.launch(spec);
    ASSERT_FALSE(revoked.has_value());
    EXPECT_EQ(revoked.error().code(), frost::ErrorCode::Untrusted);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    auto regranted = admitted.launch(spec);
    ASSERT_FALSE(regranted.has_value());
    EXPECT_EQ(regranted.error().code(), frost::ErrorCode::Untrusted);
    EXPECT_EQ(launcher.launches().size(), 1u) << "neither stale admission reached the underlying launcher";

    std::stop_source stop;
    frost::app::TrustedProjectLauncher cancelled(launcher, *rig.trust, dir.path(),
                                                 rig.trust->snapshot(dir.path()).revision, stop.get_token());
    stop.request_stop();
    auto stopped = cancelled.launch(spec);
    ASSERT_FALSE(stopped.has_value());
    EXPECT_EQ(stopped.error().code(), frost::ErrorCode::Cancelled);
    EXPECT_EQ(launcher.launches().size(), 1u);
}

TEST(WorkbenchInspection, InspectCppModulePublishesRealClangTidyFindingsAndExportsSarif) {
    frost::test::disableCrashDialogs();
    fio::IFileSystem& fs = fio::realFileSystem();
    fio::IProcessLauncher& launcher = fio::realProcessLauncher();
    auto tidy = fl::findClangTidy(fs, launcher);
    ASSERT_TRUE(tidy.has_value()) << tidy.error().describe();
    ASSERT_TRUE(fio::isAbsolutePath(tidy->path));

    WorkDir dir("cpp10-workbench-inspection");
    dir.write(".clang-tidy", "Checks: '-*,modernize-use-nullptr,modernize-use-using'\n"
                             "WarningsAsErrors: ''\n");
    constexpr std::string_view source = "typedef int LegacyInt;\n"
                                        "int* makeNull() {\n"
                                        "    int* value = 0;\n"
                                        "    return value;\n"
                                        "}\n";
    dir.write("src/sample.cpp", source);
    const std::string sourcePath = dir.join("src/sample.cpp");
    const std::vector<fl::CompileCommand> compileCommands = {fl::CompileCommand{
        .directory = dir.path(), .file = sourcePath, .arguments = {"clang++", "-std=c++20", "-c", sourcePath}}};
    ASSERT_TRUE(fl::writeCompileCommands(fs, dir.join("build"), compileCommands).has_value());

    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsClangTidyPath, tidy->path)
            .has_value());
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(sourcePath).has_value());
    ASSERT_NE(rig.view(), nullptr);

    const auto action = rig.workbench->registry().find("code.inspectCpp");
    ASSERT_TRUE(action.has_value());
    EXPECT_TRUE(rig.workbench->registry().info(*action).hasHandler);

    EXPECT_EQ(rig.workbench->registry().tryInvoke(*action, {}), frost::input::InvokeResult::Ran);

    ASSERT_NE(rig.workbench->problems(), nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            const auto& problems = rig.workbench->problems()->model()->problems();
            return std::ranges::count(problems, frost::ui::DiagnosticSource::Inspection, &frost::ui::Problem::source) >=
                   2;
        },
        120s));
    const auto& rows = rig.workbench->problems()->model()->problems();
    std::vector<const frost::ui::Problem*> inspectionRows;
    for (const frost::ui::Problem& problem : rows)
        if (problem.source == frost::ui::DiagnosticSource::Inspection)
            inspectionRows.push_back(&problem);
    EXPECT_TRUE(std::ranges::any_of(inspectionRows, [](const frost::ui::Problem* row) {
        return row->diagnostic.diagnostic.code == "modernize-use-nullptr" && row->diagnostic.diagnostic.line == 3;
    }));
    EXPECT_TRUE(std::ranges::any_of(inspectionRows, [](const frost::ui::Problem* row) {
        return row->diagnostic.diagnostic.code == "modernize-use-using" && row->diagnostic.diagnostic.line == 1;
    }));

    const std::string sarifPath = dir.join("inspection.sarif");
    ASSERT_TRUE(rig.workbench->exportCppInspectionSarif(sarifPath).has_value());
    auto bytes = fs.readFile(sarifPath);
    ASSERT_TRUE(bytes.has_value());
    auto sarif = frost::rpc::parseJson(*bytes);
    ASSERT_TRUE(sarif.has_value());
    EXPECT_TRUE(fl::validateSarifSchema(*sarif).has_value());

    ASSERT_TRUE(rig.trust->untrust(dir.path()).has_value());
    EXPECT_FALSE(rig.workbench->exportCppInspectionSarif(dir.join("revoked.sarif")).has_value());
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    EXPECT_FALSE(rig.workbench->exportCppInspectionSarif(dir.join("regranted.sarif")).has_value())
        << "a later trust grant does not revive an earlier inspection report";
    ASSERT_TRUE(waitFor([&] {
        const auto& problems = rig.workbench->problems()->model()->problems();
        return std::ranges::none_of(
            problems, [](const auto& row) { return row.source == frost::ui::DiagnosticSource::Inspection; });
    }));
}

TEST(WorkbenchInspection, UntrustedProjectCannotStartCppInspection) {
    WorkDir dir("cpp10-workbench-untrusted");
    const std::string sourcePath = dir.join("sample.cpp");
    dir.write("sample.cpp", "int main() { return 0; }\n");
    frost::io::ScriptedProcessLauncher launcher;
    frost::app::test::RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(sourcePath).has_value());
    auto* trust = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(trust, nullptr);
    EXPECT_TRUE(trust->isVisible());
    EXPECT_EQ(trust->purpose(), frost::ui::TrustPurpose::ProjectOpen);
    EXPECT_FALSE(rig.trust->isTrusted(dir.path()));

    // The menu action uses the same project trust prompt as opening/building. It must not spawn
    // clang-tidy while the user has not granted trust, and cancelling the prompt keeps the project
    // untrusted.
    const auto action = rig.workbench->registry().find("code.inspectCpp");
    ASSERT_TRUE(action.has_value());
    EXPECT_EQ(rig.workbench->registry().tryInvoke(*action, {}), frost::input::InvokeResult::Ran);
    EXPECT_TRUE(launcher.launches().empty());
    trust->cancelButton()->click();
    QApplication::processEvents();
    EXPECT_FALSE(rig.trust->isTrusted(dir.path()));

    std::optional<frost::Error> failure;
    rig.workbench->inspectCppCodeAsync(dir.path(), [&](frost::Result<fl::InspectionReport> result) {
        if (!result)
            failure = result.error();
        else
            ADD_FAILURE() << "untrusted project inspection unexpectedly ran clang-tidy";
    });
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->code(), frost::ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(WorkbenchInspection, SwitchingProjectsCancelsAnInspectionWaitingForResources) {
    frost::test::disableCrashDialogs();
    fio::IFileSystem& fs = fio::realFileSystem();
    WorkDir dir("cpp10-workbench-cancel");
    WorkDir next("cpp10-workbench-next");
    const std::string sourcePath = dir.join("sample.cpp");
    const std::string tidyPath = dir.join("clang-tidy.exe");
    dir.write("sample.cpp", "int main() { return 0; }\n");
    dir.write("clang-tidy.exe", "scripted process path\n");
    const std::vector<fl::CompileCommand> compileCommands = {fl::CompileCommand{
        .directory = dir.path(), .file = sourcePath, .arguments = {"clang++", "-std=c++20", "-c", sourcePath}}};
    ASSERT_TRUE(fl::writeCompileCommands(fs, dir.join("build"), compileCommands).has_value());

    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect(frost::io::ScriptedProcess{.expectExecutable = tidyPath, .stdoutBytes = "LLVM version 23.1.4\n"});
    frost::app::test::RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.trust->trust(next.path()).has_value());
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsClangTidyPath, tidyPath)
            .has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());

    ASSERT_NE(rig.workbench->resourceGovernor(), nullptr);
    bool completionCalled = false;
    {
        auto heldInspectionSlot =
            rig.workbench->resourceGovernor()->tryAcquireHeavyLeg(frost::sched::HeavyLegKind::Inspection);
        ASSERT_TRUE(heldInspectionSlot.has_value());
        rig.workbench->inspectCppCodeAsync(dir.path(),
                                           [&](frost::Result<fl::InspectionReport>) { completionCalled = true; });
        ASSERT_TRUE(waitFor([&] { return launcher.launches().size() == 1; }));
        ASSERT_TRUE(rig.workbench->openPath(next.path()).has_value());
    }
    const QDeadlineTimer cancelDeadline(250);
    while (!cancelDeadline.hasExpired()) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    EXPECT_FALSE(completionCalled);
    EXPECT_EQ(launcher.launches().size(), 1u);
    EXPECT_EQ(launcher.pendingScripts(), 0u);
}

TEST(WorkbenchInspection, ExportedScopeFixPreviewRefusesStaleHeaderThenAppliesAndUndoesOneTransaction) {
    frost::test::disableCrashDialogs();
    auto& fs = fio::realFileSystem();
    auto tidy = fl::findClangTidy(fs, fio::realProcessLauncher());
    ASSERT_TRUE(tidy) << tidy.error().describe();
    WorkDir dir("ref10-exported-module-fix"), recovery("ref10-private-recovery");
    const std::string before = "#include \"header.hpp\"\nint* sourcePointer = 0;\n";
    const std::string headerBefore = "inline int* headerPointer = 0;\n";
    dir.write("src/input.cpp", before);
    dir.write("src/header.hpp", headerBefore);
    dir.write(".clang-tidy", "Checks: '-*,modernize-use-nullptr'\nWarningsAsErrors: ''\n");
    const auto source = dir.join("src/input.cpp"), header = dir.join("src/header.hpp");
    ASSERT_TRUE(fl::writeCompileCommands(
        fs, dir.join("build"),
        std::vector<fl::CompileCommand>{{dir.path(), source, {"clang++", "-std=c++20", "-c", source}}}));
    frost::app::test::RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    options.workbench.refactoringStateDirectory = recovery.join("transactions");
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsClangTidyPath, tidy->path));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(source));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->clangd() && rig.workbench->clangd()->compileCommands().has_value(); }, 60s));
    const auto action = rig.workbench->registry().find("code.fixInScope");
    ASSERT_TRUE(action);
    EXPECT_TRUE(rig.workbench->registry().info(*action).hasHandler);
    using Plan = frost::document::RefactoringPlan;
    using Completion = std::optional<frost::Result<Plan>>;
    auto prepare = [&] {
        auto completed = std::make_shared<Completion>();
        fl::InspectionScope scope;
        scope.kind = fl::InspectionScopeKind::Project;
        rig.workbench->prepareCppInspectionScopeFixes(
            scope, {}, {}, [completed](frost::Result<Plan> plan) { completed->emplace(std::move(plan)); });
        return completed;
    };
    auto first = prepare();
    ASSERT_TRUE(waitFor([&] { return first->has_value(); }, 120s));
    ASSERT_TRUE(first->value()) << first->value().error().describe();
    EXPECT_EQ(first->value()->disk.size(), 1U);
    EXPECT_EQ(first->value()->buffers.size(), 1U);
    EXPECT_EQ(dir.read("src/input.cpp"), before);
    EXPECT_EQ(dir.read("src/header.hpp"), headerBefore);
    dir.write("src/header.hpp", "// external change\n" + headerBefore);
    EXPECT_FALSE(rig.workbench->applyRefactoring(*first->value()));
    EXPECT_EQ(rig.view()->editor().document().text().text(), before);
    EXPECT_FALSE(rig.workbench->canUndoRefactoring());
    auto second = prepare();
    ASSERT_TRUE(waitFor([&] { return second->has_value(); }, 120s));
    ASSERT_TRUE(second->value()) << second->value().error().describe();
    auto applied = rig.workbench->applyRefactoring(*second->value());
    ASSERT_TRUE(applied) << applied.error().describe();
    EXPECT_EQ(applied->status, frost::document::RefactoringStatus::Applied);
    EXPECT_EQ(rig.view()->editor().document().text().text(),
              "#include \"header.hpp\"\nint* sourcePointer = nullptr;\n");
    EXPECT_EQ(dir.read("src/header.hpp"), "// external change\ninline int* headerPointer = nullptr;\n");
    auto undone = rig.workbench->undoRefactoring();
    ASSERT_TRUE(undone) << undone.error().describe();
    EXPECT_EQ(rig.view()->editor().document().text().text(), before);
    EXPECT_EQ(dir.read("src/header.hpp"), "// external change\n" + headerBefore);
}

} // namespace
