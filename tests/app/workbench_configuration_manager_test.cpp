// SPDX-License-Identifier: Apache-2.0
//
// SLN-7 through the Workbench, with this machine's MSBuild, the shipped ProjectHost and the pinned
// clangd (Windows; labels "integration", "msbuild" and "clangd"): the generated solution
// (tests/project/fixtures/msvc_generated, copied to a work directory) opens on Debug|x64 and
// clangd builds Core/src/a.cpp from GetClCommandLines (SLN-5), with the NDEBUG block inactive.
// The Configuration Manager then
//  1. switches the active configuration to Release|x64: clangd restarts on Release's own database
//     directory and the inactive region flips to the #else block;
//  2. maps Core to Release|x64 in the solution's Debug|x64 and makes Debug|x64 active: the .sln
//     line changes, clangd restarts on Debug's directory and a.cpp still builds with NDEBUG.
// The inactive regions are clangd's own (textDocument/inactiveRegions), shown by the editor.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/project/project_host.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/configuration_manager_dialog.hpp"

#include "project/msbuild_oracle.hpp"
#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
namespace project = frost::project;

constexpr std::string_view kGenerated = "../../project/fixtures/msvc_generated";

// The editor shows `name` inside one of clangd's inactive regions.
bool shownInactive(frost::app::DocumentEditorView& view, std::string_view name) {
    const auto& snapshot = view.editor().document().text();
    return std::ranges::any_of(view.editor().inactiveRanges(), [&](const frost::text::ByteRange& range) {
        return snapshot.text(range).find(name) != std::string::npos;
    });
}

// The running clangd's own inactive regions for `path` (none before its first build of it) contain
// `name`. A restarted clangd starts without any, so a stale answer of the previous server never counts.
bool clangdInactive(frost::app::Workbench& workbench, frost::app::DocumentEditorView& view, const std::string& path,
                    std::string_view name) {
    auto* clangd = workbench.clangd();
    const auto regions = clangd != nullptr ? clangd->inactiveRegions(path) : std::nullopt;
    if (!regions || regions->ranges.empty())
        return false;
    const auto& snapshot = view.editor().document().text();
    return std::ranges::any_of(regions->ranges, [&](const frost::text::ByteRange& range) {
        return snapshot.text(range).find(name) != std::string::npos;
    });
}

// clangd runs on GetClCommandLines' database in `<cache>/clcommandlines/<directory>`.
bool clangdOn(frost::app::Workbench& workbench, std::string_view directory) {
    auto* clangd = workbench.clangd();
    if (clangd == nullptr || clangd->state() != frost::lang::ClangdState::Running)
        return false;
    const auto location = clangd->compileCommands();
    return location && location->source == frost::lang::CompileCommandsSource::ClCommandLines &&
           location->directory.ends_with(std::string("clcommandlines\\") + std::string(directory));
}

// Runs the Configuration Manager action; `drive` gets the open dialog (then OK is clicked).
void runConfigurationManager(WorkbenchRig& rig, std::function<void(frost::ui::ConfigurationManagerDialog&)> drive) {
    QTimer::singleShot(0, rig.window.get(), [&rig, drive = std::move(drive)] {
        auto* dialog =
            rig.window->findChild<frost::ui::ConfigurationManagerDialog*>(QStringLiteral("configurationManagerDialog"));
        if (dialog == nullptr) {
            ADD_FAILURE() << "no Configuration Manager";
            if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                modal->reject();
            return;
        }
        drive(*dialog);
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QPushButton* ok = buttons != nullptr ? buttons->button(QDialogButtonBox::Ok) : nullptr;
        if (ok == nullptr) {
            ADD_FAILURE() << "the Configuration Manager has no OK";
            dialog->reject();
            return;
        }
        ok->click();
        if (dialog->isVisible()) {
            ADD_FAILURE() << "OK did not close the Configuration Manager";
            dialog->reject();
        }
    });
    const auto id = rig.workbench->registry().find("build.configurationManager");
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(rig.workbench->registry().tryInvoke(*id, frost::input::ActionContext{}), frost::input::InvokeResult::Ran);
}

TEST(WorkbenchConfigurationManager, SwitchingAndRemappingRestartClangdOnTheNewDatabaseAndFlipNdebug) {
    frost::test::disableCrashDialogs(); // clangd inherits the error mode: no WER box if it dies.
    auto instance = project::test::machineMsBuild();
    if (!instance)
        GTEST_SKIP() << "no Visual Studio with MSBuild is installed (vswhere found none)";
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    const auto host = frost::io::normalizePath(FROST_PROJECT_HOST);
    ASSERT_TRUE(host && frost::io::realFileSystem().exists(*host).value_or(false)) << FROST_PROJECT_HOST;

    WorkDir dir("configuration-manager-clangd", kGenerated);
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    options.workbench.projectHost = true;
    options.workbench.msbuildPath = instance->msbuildPath;
    options.workbench.projectEditHostPath = *host;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Generated.sln")));
    const auto solution = rig.workbench->solution();
    ASSERT_NE(solution, nullptr);
    const auto debug = solution->findConfiguration("Debug|x64");
    const auto release = solution->findConfiguration("Release|x64");
    ASSERT_TRUE(debug && release);
    if (rig.workbench->build().activeConfigurationIndex() != debug)
        ASSERT_TRUE(rig.workbench->build().setActiveConfiguration(*debug).has_value());

    const std::string a = dir.join("Core/src/a.cpp");
    ASSERT_TRUE(rig.workbench->openPath(a));
    frost::app::DocumentEditorView* view = rig.view();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(waitFor([&] { return clangdOn(*rig.workbench, "Debug_x64"); }, 300s))
        << "clangd never ran on Debug|x64's GetClCommandLines";
    ASSERT_TRUE(waitFor([&] { return clangdInactive(*rig.workbench, *view, a, "coreReleaseOnly"); }, 120s))
        << "Debug|x64 has no inactive NDEBUG block";
    ASSERT_TRUE(waitFor([&] { return shownInactive(*view, "coreReleaseOnly"); }, 30s)) << "the editor does not show it";
    EXPECT_FALSE(shownInactive(*view, "coreDebugOnly"));

    // 1. Release|x64 becomes active.
    runConfigurationManager(rig, [&](frost::ui::ConfigurationManagerDialog& dialog) {
        dialog.showConfiguration(static_cast<int>(*release));
        EXPECT_TRUE(dialog.edits().empty());
    });
    EXPECT_EQ(rig.workbench->build().activeConfigurationIndex(), release);
    ASSERT_TRUE(waitFor([&] { return clangdOn(*rig.workbench, "Release_x64"); }, 300s))
        << "clangd did not restart on Release|x64's database";
    ASSERT_TRUE(waitFor([&] { return clangdInactive(*rig.workbench, *view, a, "coreDebugOnly"); }, 120s))
        << "Release|x64 did not flip the inactive region";
    ASSERT_TRUE(waitFor([&] { return shownInactive(*view, "coreDebugOnly"); }, 30s)) << "the editor does not show it";
    EXPECT_FALSE(shownInactive(*view, "coreReleaseOnly"));

    // 2. Debug|x64 builds Core as Release|x64, and Debug|x64 becomes active again.
    const std::string sln = dir.join("Generated.sln");
    runConfigurationManager(rig, [&](frost::ui::ConfigurationManagerDialog& dialog) {
        dialog.showConfiguration(static_cast<int>(*debug));
        project::ProjectId core{0};
        for (std::size_t p = 0; p < solution->projects().size(); ++p)
            if (solution->projects()[p].name == "Core")
                core = project::ProjectId{static_cast<std::uint32_t>(p)};
        EXPECT_TRUE(dialog.setMapping(
            core, project::ProjectConfigurationMapping{
                      .configuration = "Release", .platform = "x64", .build = true, .deploy = false}));
        EXPECT_EQ(dialog.edits().size(), 1U);
    });
    const std::string text = frost::io::realFileSystem().readFile(sln).value_or("");
    EXPECT_NE(text.find("{6A1D0C1E-0B0E-4C1B-9E51-0000000C0E01}.Debug|x64.ActiveCfg = Release|x64"), std::string::npos)
        << text;
    EXPECT_EQ(rig.workbench->build().activeConfigurationIndex(), debug);
    ASSERT_TRUE(waitFor([&] { return clangdOn(*rig.workbench, "Debug_x64"); }, 300s))
        << "clangd did not restart on Debug|x64's database";
    ASSERT_TRUE(waitFor([&] { return clangdInactive(*rig.workbench, *view, a, "coreDebugOnly"); }, 120s))
        << "Core mapped to Release did not build with NDEBUG in Debug|x64";
    ASSERT_TRUE(waitFor([&] { return shownInactive(*view, "coreDebugOnly"); }, 30s)) << "the editor does not show it";
    EXPECT_FALSE(shownInactive(*view, "coreReleaseOnly"));
    EXPECT_TRUE(rig.workbench->closeProject());
}

} // namespace
