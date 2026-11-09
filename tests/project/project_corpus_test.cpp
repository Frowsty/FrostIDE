// SPDX-License-Identifier: Apache-2.0
//
// SLN-2 and SLN-3 on R2a, the pinned Notepad++ clone (tests/corpus/corpora.lock.json, fetched by
// tools/corpora.py into <corpora root>/r2a-<sha12>): its Visual Studio solution evaluates through
// the shipped ProjectHost, and the Solution Explorer's model has, per project, exactly the items
// and imports this machine's MSBuild.exe reports (msbuild_oracle.hpp). Windows; label "corpus"
// (verify -Corpus only; tags integration, msbuild). The clone is only read: evaluation writes
// nothing into it.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/solution_model.hpp"

#include "msbuild_oracle.hpp"
#include "paths.hpp"
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

} // namespace
} // namespace frost::project
