// SPDX-License-Identifier: Apache-2.0
//
// SLN-9 and SLN-11 on R2a, the pinned Notepad++ clone (tests/corpus/corpora.lock.json, fetched by
// tools/corpora.py into <corpora root>/r2a-<sha12>), with this machine's real MSBuild:
//   - Compile (Ctrl+F7) of single sources of the Notepad++ project through BuildService: the object
//     of the file is rebuilt and no other object of the project changes; the error list equals what
//     MSBuild's own file loggers record for the same compile (msbuild_diagnostic_oracle.hpp);
//   - the notepadPlus.vcxproj.user FrostIDE writes is what MSBuild evaluates (-getProperty).
// Windows; label "corpus" (verify -Corpus only; tags integration, msbuild). The clone's tracked
// files are never changed: compiles write to its build output (x64\Debug, ignored by its
// .gitignore), and the .user file (ignored too) is removed again, or restored when one existed.

#include "frost/build/build_service.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/build/run_service.hpp"
#include "frost/build/vcxproj_user.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/trust_store.hpp"

#include "build/msbuild_diagnostic_oracle.hpp"
#include "support/scratch_dir.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::build::BuildAction;

constexpr auto kCompileTimeout = std::chrono::minutes(10);

// <FROST_CORPORA_ROOT>/r2a-<sha12>, or "" (a test failure) when the corpus tier did not set the root.
std::string r2aRoot() {
    const std::string root = frost::test::environment("FROST_CORPORA_ROOT");
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus";
        return {};
    }
    const std::string sha = FROST_CORPUS_R2A_SHA;
    return frost::io::normalizePath(root + "/r2a-" + sha.substr(0, 12)).value_or("");
}

std::optional<frost::build::VsInstance> machineMsBuild() {
    auto found = frost::build::findMsBuild(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    if (!found) {
        if (found.error().code() != ErrorCode::NotFound) {
            ADD_FAILURE() << found.error().describe();
        }
        return std::nullopt;
    }
    return *found;
}

// The last-write time of every .obj in `directory` (not recursive), by lower-cased name.
std::map<std::string, std::int64_t> objectTimes(const std::string& directory) {
    std::map<std::string, std::int64_t> times;
    auto entries = frost::io::realFileSystem().listDirectory(directory);
    if (!entries) {
        return times;
    }
    for (const frost::io::DirEntry& entry : *entries) {
        std::string name = entry.name;
        for (char& c : name) {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
        if (entry.type != frost::io::FileType::Regular || !name.ends_with(".obj")) {
            continue;
        }
        auto status = frost::io::realFileSystem().status(frost::io::joinPath(directory, entry.name).value_or(""));
        if (status) {
            times[name] = status->mtime.time_since_epoch().count();
        }
    }
    return times;
}

// The Notepad++ project of the clone's solution, with a BuildService on it (Debug|x64, trusted).
class R2aBuild : public ::testing::Test {
protected:
    void SetUp() override {
        root = r2aRoot();
        ASSERT_FALSE(root.empty());
        solutionPath = frost::io::normalizePath(root + "/PowerEditor/visual.net/notepadPlus.sln").value_or("");
        ASSERT_TRUE(fs.exists(solutionPath).value_or(false))
            << solutionPath << " is missing: python tools/corpora.py fetch --only r2a";
        msbuild = machineMsBuild();
        if (!msbuild) {
            GTEST_SKIP() << "no Visual Studio with MSBuild is installed (vswhere found none)";
        }
        projectPath = frost::io::normalizePath(root + "/PowerEditor/visual.net/notepadPlus.vcxproj").value_or("");
        auto loaded = frost::project::SolutionModel::load(fs, solutionPath);
        ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
        solution = std::make_shared<const frost::project::SolutionModel>(std::move(*loaded));
        auto opened = frost::project::TrustStore::open(fs, scratch.path() + "\\trusted-roots.txt");
        ASSERT_TRUE(opened.has_value()) << opened.error().describe();
        store = std::move(*opened);
        ASSERT_TRUE(store->trust(root).has_value());
        service = frost::build::BuildService::create(
            fs, frost::io::realProcessLauncher(), *store,
            {.stateFilePath = scratch.path() + "\\active-configurations.txt", .msbuildPath = msbuild->msbuildPath});
        ASSERT_TRUE(service->openSolution(solution).has_value());
        ASSERT_TRUE(service->setActiveConfiguration("Debug|x64").has_value());
    }

    void TearDown() override { service.reset(); }

    frost::io::IFileSystem& fs = frost::io::realFileSystem();
    frost::test::ScratchDir scratch{"r2a-build"};
    std::string root;
    std::string solutionPath;
    std::string projectPath;
    std::optional<frost::build::VsInstance> msbuild;
    std::shared_ptr<const frost::project::SolutionModel> solution;
    std::unique_ptr<frost::project::TrustStore> store;
    std::unique_ptr<frost::build::BuildService> service;
};

// Ctrl+F7 on two Notepad++ sources: SortLocale.cpp and AboutDlg.cpp. Each compile rebuilds the
// file's object only, and the error list equals the oracle's record of the same compile.
TEST_F(R2aBuild, CompileRebuildsOnlyTheFileAndTheErrorListEqualsMsBuildsLog) {
    const std::string intDir = frost::io::normalizePath(root + "/PowerEditor/visual.net/x64/Debug").value_or("");
    struct Case {
        std::string source;   // Below the clone.
        std::string identity; // As notepadPlus.vcxproj writes it: the oracle's own SelectedFiles.
        std::string object;
    };
    const std::vector<Case> cases = {
        {"PowerEditor/src/MISC/Common/SortLocale.cpp", "..\\src\\MISC\\Common\\SortLocale.cpp", "sortlocale.obj"},
        {"PowerEditor/src/WinControls/AboutDlg/AboutDlg.cpp", "..\\src\\WinControls\\AboutDlg\\AboutDlg.cpp",
         "aboutdlg.obj"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.source);
        const std::string file = frost::io::normalizePath(root + "/" + c.source).value_or("");
        const auto project = service->compileProjectFor(file);
        ASSERT_TRUE(project.has_value());
        EXPECT_EQ(solution->project(*project).name, "Notepad++");

        // Without its object MSBuild's up-to-date check cannot skip the file (the clone may hold
        // objects of an earlier build, which the compile must leave alone).
        const std::string object = frost::io::joinPath(intDir, c.object).value_or("");
        static_cast<void>(fs.removeFile(object));
        std::map<std::string, std::int64_t> before = objectTimes(intDir);
        ASSERT_TRUE(service->compile(file).has_value());
        ASSERT_TRUE(service->waitForIdle(kCompileTimeout)) << "the compile did not finish in time";
        const auto outcome = service->lastOutcome();
        ASSERT_TRUE(outcome.has_value());
        EXPECT_FALSE(outcome->error.has_value()) << outcome->error->describe();
        EXPECT_EQ(outcome->target.action, BuildAction::Compile);
        const std::set<frost::test::LoggedDiagnostic> shown = frost::test::errorListRows(service->diagnostics());
        std::map<std::string, std::int64_t> after = objectTimes(intDir);
        EXPECT_EQ(after.contains(c.object), outcome->result.success) << "the object exists if the compile succeeded";
        after.erase(c.object);
        EXPECT_EQ(after, before) << "no other object was compiled";

        static_cast<void>(fs.removeFile(object));
        const auto oracle = frost::test::runMsBuildOracle(
            msbuild->msbuildPath,
            {projectPath, "-t:ClCompile", "-p:SelectedFiles=" + c.identity, "-p:Configuration=Debug", "-p:Platform=x64",
             "-p:SolutionDir=" + std::string(frost::io::parentPath(solutionPath)) + "\\",
             "-p:SolutionPath=" + solutionPath, "-p:SolutionName=notepadPlus", "-p:SolutionFileName=notepadPlus.sln",
             "-p:SolutionExt=.sln"},
            std::string(frost::io::parentPath(projectPath)), scratch.path());
        ASSERT_TRUE(oracle.has_value());
        EXPECT_EQ(outcome->result.success, oracle->exitCode == 0);
        EXPECT_EQ(shown, oracle->diagnostics) << "error list:" << frost::test::describe(shown)
                                              << "\nMSBuild logged:" << frost::test::describe(oracle->diagnostics);
        std::cout << c.source << ": " << oracle->diagnostics.size() << " diagnostic(s), compile "
                  << (outcome->result.success ? "succeeded" : "failed") << "\n";
    }
}

// SLN-11 on the corpus: the .vcxproj.user written for Debug|x64 is what MSBuild evaluates, and the
// clone is left as it was.
TEST_F(R2aBuild, TheUserFileFrostIdeWritesIsWhatMsBuildEvaluates) {
    const std::string user = projectPath + ".user";
    const std::optional<std::string> original =
        fs.exists(user).value_or(false) ? std::optional<std::string>(fs.readFile(user).value_or("")) : std::nullopt;
    const auto restore = [&] {
        if (original) {
            EXPECT_TRUE(fs.writeFileAtomic(user, *original).has_value());
        } else {
            EXPECT_TRUE(fs.removeFile(user).has_value());
        }
    };
    auto written = frost::build::writeUserFileProperties(
        fs, projectPath, "Debug", "x64",
        {{"LocalDebuggerCommandArguments", "-multiInst -nosession \"$(ProjectDir)..\\..\\README.md\""},
         {"LocalDebuggerWorkingDirectory", "$(SolutionDir)"}});
    ASSERT_TRUE(written.has_value()) << written.error().describe();
    frost::build::RunPropertyRequest request{.msbuildPath = msbuild->msbuildPath,
                                             .projectPath = projectPath,
                                             .solutionPath = solutionPath,
                                             .configuration = "Debug",
                                             .platform = "x64"};
    auto properties = frost::build::queryRunProperties(frost::io::realProcessLauncher(), request, kCompileTimeout);
    request.configuration = "Release";
    auto release = frost::build::queryRunProperties(frost::io::realProcessLauncher(), request, kCompileTimeout);
    restore();
    ASSERT_TRUE(properties.has_value()) << properties.error().describe();
    const std::string projectDir(frost::io::parentPath(projectPath));
    EXPECT_EQ(properties->commandArguments, "-multiInst -nosession \"" + projectDir + "\\..\\..\\README.md\"");
    EXPECT_EQ(properties->workingDirectory, projectDir + "\\");
    ASSERT_TRUE(release.has_value()) << release.error().describe();
    EXPECT_TRUE(release->commandArguments.empty()) << release->commandArguments;
    EXPECT_EQ(fs.exists(user).value_or(true), original.has_value());
}

} // namespace
