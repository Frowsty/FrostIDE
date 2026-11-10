// SPDX-License-Identifier: Apache-2.0
//
// SLN-6 on R2a, the pinned Notepad++ clone (label "corpus"; tags integration, msbuild): a shallow
// git clone of the pin into a scratch directory (the pinned clone itself is only read) gets a
// source file renamed and another added through the shipped ProjectHost, and Lexilla removed from
// and added back to notepadPlus.sln through SolutionPersistence. Notepad++'s items live beside the
// solution ("..\src\..."), and its project has no .filters file. Oracles: git diff of the
// definitions, MSBuild's evaluation (msbuild_oracle.hpp), MSBuild compiling the renamed and the
// added file in the edited project (its ClCompile target with SelectedFiles, as Visual Studio's
// Compile command does), and frost's own solution reader.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/project_edit.hpp"
#include "frost/project/solution_model.hpp"

#include "msbuild_oracle.hpp"
#include "paths.hpp"
#include "project_edit_rig.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace frost::project {
namespace {

std::string pinnedR2a() {
    const std::string root = frost::test::environment("FROST_CORPORA_ROOT");
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus";
        return {};
    }
    const std::string sha = FROST_CORPUS_R2A_SHA;
    return io::normalizePath(root + "/r2a-" + sha.substr(0, 12)).value_or("");
}

std::string key(const std::string& path) { return detail::pathKey(io::normalizePath(path).value_or(path)); }

std::set<std::string> oracleKeys(const std::string& msbuild, const std::string& project) {
    auto oracle = test::runOracle(
        msbuild, {.projectPath = project, .configuration = "Release", .platform = "x64", .solutionPath = {}});
    EXPECT_TRUE(oracle.has_value());
    const auto keys = oracle ? test::oraclePathKeys(*oracle) : std::vector<std::string>{};
    return {keys.begin(), keys.end()};
}

std::vector<std::string> projectKeys(const std::string& solution) {
    auto model = SolutionModel::load(io::realFileSystem(), solution);
    EXPECT_TRUE(model.has_value()) << (model ? "" : model.error().describe());
    std::vector<std::string> keys;
    for (const ProjectInfo& project : model ? model->projects() : std::span<const ProjectInfo>{}) {
        keys.push_back(key(project.path));
    }
    std::ranges::sort(keys);
    return keys;
}

TEST(R2aCorpus, NotepadPlusPlusEditsChangeOnlyTheirLinesAndTheEditedFilesCompile) {
    const std::string pinned = pinnedR2a();
    ASSERT_FALSE(pinned.empty());
    ASSERT_TRUE(io::realFileSystem().exists(pinned + "\\PowerEditor\\visual.net\\notepadPlus.sln").value_or(false))
        << pinned << " is missing: python tools/corpora.py fetch --only r2a";
    auto instance = test::machineMsBuild();
    if (!instance) {
        GTEST_SKIP() << "no Visual Studio with MSBuild is installed (vswhere found none)";
    }
    const std::string msbuild = instance->msbuildPath;
    const std::string host = io::normalizePath(FROST_PROJECT_HOST).value_or(FROST_PROJECT_HOST);
    ASSERT_TRUE(io::realFileSystem().exists(host).value_or(false)) << host;

    test::EditScratch scratch("sln6 r2a");
    std::string url = "file:///" + pinned;
    std::ranges::replace(url, '\\', '/');
    ASSERT_EQ(scratch.git({"clone", "-q", "--depth", "1", "--no-tags", url, "."}).exitCode, 0);
    EXPECT_EQ(scratch.git({"rev-parse", "HEAD"}).stdoutBytes.substr(0, 40), FROST_CORPUS_R2A_SHA);
    const std::string visual = scratch.join("PowerEditor/visual.net");
    const std::string project = scratch.join("PowerEditor/visual.net/notepadPlus.vcxproj");
    const std::string solution = scratch.join("PowerEditor/visual.net/notepadPlus.sln");
    const auto edit = [&](ProjectEditKind kind) {
        ProjectEdit out;
        out.kind = kind;
        out.root = visual; // The solution's folder, the root the IDE trusts when it opens the .sln.
        out.definition =
            kind == ProjectEditKind::AddProject || kind == ProjectEditKind::RemoveProject ? solution : project;
        out.configuration = "Release";
        out.platform = "x64";
        return out;
    };
    const auto compiles = [&](std::string_view include) {
        const auto built = test::msbuild(msbuild,
                                         {project, "-t:ClCompile", "-p:SelectedFiles=" + std::string(include),
                                          "-p:Configuration=Release", "-p:Platform=x64"},
                                         visual);
        EXPECT_EQ(built.exitCode, 0) << built.stdoutBytes << built.stderrBytes;
        const std::string name(include.substr(include.find_last_of('\\') + 1));
        EXPECT_NE(built.stdoutBytes.find(name), std::string::npos)
            << "MSBuild compiled " << name << ": " << built.stdoutBytes;
    };

    // Rename a source the project lists beside the solution folder.
    const std::string oldSource = scratch.join("PowerEditor/src/MISC/Process/Processus.cpp");
    const std::string newSource = scratch.join("PowerEditor/src/MISC/Process/ProcessusRenamed.cpp");
    auto before = oracleKeys(msbuild, project);
    ASSERT_TRUE(before.contains(key(oldSource)));
    std::filesystem::rename(*io::toWin32Path(oldSource), *io::toWin32Path(newSource));
    auto rename = edit(ProjectEditKind::RenameFiles);
    rename.files = {oldSource};
    rename.renamedTo = {newSource};
    auto renamed = test::runEdit(host, msbuild, rename);
    ASSERT_TRUE(renamed) << renamed.error().describe();
    EXPECT_EQ(
        scratch.changedLines({"PowerEditor/visual.net"}),
        (std::vector<std::string>{"-    <ClCompile Include=\"..\\src\\MISC\\Process\\Processus.cpp\" />",
                                  "+    <ClCompile Include=\"..\\src\\MISC\\Process\\ProcessusRenamed.cpp\" />"}));
    auto expected = before;
    expected.erase(key(oldSource));
    expected.insert(key(newSource));
    EXPECT_EQ(oracleKeys(msbuild, project), expected);
    compiles("..\\src\\MISC\\Process\\ProcessusRenamed.cpp");
    ASSERT_NO_FATAL_FAILURE(scratch.commit());

    // Add a new source: one item line, placed by MSBuild among the project's ClCompile items.
    const std::string added = scratch.join("PowerEditor/src/MISC/Process/FrostAdded.cpp");
    {
        auto written = io::realFileSystem().writeFileAtomic(added, "int frostAdded() { return 6; }\n");
        ASSERT_TRUE(written.has_value());
    }
    auto add = edit(ProjectEditKind::AddFiles);
    add.files = {added};
    auto addedResult = test::runEdit(host, msbuild, add);
    ASSERT_TRUE(addedResult) << addedResult.error().describe();
    EXPECT_EQ(scratch.changedLines({"PowerEditor/visual.net"}),
              (std::vector<std::string>{"+    <ClCompile Include=\"..\\src\\MISC\\Process\\FrostAdded.cpp\" />"}));
    expected.insert(key(added));
    EXPECT_EQ(oracleKeys(msbuild, project), expected);
    compiles("..\\src\\MISC\\Process\\FrostAdded.cpp");
    ASSERT_NO_FATAL_FAILURE(scratch.commit());

    // Remove Lexilla from the solution and add it back: only Lexilla's lines change.
    const std::string lexilla = scratch.join("lexilla/src/Lexilla.vcxproj");
    const auto projects = projectKeys(solution);
    ASSERT_TRUE(std::ranges::contains(projects, key(lexilla)));
    auto removeProject = edit(ProjectEditKind::RemoveProject);
    removeProject.project = lexilla;
    auto removed = test::runEdit(host, msbuild, removeProject);
    ASSERT_TRUE(removed) << removed.error().describe();
    const auto removedLines = scratch.changedLines({"PowerEditor/visual.net/notepadPlus.sln"});
    ASSERT_FALSE(removedLines.empty());
    int endProject = 0;
    for (const std::string& line : removedLines) {
        EXPECT_EQ(line.front(), '-') << line;
        const bool lexillaLine = line.find("{E541C9BE-13BC-4CE6-A0A4-31145F51A2C1}") != std::string::npos;
        const bool end = line == "-EndProject";
        endProject += end ? 1 : 0;
        EXPECT_TRUE(lexillaLine || end || line == "-\tProjectSection(ProjectDependencies) = postProject" ||
                    line == "-\tEndProjectSection")
            << line;
    }
    EXPECT_EQ(endProject, 1);
    auto without = projects;
    std::erase(without, key(lexilla));
    EXPECT_EQ(projectKeys(solution), without);
    ASSERT_NO_FATAL_FAILURE(scratch.commit());
    auto addProject = edit(ProjectEditKind::AddProject);
    addProject.project = lexilla;
    auto readded = test::runEdit(host, msbuild, addProject);
    ASSERT_TRUE(readded) << readded.error().describe();
    EXPECT_EQ(projectKeys(solution), projects);
    for (const std::string& line : scratch.changedLines({"PowerEditor/visual.net/notepadPlus.sln"})) {
        EXPECT_TRUE(line.starts_with('+') &&
                    (line.find("{E541C9BE-13BC-4CE6-A0A4-31145F51A2C1}") != std::string::npos || line == "+EndProject"))
            << line;
    }
}

} // namespace
} // namespace frost::project
