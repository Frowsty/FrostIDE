// SPDX-License-Identifier: Apache-2.0
//
// SLN-4, SLN-5 and SLN-7 against this machine's real MSBuild (Windows; labels "integration" and
// "msbuild"), on the synthetic generated solution (fixtures/msvc_generated):
//  - ProjectHost's GetClCommandLines equals MSBuild.exe's (cl_command_lines_oracle.hpp) for every
//    project in Debug|x64 and Release|x64;
//  - a warm open from the ProjectModelCache is faster than the cold one and serves the same items
//    and command lines, revalidation confirms them, and changing an import evaluates again;
//  - a Configuration Manager mapping edit (writeSolutionConfigurationEdits) builds in MSBuild.exe
//    with the mapped configuration, for .sln and .slnx.
// Tests that change files work on a scratch copy of the fixture.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/project_model_cache.hpp"
#include "frost/project/solution_configuration_edit.hpp"
#include "frost/project/solution_model.hpp"

#include "cl_command_lines_oracle.hpp"
#include "msbuild_oracle.hpp"
#include "paths.hpp"
#include "project_test_util.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace frost::project {
namespace {

using namespace std::chrono_literals;
using test::fixture;

std::string hostPath() { return io::normalizePath(FROST_PROJECT_HOST).value_or(FROST_PROJECT_HOST); }

// A scratch copy of fixtures/msvc_generated; removed with the test.
class GeneratedCopy {
public:
    GeneratedCopy() : m_scratch("gcl-generated") {
        std::error_code error;
        std::filesystem::copy(std::filesystem::path(fixture("msvc_generated")), std::filesystem::path(m_scratch.path()),
                              std::filesystem::copy_options::recursive, error);
        EXPECT_FALSE(error) << error.message();
    }
    [[nodiscard]] std::string join(std::string_view relative) const {
        return io::normalizePath(m_scratch.join(test::native(relative))).value_or("");
    }

private:
    frost::test::ScratchDir m_scratch;
};

std::shared_ptr<const SolutionModel> loadSolution(const std::string& path) {
    auto model = SolutionModel::load(io::realFileSystem(), path);
    EXPECT_TRUE(model.has_value()) << (model ? "" : model.error().describe());
    return model ? std::make_shared<const SolutionModel>(std::move(*model)) : nullptr;
}

ProjectId projectNamed(const SolutionModel& model, std::string_view name) {
    for (std::size_t p = 0; p < model.projects().size(); ++p) {
        if (model.projects()[p].name == name) {
            return ProjectId{static_cast<std::uint32_t>(p)};
        }
    }
    ADD_FAILURE() << "no project " << name;
    return ProjectId{0};
}

bool hasToken(const ProjectClCommandLines& lines, std::string_view source, std::string_view token) {
    for (const ClangdCompileCommand& command : clangdCommands(lines)) {
        if (detail::pathKey(command.file) == detail::pathKey(source)) {
            return std::ranges::find(command.arguments, token) != command.arguments.end();
        }
    }
    ADD_FAILURE() << source << " has no command line";
    return false;
}

class ClCommandLinesReal : public ::testing::Test {
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

    [[nodiscard]] SolutionEvaluationOptions options(std::string configuration,
                                                    std::shared_ptr<ProjectModelCache> cache = {}) const {
        SolutionEvaluationOptions out{.hostPath = hostPath(),
                                      .msbuildPath = m_msbuild,
                                      .locateMsBuild = {},
                                      .configuration = std::move(configuration),
                                      .trusted = [] { return true; },
                                      .environment = {},
                                      .startTimeout = 120s,
                                      .requestTimeout = 300s,
                                      .afterProject = {}};
        out.cache = std::move(cache);
        out.clCommandLines = true;
        return out;
    }

    // Waits (bounded) for Ready; returns the status and how long it took from `started`.
    static std::pair<SolutionEvaluationStatus, std::chrono::steady_clock::duration>
    untilReady(const SolutionEvaluation& evaluation, std::chrono::steady_clock::time_point started) {
        const auto deadline = started + 10min;
        SolutionEvaluationStatus status = evaluation.status();
        while (status.state != SolutionEvaluationState::Ready && status.state != SolutionEvaluationState::Stopped &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms); // Polling a status; the period bounds the timing error.
            status = evaluation.status();
        }
        const auto took = std::chrono::steady_clock::now() - started;
        EXPECT_EQ(status.state, SolutionEvaluationState::Ready);
        return {status, took};
    }

    static SolutionEvaluationStatus untilRevalidated(const SolutionEvaluation& evaluation) {
        const auto deadline = std::chrono::steady_clock::now() + 10min;
        SolutionEvaluationStatus status = evaluation.status();
        while ((status.state != SolutionEvaluationState::Ready || status.revalidating) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
            status = evaluation.status();
        }
        EXPECT_FALSE(status.revalidating) << "revalidation did not end";
        return status;
    }

    std::string m_msbuild;
};

// SLN-5: ProjectHost's GetClCommandLines is MSBuild.exe's, project by project, for Debug|x64 and
// Release|x64; the two differ where the projects say they do (NDEBUG, the runtime, debug_only.cpp).
TEST_F(ClCommandLinesReal, ProjectHostGetClCommandLinesEqualsMsBuildExeForDebugAndRelease) {
    auto client = ProjectHostClient::start(io::realProcessLauncher(), {.hostPath = hostPath(),
                                                                       .msbuildPath = m_msbuild,
                                                                       .environment = {},
                                                                       .startTimeout = 120s,
                                                                       .requestTimeout = 300s});
    ASSERT_TRUE(client.has_value()) << client.error().describe();
    const auto model = loadSolution(fixture("msvc_generated/Generated.sln"));
    ASSERT_NE(model, nullptr);
    std::map<std::string, ProjectClCommandLines> core;
    for (const char* configuration : {"Debug|x64", "Release|x64"}) {
        SCOPED_TRACE(configuration);
        const auto index = model->findConfiguration(configuration);
        ASSERT_TRUE(index.has_value());
        for (std::size_t p = 0; p < model->projects().size(); ++p) {
            const ProjectId id{static_cast<std::uint32_t>(p)};
            const ProjectInfo& info = model->project(id);
            if (info.nodeKind != NodeKind::Project) {
                continue;
            }
            SCOPED_TRACE(info.name);
            const auto& mapping = model->projectConfiguration(id, *index);
            ASSERT_TRUE(mapping.has_value());
            const ProjectEvaluationRequest request{.projectPath = info.path,
                                                   .configuration = mapping->configuration,
                                                   .platform = mapping->platform,
                                                   .solutionPath = model->path(),
                                                   .itemTypes = {},
                                                   .metadata = {},
                                                   .properties = {}};
            auto product = (*client)->clCommandLines(request);
            ASSERT_TRUE(product.has_value()) << product.error().describe();
            auto oracle = test::runClCommandLinesOracle(m_msbuild, request);
            ASSERT_TRUE(oracle.has_value());
            EXPECT_FALSE(oracle->commandLines.empty());
            EXPECT_FALSE(oracle->includePath.empty());
            EXPECT_EQ(product->commandLines, oracle->commandLines);
            EXPECT_EQ(product->includePath, oracle->includePath);
            EXPECT_EQ(product->externalIncludePath, oracle->externalIncludePath);
            EXPECT_EQ(detail::pathKey(product->projectPath), detail::pathKey(oracle->projectPath));
            if (info.name == "Core") {
                core.emplace(configuration, *product);
            }
        }
    }
    ASSERT_EQ(core.size(), 2U);
    const std::string a = fixture("msvc_generated/Core/src/a.cpp");
    EXPECT_FALSE(hasToken(core.at("Debug|x64"), a, "NDEBUG"));
    EXPECT_TRUE(hasToken(core.at("Release|x64"), a, "NDEBUG"));
    EXPECT_TRUE(hasToken(core.at("Debug|x64"), a, "/MDd"));
    EXPECT_TRUE(hasToken(core.at("Release|x64"), a, "/MD"));
}

// SLN-4: the warm open serves every project from the cache, is faster than the cold one, and has
// the same items and command lines; revalidation through ProjectHost confirms them unchanged.
TEST_F(ClCommandLinesReal, AWarmOpenIsFasterThanAColdOneAndServesTheSameModel) {
    GeneratedCopy copy;
    const auto model = loadSolution(copy.join("Generated.sln"));
    ASSERT_NE(model, nullptr);
    auto cache = std::make_shared<ProjectModelCache>(io::realFileSystem(), copy.join("cache"));
    std::vector<EvaluatedProjectItems> coldItems;
    std::vector<ProjectClCommandLines> coldLines;
    std::chrono::steady_clock::duration cold{};
    {
        const auto started = std::chrono::steady_clock::now();
        SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model,
                                      options("Debug|x64", cache));
        const auto [status, took] = untilReady(evaluation, started);
        cold = took;
        EXPECT_EQ(status.cached, 0U);
        EXPECT_EQ(status.source, EvaluationSource::ProjectHost) << status.projectHostProblem;
        EXPECT_EQ(status.clCommandLinesProblem, "");
        EXPECT_EQ(status.total, 3U);
        coldItems = evaluation.results();
        coldLines = evaluation.clCommandLines();
    }
    ASSERT_EQ(coldItems.size(), 3U);
    ASSERT_EQ(coldLines.size(), 3U);
    const auto started = std::chrono::steady_clock::now();
    SolutionEvaluation warm(io::realProcessLauncher(), io::realFileSystem(), model, options("Debug|x64", cache));
    const auto [status, took] = untilReady(warm, started);
    EXPECT_EQ(status.cached, 3U) << "every project came from the cache";
    EXPECT_LT(took, cold) << "warm " << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                          << " ms, cold " << std::chrono::duration_cast<std::chrono::milliseconds>(cold).count()
                          << " ms";
    const auto warmItems = warm.results();
    ASSERT_EQ(warmItems.size(), coldItems.size());
    for (std::size_t i = 0; i < coldItems.size(); ++i) {
        SCOPED_TRACE(coldItems[i].projectPath);
        EXPECT_EQ(warmItems[i].projectPath, coldItems[i].projectPath);
        EXPECT_EQ(warmItems[i].imports, coldItems[i].imports);
        ASSERT_EQ(warmItems[i].items.size(), coldItems[i].items.size());
        for (std::size_t j = 0; j < coldItems[i].items.size(); ++j) {
            EXPECT_EQ(warmItems[i].items[j].path, coldItems[i].items[j].path);
            EXPECT_EQ(warmItems[i].items[j].type, coldItems[i].items[j].type);
            EXPECT_EQ(warmItems[i].items[j].include, coldItems[i].items[j].include);
            EXPECT_EQ(warmItems[i].items[j].fromWildcard, coldItems[i].items[j].fromWildcard);
        }
    }
    EXPECT_EQ(warm.clCommandLines(), coldLines);
    // Revalidation through ProjectHost finds the cached model current: nothing is replaced.
    const auto revalidated = untilRevalidated(warm);
    EXPECT_EQ(revalidated.revalidated, 3U);
    EXPECT_EQ(revalidated.source, EvaluationSource::ProjectHost) << revalidated.projectHostProblem;
    const auto after = warm.results();
    ASSERT_EQ(after.size(), warmItems.size());
    for (std::size_t i = 0; i < after.size(); ++i) {
        ASSERT_EQ(after[i].items.size(), warmItems[i].items.size()) << after[i].projectPath;
        for (std::size_t j = 0; j < after[i].items.size(); ++j) {
            EXPECT_EQ(after[i].items[j].path, warmItems[i].items[j].path);
        }
        EXPECT_EQ(after[i].imports, warmItems[i].imports);
    }
    EXPECT_EQ(warm.clCommandLines(), coldLines);
}

// SLN-4: an edited property sheet invalidates every project that imports it, and the next open
// evaluates them again, with the edit in their command lines.
TEST_F(ClCommandLinesReal, ChangingAnImportEvaluatesAgainOnTheNextOpen) {
    GeneratedCopy copy;
    const auto model = loadSolution(copy.join("Generated.sln"));
    ASSERT_NE(model, nullptr);
    auto cache = std::make_shared<ProjectModelCache>(io::realFileSystem(), copy.join("cache"));
    {
        SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model,
                                      options("Debug|x64", cache));
        (void)untilReady(evaluation, std::chrono::steady_clock::now());
    }
    auto& fs = io::realFileSystem();
    const std::string sheet = copy.join("props/Common.props");
    std::string text = fs.readFile(sheet).value();
    const std::string from = "GEN_COMMON=1;";
    ASSERT_NE(text.find(from), std::string::npos);
    text.replace(text.find(from), from.size(), "GEN_COMMON=1;GEN_EDITED=1;");
    std::this_thread::sleep_for(20ms); // A later modification time than the cached stamp, on any file system.
    ASSERT_TRUE(fs.writeFileAtomic(sheet, text).has_value());
    SolutionEvaluation again(io::realProcessLauncher(), io::realFileSystem(), model, options("Debug|x64", cache));
    const auto [status, took] = untilReady(again, std::chrono::steady_clock::now());
    (void)took;
    EXPECT_EQ(status.cached, 0U) << "every project imports Common.props";
    const auto lines = again.clCommandLines();
    ASSERT_EQ(lines.size(), 3U);
    for (const ProjectClCommandLines& project : lines) {
        SCOPED_TRACE(project.projectPath);
        ASSERT_FALSE(project.commandLines.empty());
        EXPECT_NE(project.commandLines[0].commandLine.find("/D GEN_EDITED=1"), std::string::npos);
    }
}

// SLN-7: a mapping edited through writeSolutionConfigurationEdits is what MSBuild.exe builds: the
// solution's Debug|x64 builds Core as Release|x64 (MSBuild's own "->" line and the library land in
// x64\Release), for the .sln and the .slnx.
TEST_F(ClCommandLinesReal, AnEditedMappingBuildsInMsBuildWithTheMappedConfiguration) {
    for (const char* name : {"Generated.sln", "Generated.slnx"}) {
        SCOPED_TRACE(name);
        GeneratedCopy copy;
        const auto model = loadSolution(copy.join(name));
        ASSERT_NE(model, nullptr);
        const auto debug = model->findConfiguration("Debug|x64");
        ASSERT_TRUE(debug.has_value());
        const ProjectConfigurationMapping release{
            .configuration = "Release", .platform = "x64", .build = true, .deploy = false};
        const ConfigurationMappingEdit edit{
            .project = projectNamed(*model, "Core"), .solutionConfiguration = *debug, .mapping = release};
        auto written = writeSolutionConfigurationEdits(io::realFileSystem(), *model, std::span(&edit, 1));
        ASSERT_TRUE(written.has_value()) << written.error().describe();

        io::ProcessSpec spec;
        spec.executable = m_msbuild;
        spec.arguments = {copy.join(name), "-nologo",         "-nodeReuse:false",       "-m:1",
                          "-v:m",          "-t:Engine\\Core", "-p:Configuration=Debug", "-p:Platform=x64"};
        spec.workingDirectory = std::string(io::parentPath(copy.join(name)));
        spec.environment = {{"MSBUILDDISABLENODEREUSE", "1"}, {"MSBUILDTERMINALLOGGER", "off"}};
        auto launched = io::realProcessLauncher().launch(spec);
        ASSERT_TRUE(launched.has_value()) << launched.error().describe();
        auto ran = (*launched)->communicate({}, std::chrono::minutes(10));
        ASSERT_TRUE(ran.has_value()) << ran.error().describe();
        ASSERT_EQ(ran->exitCode, 0) << ran->stdoutBytes << ran->stderrBytes;
        const std::string releaseLib = copy.join("x64/Release/Core.lib");
        EXPECT_NE(ran->stdoutBytes.find("Core.vcxproj -> "), std::string::npos) << ran->stdoutBytes;
        EXPECT_NE(ran->stdoutBytes.find(R"(\x64\Release\Core.lib)"), std::string::npos) << ran->stdoutBytes;
        EXPECT_TRUE(io::realFileSystem().exists(releaseLib).value_or(false));
        EXPECT_FALSE(io::realFileSystem().exists(copy.join("x64/Debug/Core.lib")).value_or(true))
            << "Core was built as Debug";
    }
}

} // namespace
} // namespace frost::project
