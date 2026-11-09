// SPDX-License-Identifier: Apache-2.0
//
// SLN-2 and SLN-3 on R2a, the pinned Notepad++ clone (tests/corpus/corpora.lock.json, fetched by
// tools/corpora.py into <corpora root>/r2a-<sha12>): its Visual Studio solution evaluates through
// the shipped ProjectHost, and the Solution Explorer's model has, per project, exactly the items
// and imports this machine's MSBuild.exe reports (msbuild_oracle.hpp). SLN-4 and SLN-5: the warm
// open from the ProjectModelCache and GetClCommandLines against MSBuild.exe's. Windows; label "corpus"
// (verify -Corpus only; tags integration, msbuild). The clone is only read: evaluation writes
// nothing into it.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/project_model_cache.hpp"
#include "frost/project/solution_model.hpp"

#include "cl_command_lines_oracle.hpp"
#include "msbuild_oracle.hpp"
#include "paths.hpp"
#include "support/scratch_dir.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace frost::project {
namespace {

using namespace std::chrono_literals;

// <FROST_CORPORA_ROOT>/r2a-<sha12>, or "" (a test failure) when the corpus tier did not set the root.
std::string r2aRoot() {
    const std::string root = frost::test::environment("FROST_CORPORA_ROOT");
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus";
        return {};
    }
    const std::string sha = FROST_CORPUS_R2A_SHA;
    return io::normalizePath(root + "/r2a-" + sha.substr(0, 12)).value_or("");
}

TEST(R2aCorpus, NotepadPlusPlusEvaluatesThroughProjectHostAndItemCountsEqualTheOracle) {
    const std::string root = r2aRoot();
    ASSERT_FALSE(root.empty());
    const std::string solution = io::normalizePath(root + "/PowerEditor/visual.net/notepadPlus.sln").value_or("");
    ASSERT_TRUE(io::realFileSystem().exists(solution).value_or(false))
        << solution << " is missing: python tools/corpora.py fetch --only r2a";
    auto instance = test::machineMsBuild();
    if (!instance) {
        GTEST_SKIP() << "no Visual Studio with MSBuild is installed (vswhere found none)";
    }
    const std::string host = io::normalizePath(FROST_PROJECT_HOST).value_or(FROST_PROJECT_HOST);
    ASSERT_TRUE(io::realFileSystem().exists(host).value_or(false)) << host;

    for (const char* configuration : {"Debug|x64", "Release|Win32"}) {
        SCOPED_TRACE(configuration);
        auto loaded = SolutionModel::load(io::realFileSystem(), solution);
        ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
        auto model = std::make_shared<const SolutionModel>(std::move(*loaded));
        ASSERT_EQ(model->projects().size(), 3U); // Notepad++, Lexilla, Scintilla.
        SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model,
                                      {.hostPath = host,
                                       .msbuildPath = instance->msbuildPath,
                                       .locateMsBuild = {},
                                       .configuration = configuration,
                                       .trusted = [] { return true; },
                                       .environment = {},
                                       .startTimeout = 120s,
                                       .requestTimeout = 600s,
                                       .afterProject = {}});
        const auto deadline = std::chrono::steady_clock::now() + 20min;
        auto status = evaluation.status();
        while (status.state != SolutionEvaluationState::Ready && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(50ms); // Polling a status; nothing depends on the period.
            status = evaluation.status();
        }
        ASSERT_EQ(status.state, SolutionEvaluationState::Ready);
        EXPECT_EQ(status.source, EvaluationSource::ProjectHost) << status.projectHostProblem;
        EXPECT_EQ(status.projectHostProblem, "");
        EXPECT_EQ(status.msbuildProblem, "");
        EXPECT_TRUE(status.warnings.empty()) << status.warnings.front();
        const auto results = evaluation.results();
        ASSERT_EQ(results.size(), 3U);
        auto evaluated = SolutionModel::load(
            io::realFileSystem(), solution,
            {.cppHierarchy = CppHierarchyMode::Filters, .unloadedProjectPaths = {}, .evaluated = results});
        ASSERT_TRUE(evaluated.has_value()) << evaluated.error().describe();
        const auto index = evaluated->findConfiguration(configuration);
        ASSERT_TRUE(index.has_value());
        for (std::size_t p = 0; p < evaluated->projects().size(); ++p) {
            const ProjectId id{static_cast<std::uint32_t>(p)};
            const ProjectInfo& info = evaluated->project(id);
            SCOPED_TRACE(info.name);
            ASSERT_TRUE(info.evaluation.has_value());
            const auto& mapping = evaluated->projectConfiguration(id, *index);
            ASSERT_TRUE(mapping.has_value());
            auto oracle = test::runOracle(instance->msbuildPath,
                                          {.projectPath = info.path,
                                           .configuration = mapping->configuration,
                                           .platform = mapping->platform,
                                           .solutionPath = solution},
                                          {}, {}, true);
            ASSERT_TRUE(oracle.has_value());
            std::vector<std::string> shown;
            for (const std::string& path : evaluated->projectFilePaths(id)) {
                shown.push_back(detail::pathKey(path));
            }
            std::ranges::sort(shown);
            const auto expected = test::oraclePathKeys(*oracle);
            EXPECT_FALSE(expected.empty());
            EXPECT_EQ(shown.size(), expected.size()) << "item count per project equals the oracle";
            EXPECT_EQ(shown, expected);
            std::set<std::string> imports;
            for (const std::string& import : info.evaluation->imports) {
                imports.insert(detail::pathKey(import));
            }
            EXPECT_EQ(imports, oracle->imports);
        }
    }
}

/*
 * SLN-4 and SLN-5 on R2a: a warm open from the ProjectModelCache (in a scratch directory; the
 * clone is only read) is faster than the cold one and has the same items and GetClCommandLines,
 * and each project's command lines, include paths and sources in Debug|x64 are exactly what
 * MSBuild.exe's GetClCommandLines reports (cl_command_lines_oracle.hpp).
 */
TEST(R2aCorpus, AWarmOpenIsFasterWithTheSameModelAndCommandLinesEqualMsBuildExe) {
    const std::string root = r2aRoot();
    ASSERT_FALSE(root.empty());
    const std::string solution = io::normalizePath(root + "/PowerEditor/visual.net/notepadPlus.sln").value_or("");
    ASSERT_TRUE(io::realFileSystem().exists(solution).value_or(false))
        << solution << " is missing: python tools/corpora.py fetch --only r2a";
    auto instance = test::machineMsBuild();
    if (!instance) {
        GTEST_SKIP() << "no Visual Studio with MSBuild is installed (vswhere found none)";
    }
    const std::string host = io::normalizePath(FROST_PROJECT_HOST).value_or(FROST_PROJECT_HOST);
    ASSERT_TRUE(io::realFileSystem().exists(host).value_or(false)) << host;
    auto loaded = SolutionModel::load(io::realFileSystem(), solution);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    auto model = std::make_shared<const SolutionModel>(std::move(*loaded));
    frost::test::ScratchDir scratch("r2a-model-cache");
    auto cache = std::make_shared<ProjectModelCache>(io::realFileSystem(), scratch.join("cache"));
    const auto options = [&] {
        SolutionEvaluationOptions out{.hostPath = host,
                                      .msbuildPath = instance->msbuildPath,
                                      .locateMsBuild = {},
                                      .configuration = "Debug|x64",
                                      .trusted = [] { return true; },
                                      .environment = {},
                                      .startTimeout = 120s,
                                      .requestTimeout = 600s,
                                      .afterProject = {}};
        out.cache = cache;
        out.clCommandLines = true;
        return out;
    };
    const auto untilReady = [](const SolutionEvaluation& evaluation) {
        const auto deadline = std::chrono::steady_clock::now() + 20min;
        auto status = evaluation.status();
        while (status.state != SolutionEvaluationState::Ready && status.state != SolutionEvaluationState::Stopped &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms); // Polling a status; the period bounds the timing error.
            status = evaluation.status();
        }
        return status;
    };

    std::vector<EvaluatedProjectItems> coldItems;
    std::vector<ProjectClCommandLines> coldLines;
    std::chrono::steady_clock::duration cold{};
    {
        const auto started = std::chrono::steady_clock::now();
        SolutionEvaluation evaluation(io::realProcessLauncher(), io::realFileSystem(), model, options());
        const auto status = untilReady(evaluation);
        cold = std::chrono::steady_clock::now() - started;
        ASSERT_EQ(status.state, SolutionEvaluationState::Ready);
        EXPECT_EQ(status.source, EvaluationSource::ProjectHost) << status.projectHostProblem;
        EXPECT_EQ(status.clCommandLinesProblem, "");
        EXPECT_EQ(status.cached, 0U);
        coldItems = evaluation.results();
        coldLines = evaluation.clCommandLines();
    }
    ASSERT_EQ(coldItems.size(), 3U);
    ASSERT_EQ(coldLines.size(), 3U);
    const auto started = std::chrono::steady_clock::now();
    SolutionEvaluation warm(io::realProcessLauncher(), io::realFileSystem(), model, options());
    const auto status = untilReady(warm);
    const auto took = std::chrono::steady_clock::now() - started;
    ASSERT_EQ(status.state, SolutionEvaluationState::Ready);
    EXPECT_EQ(status.cached, 3U);
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
        }
    }
    EXPECT_EQ(warm.clCommandLines(), coldLines);

    const auto index = model->findConfiguration("Debug|x64");
    ASSERT_TRUE(index.has_value());
    for (const ProjectClCommandLines& product : coldLines) {
        SCOPED_TRACE(product.projectPath);
        auto oracle = test::runClCommandLinesOracle(instance->msbuildPath, {.projectPath = product.projectPath,
                                                                            .configuration = product.configuration,
                                                                            .platform = product.platform,
                                                                            .solutionPath = solution,
                                                                            .itemTypes = {},
                                                                            .metadata = {},
                                                                            .properties = {}});
        ASSERT_TRUE(oracle.has_value());
        EXPECT_FALSE(oracle->commandLines.empty());
        EXPECT_EQ(product.commandLines, oracle->commandLines);
        EXPECT_EQ(product.includePath, oracle->includePath);
        EXPECT_EQ(product.externalIncludePath, oracle->externalIncludePath);
        EXPECT_EQ(clangdCommands(product), clangdCommands(*oracle)) << "clangd's entries";
    }
    // Revalidation (through ProjectHost) ends; it may only replace what really changed.
    const auto deadline = std::chrono::steady_clock::now() + 20min;
    while (warm.status().revalidating && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_FALSE(warm.status().revalidating);
    EXPECT_EQ(warm.status().revalidated, 3U);
}

} // namespace
} // namespace frost::project
