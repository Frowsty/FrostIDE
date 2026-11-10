// SPDX-License-Identifier: Apache-2.0
//
// ProjectHost and the Solution Explorer's evaluated model against this machine's real MSBuild
// (SLN-0, SLN-2, SLN-3; Windows, labels "integration" and "msbuild"). The oracle is MSBuild.exe
// run by the test itself (msbuild_oracle.hpp); the product side is the shipped ProjectHost helper
// (FROST_PROJECT_HOST, built by tools/build_project_host.py) and SolutionEvaluation. The fixture is
// the synthetic generated solution, fixtures/msvc_generated, evaluated in place (evaluation writes
// nothing). A machine without Visual Studio skips the tests and says so.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/solution_model.hpp"

#include "msbuild_oracle.hpp"
#include "paths.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace frost::project {
namespace {

using namespace std::chrono_literals;
using test::fixture;

std::string generated(std::string_view relative = {}) {
    return relative.empty() ? fixture("msvc_generated")
                            : fixture(std::string("msvc_generated/") + std::string(relative));
}

std::string hostPath() { return io::normalizePath(FROST_PROJECT_HOST).value_or(FROST_PROJECT_HOST); }

// Ends a process the way a crash or Task Manager would: TerminateProcess from outside its owner.
void killProcess(io::ProcessId pid) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    ASSERT_NE(process, nullptr) << "OpenProcess " << GetLastError();
    EXPECT_TRUE(TerminateProcess(process, 9));
    EXPECT_EQ(WaitForSingleObject(process, 30000), WAIT_OBJECT_0);
    CloseHandle(process);
}

// Waits (bounded) until `done` holds for the evaluation's status.
template<class Predicate>
SolutionEvaluationStatus waitFor(const SolutionEvaluation& evaluation, Predicate done,
                                 std::chrono::seconds limit = std::chrono::seconds(600)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    SolutionEvaluationStatus status = evaluation.status();
    while (!done(status) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms); // Polling a status; no ordering depends on the period.
        status = evaluation.status();
    }
    EXPECT_TRUE(done(status)) << "the evaluation did not reach the expected state in time";
    return status;
}

bool finished(const SolutionEvaluationStatus& status) {
    return status.state == SolutionEvaluationState::Ready || status.state == SolutionEvaluationState::Stopped;
}

class ProjectHostReal : public ::testing::Test {
protected:
    void SetUp() override {
        auto instance = test::machineMsBuild();
        if (!instance) {
            GTEST_SKIP() << "no Visual Studio with MSBuild is installed (vswhere found none)";
        }
        m_msbuild = instance->msbuildPath;
        ASSERT_TRUE(io::realFileSystem().exists(hostPath()).value_or(false))
            << hostPath() << " was not built (tools/build_project_host.py)";
    }

    [[nodiscard]] std::shared_ptr<const SolutionModel> load(std::string_view name,
                                                            std::vector<EvaluatedProjectItems> evaluated = {}) const {
        auto model = SolutionModel::load(
            io::realFileSystem(), generated(name),
            {.cppHierarchy = CppHierarchyMode::Filters, .unloadedProjectPaths = {}, .evaluated = std::move(evaluated)});
        EXPECT_TRUE(model.has_value()) << (model ? "" : model.error().describe());
        return model ? std::make_shared<const SolutionModel>(std::move(*model)) : nullptr;
    }

    [[nodiscard]] SolutionEvaluationOptions options(std::string configuration) const {
        return {.hostPath = hostPath(),
                .msbuildPath = m_msbuild,
                .locateMsBuild = {},
                .configuration = std::move(configuration),
                .trusted = [] { return true; },
                .environment = {},
                .startTimeout = 120s,
                .requestTimeout = 300s,
                .afterProject = {}};
    }

    // The oracle's item paths for every evaluated project of `model` in `configuration`, keyed by
    // project path key, compared with the evaluated model's file nodes.
    void expectCountsEqualTheOracle(const SolutionModel& evaluatedModel, std::string_view configuration) {
        const auto index = evaluatedModel.findConfiguration(configuration);
        ASSERT_TRUE(index.has_value()) << configuration;
        for (std::size_t p = 0; p < evaluatedModel.projects().size(); ++p) {
            const ProjectId id{static_cast<std::uint32_t>(p)};
            const ProjectInfo& info = evaluatedModel.project(id);
            SCOPED_TRACE(info.name);
            const auto& mapping = evaluatedModel.projectConfiguration(id, *index);
            ASSERT_TRUE(mapping.has_value());
            ASSERT_TRUE(info.evaluation.has_value()) << "the project was not evaluated";
            EXPECT_EQ(info.evaluation->configuration, mapping->configuration);
            EXPECT_EQ(info.evaluation->platform, mapping->platform);
            auto oracle = test::runOracle(m_msbuild, {.projectPath = info.path,
                                                      .configuration = mapping->configuration,
                                                      .platform = mapping->platform,
                                                      .solutionPath = evaluatedModel.path()});
            ASSERT_TRUE(oracle.has_value());
            std::vector<std::string> shown;
            for (const std::string& path : evaluatedModel.projectFilePaths(id)) {
                shown.push_back(detail::pathKey(path));
            }
            std::ranges::sort(shown);
            const auto expected = test::oraclePathKeys(*oracle);
            EXPECT_EQ(shown.size(), expected.size()) << "item count per project equals the oracle";
            EXPECT_EQ(shown, expected);
            EXPECT_EQ(info.evaluation->itemCount, expected.size());
        }
    }

    std::string m_msbuild;
};

// SLN-0: the oracle itself produces item and flag lists from real MSBuild, and they show what
// only an evaluation can see in the synthetic solution.
TEST_F(ProjectHostReal, TheOracleProducesItemAndFlagListsFromRealMsBuild) {
    auto debug = test::runOracle(m_msbuild,
                                 {.projectPath = generated("Core/Core.vcxproj"),
                                  .configuration = "Debug",
                                  .platform = "x64",
                                  .solutionPath = generated("Generated.sln")},
                                 {"PreprocessorDefinitions", "AdditionalIncludeDirectories"}, {"GenToolchain"}, true);
    ASSERT_TRUE(debug.has_value());
    const auto find = [](const test::OracleEvaluation& e, std::string_view path) {
        return std::ranges::find_if(
            e.items, [&](const test::OracleItem& i) { return detail::pathKey(i.fullPath) == detail::pathKey(path); });
    };
    // Directory.Build.props -> Common -> Toolchain -> Platform.x64 -> Deep.props adds the shared file.
    const auto deep = find(*debug, generated("shared/deep_shared.cpp"));
    ASSERT_NE(deep, debug->items.end());
    EXPECT_EQ(deep->type, "ClCompile");
    const std::string& defines = deep->metadata.at("PreprocessorDefinitions");
    for (const char* define : {"GEN_DEEP=4", "GEN_X64=1", "GEN_COMMON=1", "GEN_ENGINE=1", "CORE_LIB=1"}) {
        EXPECT_NE(defines.find(define), std::string::npos) << define << " in " << defines;
    }
    EXPECT_EQ(defines.find("GEN_WIN32"), std::string::npos);
    EXPECT_NE(deep->metadata.at("AdditionalIncludeDirectories").find("shared"), std::string::npos);
    EXPECT_EQ(debug->properties.at("GenToolchain"), "msvc-x64");
    EXPECT_NE(find(*debug, generated("Core/debug_only.cpp")), debug->items.end());
    EXPECT_NE(find(*debug, generated("Core/src/b.cpp")), debug->items.end()); // The wildcard.
    EXPECT_TRUE(debug->imports.contains(detail::pathKey(generated("props/Deep.props"))));
    EXPECT_TRUE(debug->imports.contains(detail::pathKey(generated("Directory.Build.props"))));
    EXPECT_FALSE(debug->imports.contains(detail::pathKey(generated("props/Platform.Win32.props"))));

    auto release = test::runOracle(m_msbuild,
                                   {.projectPath = generated("Core/Core.vcxproj"),
                                    .configuration = "Release",
                                    .platform = "Win32",
                                    .solutionPath = generated("Generated.sln")},
                                   {"PreprocessorDefinitions"});
    ASSERT_TRUE(release.has_value());
    EXPECT_EQ(find(*release, generated("Core/debug_only.cpp")), release->items.end());
    const auto deepRelease = find(*release, generated("shared/deep_shared.cpp"));
    ASSERT_NE(deepRelease, release->items.end());
    EXPECT_NE(deepRelease->metadata.at("PreprocessorDefinitions").find("GEN_WIN32=1"), std::string::npos);
}

// SLN-2: the helper evaluates each project exactly as MSBuild.exe does: the same items in the
// same order, the same flags, and the import closure MSBuild's -pp shows.
TEST_F(ProjectHostReal, TheHelperEvaluatesEveryProjectAsMsBuildExeDoes) {
    auto client = ProjectHostClient::start(io::realProcessLauncher(), {.hostPath = hostPath(),
                                                                       .msbuildPath = m_msbuild,
                                                                       .environment = {},
                                                                       .startTimeout = 120s,
                                                                       .requestTimeout = 300s});
    ASSERT_TRUE(client.has_value()) << client.error().describe();
    EXPECT_EQ((*client)->hello().protocol, kProjectHostProtocol);
    EXPECT_FALSE((*client)->hello().msbuildVersion.empty());
    // The helper loaded the instance's Microsoft.Build (the locator), not another copy.
    EXPECT_TRUE(detail::isSameOrUnder(detail::normalizeAbsolute((*client)->hello().msbuildAssembly).value_or(""),
                                      std::string(io::parentPath(m_msbuild))))
        << (*client)->hello().msbuildAssembly;
    const std::vector<std::string> metadata{"PreprocessorDefinitions", "AdditionalIncludeDirectories"};
    for (const char* project : {"Core/Core.vcxproj", "Renderer/Renderer.vcxproj", "App/App.vcxproj"}) {
        for (const auto& [configuration, platform] :
             {std::pair{"Debug", "x64"}, std::pair{"Release", "Win32"}, std::pair{"Release", "x64"}}) {
            SCOPED_TRACE(std::string(project) + " " + configuration + "|" + platform);
            const ProjectEvaluationRequest request{.projectPath = generated(project),
                                                   .configuration = configuration,
                                                   .platform = platform,
                                                   .solutionPath = generated("Generated.sln"),
                                                   .itemTypes = {},
                                                   .metadata = metadata,
                                                   .properties = {"ConfigurationType"}};
            auto evaluated = (*client)->evaluate(request);
            ASSERT_TRUE(evaluated.has_value()) << evaluated.error().describe();
            auto oracle = test::runOracle(m_msbuild, request, metadata, {"ConfigurationType"}, true);
            ASSERT_TRUE(oracle.has_value());
            ASSERT_EQ(evaluated->items.size(), oracle->items.size());
            for (std::size_t i = 0; i < oracle->items.size(); ++i) {
                EXPECT_EQ(evaluated->items[i].type, oracle->items[i].type);
                EXPECT_EQ(detail::pathKey(detail::normalizeAbsolute(evaluated->items[i].fullPath).value_or("")),
                          detail::pathKey(oracle->items[i].fullPath));
                for (const std::string& name : metadata) {
                    EXPECT_EQ(evaluated->items[i].metadata.at(name), oracle->items[i].metadata.at(name)) << name;
                }
            }
            EXPECT_EQ(evaluated->properties.at("ConfigurationType"), oracle->properties.at("ConfigurationType"));
            std::set<std::string> imports;
            for (const std::string& import : evaluated->imports) {
                imports.insert(detail::pathKey(detail::normalizeAbsolute(import).value_or(import)));
            }
            EXPECT_EQ(imports, oracle->imports);
        }
    }
    // A project MSBuild cannot load is that request's error; the helper keeps serving.
    auto missing = (*client)->evaluate({.projectPath = generated("Missing/Missing.vcxproj"),
                                        .configuration = "Debug",
                                        .platform = "x64",
                                        .solutionPath = {}});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::IoError);
    EXPECT_TRUE((*client)->running());
}

// SLN-3: the Solution Explorer's model from the evaluated items has, per project, exactly the
// oracle's items, for .sln and .slnx and for a configuration that maps projects differently.
TEST_F(ProjectHostReal, ExplorerItemCountsEqualTheOracleForSlnAndSlnx) {
    for (const auto& [name, configuration] :
         {std::pair{"Generated.sln", "Debug|x64"}, std::pair{"Generated.slnx", "Debug|x64"},
          std::pair{"Generated.sln", "Profile|x64"}, std::pair{"Generated.slnx", "Release|x86"}}) {
        SCOPED_TRACE(std::string(name) + " " + configuration);
        auto model = load(name);
        ASSERT_NE(model, nullptr);
        SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model, options(configuration));
        const auto status = waitFor(evaluation, finished);
        EXPECT_EQ(status.state, SolutionEvaluationState::Ready);
        EXPECT_EQ(status.source, EvaluationSource::ProjectHost);
        EXPECT_EQ(status.projectHostProblem, "");
        EXPECT_EQ(status.msbuildProblem, "");
        EXPECT_EQ(status.evaluated, 3U);
        EXPECT_TRUE(status.warnings.empty()) << status.warnings.front();
        const auto results = evaluation.results();
        ASSERT_EQ(results.size(), 3U);
        for (const EvaluatedProjectItems& result : results) {
            EXPECT_EQ(result.source, EvaluationSource::ProjectHost);
            EXPECT_FALSE(result.imports.empty());
        }
        auto evaluatedModel = load(name, results);
        ASSERT_NE(evaluatedModel, nullptr);
        expectCountsEqualTheOracle(*evaluatedModel, configuration);
    }
}

// SLN-2: killing the helper mid-solution sends the rest to MSBuild.exe and says so.
TEST_F(ProjectHostReal, KillingTheHelperFallsBackToMsBuildExeAndSaysWhy) {
    auto model = load("Generated.sln");
    ASSERT_NE(model, nullptr);
    auto opts = options("Debug|x64");
    std::atomic<bool> killed{false};
    opts.afterProject = [&killed](std::size_t evaluated, std::optional<io::ProcessId> pid) {
        if (evaluated == 1 && pid) {
            killProcess(*pid); // After the first project, before the second request.
            killed.store(true);
        }
    };
    SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model, std::move(opts));
    const auto status = waitFor(
        evaluation, [](const SolutionEvaluationStatus& s) { return finished(s) && !s.projectHostProblem.empty(); });
    EXPECT_TRUE(killed.load());
    EXPECT_NE(status.projectHostProblem.find("ProjectHost ended"), std::string::npos) << status.projectHostProblem;
    EXPECT_EQ(status.source, EvaluationSource::MsBuildCli);
    EXPECT_EQ(status.msbuildProblem, "");
    EXPECT_FALSE(evaluation.hostPid().has_value());
    const auto results = evaluation.results();
    ASSERT_EQ(results.size(), 3U);
    EXPECT_EQ(results.front().source, EvaluationSource::ProjectHost);
    // The fallback evaluated the rest, with the same items as the oracle.
    EXPECT_TRUE(std::ranges::any_of(results, [](const auto& r) { return r.source == EvaluationSource::MsBuildCli; }));
    auto evaluatedModel = load("Generated.sln", results);
    ASSERT_NE(evaluatedModel, nullptr);
    expectCountsEqualTheOracle(*evaluatedModel, "Debug|x64");
}

// SLN-2: an idle helper that is killed is reported too (the banner must not wait for a request).
TEST_F(ProjectHostReal, KillingTheIdleHelperIsReported) {
    auto model = load("Generated.slnx");
    ASSERT_NE(model, nullptr);
    SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model, options("Release|x64"));
    const auto ready = waitFor(evaluation, finished);
    ASSERT_EQ(ready.source, EvaluationSource::ProjectHost);
    ASSERT_TRUE(evaluation.hostPid().has_value());
    killProcess(*evaluation.hostPid());
    const auto status =
        waitFor(evaluation, [](const SolutionEvaluationStatus& s) { return !s.projectHostProblem.empty(); });
    EXPECT_NE(status.projectHostProblem.find("ProjectHost ended"), std::string::npos);
    EXPECT_EQ(status.source, EvaluationSource::MsBuildCli);
    EXPECT_EQ(evaluation.results().size(), 3U); // The items already shown stay valid.
}

// SLN-2: without the helper, MSBuild.exe evaluates everything and the reason is given.
TEST_F(ProjectHostReal, AMissingHelperFallsBackToMsBuildExe) {
    auto model = load("Generated.sln");
    ASSERT_NE(model, nullptr);
    auto opts = options("Debug|x86");
    opts.hostPath = generated("no-such-dir/ProjectHostVsProbe.exe");
    SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model, std::move(opts));
    const auto status = waitFor(evaluation, finished);
    EXPECT_NE(status.projectHostProblem.find("not installed"), std::string::npos) << status.projectHostProblem;
    EXPECT_EQ(status.source, EvaluationSource::MsBuildCli);
    const auto results = evaluation.results();
    ASSERT_EQ(results.size(), 3U);
    auto evaluatedModel = load("Generated.sln", results);
    ASSERT_NE(evaluatedModel, nullptr);
    expectCountsEqualTheOracle(*evaluatedModel, "Debug|x86");
}

// The truth rule: no MSBuild, or no trust, evaluates nothing and says why.
TEST_F(ProjectHostReal, WithoutMsBuildOrTrustNothingIsEvaluated) {
    auto model = load("Generated.sln");
    ASSERT_NE(model, nullptr);
    {
        auto opts = options("Debug|x64");
        opts.msbuildPath.clear();
        opts.locateMsBuild = []() -> Result<std::string> { return fail(ErrorCode::NotFound, "no Visual Studio"); };
        SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model, std::move(opts));
        const auto status = waitFor(evaluation, finished);
        EXPECT_NE(status.msbuildProblem.find("MSBuild unavailable: no Visual Studio"), std::string::npos);
        EXPECT_FALSE(status.source.has_value());
        EXPECT_TRUE(evaluation.results().empty());
        EXPECT_FALSE(evaluation.hostPid().has_value());
    }
    {
        auto opts = options("Debug|x64");
        opts.trusted = [] { return false; };
        SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model, std::move(opts));
        const auto status = waitFor(evaluation, finished);
        EXPECT_NE(status.msbuildProblem.find("not trusted"), std::string::npos);
        EXPECT_TRUE(evaluation.results().empty());
        EXPECT_FALSE(evaluation.hostPid().has_value());
    }
}

// Stopping ends the helper and the worker promptly.
TEST_F(ProjectHostReal, StopEndsTheHelper) {
    auto model = load("Generated.sln");
    ASSERT_NE(model, nullptr);
    std::optional<io::ProcessId> pid;
    {
        SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model, options("Debug|x64"));
        waitFor(evaluation, finished);
        pid = evaluation.hostPid();
        ASSERT_TRUE(pid.has_value());
        evaluation.stopAndJoin();
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, *pid);
    if (process != nullptr) { // The pid may already be gone entirely.
        EXPECT_EQ(WaitForSingleObject(process, 10000), WAIT_OBJECT_0) << "the helper outlived its evaluation";
        CloseHandle(process);
    }
}

} // namespace
} // namespace frost::project
