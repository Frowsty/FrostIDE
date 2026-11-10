// SPDX-License-Identifier: Apache-2.0
//
// SLN-2 and SLN-3 through the Workbench: a trusted generated solution (tests/project/fixtures/
// msvc_generated, copied to a work directory) opens, the shipped ProjectHost evaluates it with this
// machine's MSBuild, and the Solution Explorer shows exactly the oracle's items per project
// (MSBuild.exe run by the test, tests/project/msbuild_oracle.hpp). Killing the helper shows the
// fallback banner; a missing helper shows it from the start while MSBuild.exe evaluates. Windows,
// labels "integration" and "msbuild"; skipped without Visual Studio.

#include "frost/io/path.hpp"
#include "frost/project/project_host.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "project/msbuild_oracle.hpp"
#include "workbench_rig.hpp"
#include <QLabel>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <windows.h>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
namespace project = frost::project;

// The work directory copy of the generated solution (FROST_BUILD_FIXTURES is tests/build/fixtures).
constexpr std::string_view kGenerated = "../../project/fixtures/msvc_generated";

void killProcess(frost::io::ProcessId pid) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    ASSERT_NE(process, nullptr) << "OpenProcess " << GetLastError();
    EXPECT_TRUE(TerminateProcess(process, 9));
    EXPECT_EQ(WaitForSingleObject(process, 30000), WAIT_OBJECT_0);
    CloseHandle(process);
}

bool finished(const frost::app::Workbench& workbench) {
    const auto evaluation = workbench.solutionEvaluation();
    if (!evaluation)
        return false;
    const auto state = evaluation->status().state;
    return state == project::SolutionEvaluationState::Ready || state == project::SolutionEvaluationState::Stopped;
}

// The explorer's model (composed by the workbench's timer) carries every project's evaluation.
bool everyProjectShownEvaluated(const frost::ui::SolutionExplorer& explorer) {
    const auto shown = explorer.solution();
    return shown && std::ranges::all_of(shown->projects(),
                                        [](const project::ProjectInfo& info) { return info.evaluation.has_value(); });
}

// Every C++ project the explorer shows has the oracle's items for the active configuration.
void expectExplorerMatchesTheOracle(const frost::ui::SolutionExplorer& explorer, const std::string& msbuild,
                                    std::string_view configuration, project::EvaluationSource source,
                                    const project::SolutionEvaluationStatus& status) {
    std::string warnings;
    for (const std::string& warning : status.warnings)
        warnings += "\n  " + warning;
    const auto shown = explorer.solution();
    ASSERT_NE(shown, nullptr);
    const auto index = shown->findConfiguration(configuration);
    ASSERT_TRUE(index.has_value()) << configuration;
    ASSERT_EQ(shown->projects().size(), 3U);
    for (std::size_t p = 0; p < shown->projects().size(); ++p) {
        const project::ProjectId id{static_cast<std::uint32_t>(p)};
        const project::ProjectInfo& info = shown->project(id);
        SCOPED_TRACE(info.name);
        ASSERT_TRUE(info.evaluation.has_value()) << "evaluation warnings:" << warnings;
        const auto& mapping = shown->projectConfiguration(id, *index);
        ASSERT_TRUE(mapping.has_value());
        auto oracle = project::test::runOracle(msbuild, {.projectPath = info.path,
                                                         .configuration = mapping->configuration,
                                                         .platform = mapping->platform,
                                                         .solutionPath = shown->path()});
        ASSERT_TRUE(oracle.has_value());
        std::vector<std::string> files;
        for (const std::string& path : shown->projectFilePaths(id)) {
            files.push_back(project::detail::pathKey(path));
            // Each file is a row of the explorer's tree, not only a node of the model.
            EXPECT_TRUE(explorer.model()->indexOfPath(path).isValid()) << path;
        }
        std::ranges::sort(files);
        EXPECT_EQ(files, project::test::oraclePathKeys(*oracle)) << "item count per project equals the oracle";
        EXPECT_EQ(info.evaluation->source, source);
        const QString tip = explorer.model()->indexOf(info.node).data(Qt::ToolTipRole).toString();
        EXPECT_TRUE(tip.contains(QString::fromStdString(std::string(project::toString(source))))) << tip.toStdString();
    }
}

class WorkbenchProjectHost : public ::testing::Test {
protected:
    void SetUp() override {
        auto instance = project::test::machineMsBuild();
        if (!instance)
            GTEST_SKIP() << "no Visual Studio with MSBuild is installed (vswhere found none)";
        m_msbuild = instance->msbuildPath;
    }

    [[nodiscard]] RigOptions options(std::string hostPath) const {
        RigOptions options;
        options.workbench.projectHost = true;
        options.workbench.msbuildPath = m_msbuild;
        options.workbench.projectEditHostPath = std::move(hostPath);
        options.disableProjectEvaluation = true; // Only the explorer's evaluation, not compile commands.
        return options;
    }

    std::string m_msbuild;
};

TEST_F(WorkbenchProjectHost, TheExplorerShowsProjectHostItemsAndKillingTheHelperShowsTheBanner) {
    const auto host = frost::io::normalizePath(FROST_PROJECT_HOST);
    ASSERT_TRUE(host && frost::io::realFileSystem().exists(*host).value_or(false)) << FROST_PROJECT_HOST;
    WorkDir dir("project-host-explorer", kGenerated);
    WorkbenchRig rig(dir, options(*host));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Generated.sln")));
    auto* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    // Only the evaluation sees Deep.props' file (Directory.Build.props -> ... -> Deep.props).
    ASSERT_TRUE(waitFor(
        [&] {
            return finished(*rig.workbench) && everyProjectShownEvaluated(*explorer) &&
                   explorer->model()->indexOfPath(dir.join("shared/deep_shared.cpp")).isValid();
        },
        300s));
    const auto status = rig.workbench->solutionEvaluation()->status();
    EXPECT_EQ(status.source, project::EvaluationSource::ProjectHost);
    EXPECT_EQ(status.projectHostProblem, "");
    EXPECT_EQ(explorer->evaluationBanner(), QString());
    EXPECT_TRUE(explorer->evaluationBannerLabel()->isHidden());
    const auto active = rig.workbench->build().activeConfigurationIndex();
    ASSERT_TRUE(active.has_value());
    const std::string configuration = rig.workbench->build().configurations()[*active].name();
    expectExplorerMatchesTheOracle(*explorer, m_msbuild, configuration, project::EvaluationSource::ProjectHost, status);

    const auto pid = rig.workbench->solutionEvaluation()->hostPid();
    ASSERT_TRUE(pid.has_value());
    killProcess(*pid);
    ASSERT_TRUE(waitFor([&] { return !explorer->evaluationBanner().isEmpty(); }, 60s));
    EXPECT_TRUE(
        explorer->evaluationBanner().startsWith(QStringLiteral("ProjectHost is unavailable (ProjectHost ended")))
        << explorer->evaluationBanner().toStdString();
    EXPECT_FALSE(explorer->evaluationBannerLabel()->isHidden());
    // The items already evaluated stay; nothing is invented or dropped.
    EXPECT_TRUE(explorer->model()->indexOfPath(dir.join("shared/deep_shared.cpp")).isValid());
    EXPECT_TRUE(rig.workbench->closeProject());
    EXPECT_EQ(explorer->evaluationBanner(), QString());
}

TEST_F(WorkbenchProjectHost, WithoutTheHelperMsBuildExeEvaluatesAndTheBannerSaysSo) {
    WorkDir dir("project-host-missing", kGenerated);
    WorkbenchRig rig(dir, options(dir.join("no-helper/ProjectHostVsProbe.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Generated.slnx")));
    auto* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            return finished(*rig.workbench) && everyProjectShownEvaluated(*explorer) &&
                   explorer->model()->indexOfPath(dir.join("shared/deep_shared.cpp")).isValid();
        },
        300s));
    EXPECT_TRUE(explorer->evaluationBanner().startsWith(
        QStringLiteral("ProjectHost is unavailable (ProjectHost is not installed")))
        << explorer->evaluationBanner().toStdString();
    const auto active = rig.workbench->build().activeConfigurationIndex();
    ASSERT_TRUE(active.has_value());
    const std::string configuration = rig.workbench->build().configurations()[*active].name();
    expectExplorerMatchesTheOracle(*explorer, m_msbuild, configuration, project::EvaluationSource::MsBuildCli,
                                   rig.workbench->solutionEvaluation()->status());
    EXPECT_FALSE(rig.workbench->solutionEvaluation()->hostPid().has_value());
}

// Untrusted: nothing evaluates, nothing is claimed, no banner (trust has its own prompt).
TEST_F(WorkbenchProjectHost, AnUntrustedSolutionIsNotEvaluated) {
    const auto host = frost::io::normalizePath(FROST_PROJECT_HOST);
    ASSERT_TRUE(host.has_value());
    WorkDir dir("project-host-untrusted", kGenerated);
    WorkbenchRig rig(dir, options(*host));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Generated.sln")));
    auto* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    EXPECT_EQ(rig.workbench->solutionEvaluation(), nullptr);
    EXPECT_EQ(explorer->evaluationBanner(), QString());
    EXPECT_FALSE(explorer->model()->indexOfPath(dir.join("shared/deep_shared.cpp")).isValid());
    EXPECT_TRUE(explorer->model()->indexOfPath(dir.join("Core/src/a.cpp")).isValid()); // As written.
}

} // namespace
