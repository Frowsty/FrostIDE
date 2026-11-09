// SPDX-License-Identifier: Apache-2.0
//
// SLN-6's safe edits against this machine's real tools (Windows; labels "integration" and
// "msbuild"): the shipped ProjectHost (FROST_PROJECT_HOST) adds, renames and removes files of a
// scratch copy of the synthetic generated solution (fixtures/msvc_generated) through the MSBuild
// construction API, and adds and removes projects of its .slnx and .sln through SolutionPersistence.
// The oracles are git (`git diff -U0` of the definitions shows only the intended elements), MSBuild's
// own evaluation (msbuild_oracle.hpp: the items change exactly as intended) and MSBuild's build of
// the edited solution, which must pass. The pinned fixture itself is never edited.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/project_edit.hpp"
#include "frost/project/solution_model.hpp"

#include "msbuild_oracle.hpp"
#include "paths.hpp"
#include "project_edit_rig.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace frost::project {
namespace {

using test::EditScratch;

std::string hostPath() { return io::normalizePath(FROST_PROJECT_HOST).value_or(FROST_PROJECT_HOST); }

std::vector<std::string> sorted(std::vector<std::string> lines) {
    std::ranges::sort(lines);
    return lines;
}

// MSBuild's evaluated item paths (keys) of one project configuration.
std::set<std::string> oracleKeys(const std::string& msbuild, const std::string& project, const char* configuration,
                                 const char* platform) {
    auto oracle = test::runOracle(msbuild, ProjectEvaluationRequest{.projectPath = project,
                                                                    .configuration = configuration,
                                                                    .platform = platform,
                                                                    .solutionPath = {}});
    EXPECT_TRUE(oracle.has_value());
    const auto keys = oracle ? test::oraclePathKeys(*oracle) : std::vector<std::string>{};
    return {keys.begin(), keys.end()};
}

std::string key(const std::string& path) { return detail::pathKey(io::normalizePath(path).value_or(path)); }

bool exists(const std::string& path) { return io::realFileSystem().exists(path).value_or(false); }

void writeFile(const std::string& path, std::string_view content) {
    ASSERT_TRUE(io::realFileSystem().createDirectories(std::string(io::parentPath(path))).has_value()) << path;
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(path, content).has_value()) << path;
}

class ProjectEditReal : public ::testing::Test {
protected:
    void SetUp() override {
        auto instance = test::machineMsBuild();
        if (!instance) {
            GTEST_SKIP() << "no Visual Studio with MSBuild is installed (vswhere found none)";
        }
        m_msbuild = instance->msbuildPath;
        ASSERT_TRUE(exists(hostPath())) << hostPath() << " was not built (tools/build_project_host.py)";
    }

    // Builds `solution` (relative) in Debug|x64 with MSBuild.exe; the build must pass.
    void expectBuilds(const EditScratch& scratch, std::string_view solution) {
        const auto built = test::msbuild(m_msbuild, {scratch.join(solution), "-p:Configuration=Debug", "-p:Platform=x64"},
                                         scratch.root());
        EXPECT_EQ(built.exitCode, 0) << built.stdoutBytes << built.stderrBytes;
    }

    ProjectEdit fileEdit(const EditScratch& scratch, ProjectEditKind kind, std::string_view project) {
        ProjectEdit edit;
        edit.kind = kind;
        edit.root = scratch.root();
        edit.definition = scratch.join(project);
        edit.configuration = "Debug";
        edit.platform = "x64";
        return edit;
    }

    std::string m_msbuild;
};

TEST_F(ProjectEditReal, AddRenameAndRemoveChangeOnlyTheirElementsAndTheSolutionStillBuilds) {
    EditScratch scratch("sln6 file edits");
    ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("msvc_generated")));
    const std::string app = scratch.join("App/App.vcxproj");
    const std::string zeta = scratch.join("App/zeta.cpp");
    writeFile(zeta, "int zeta() { return 3; }\n");
    ASSERT_NO_FATAL_FAILURE(scratch.commit());
    const auto definitions = {std::string_view("App/App.vcxproj"), std::string_view("App/App.vcxproj.filters")};

    // Add: one item in the project, one in the filters file with its filter.
    auto before = oracleKeys(m_msbuild, app, "Debug", "x64");
    auto add = fileEdit(scratch, ProjectEditKind::AddFiles, "App/App.vcxproj");
    add.files = {zeta};
    add.filter = "Source Files";
    auto added = test::runEdit(hostPath(), m_msbuild, add);
    ASSERT_TRUE(added) << added.error().describe();
    EXPECT_EQ(sorted(scratch.changedLines(definitions)),
              sorted({"+    <ClCompile Include=\"zeta.cpp\" />", "+    <ClCompile Include=\"zeta.cpp\">",
                      "+      <Filter>Source Files</Filter>", "+    </ClCompile>"}));
    auto expected = before;
    expected.insert(key(zeta));
    EXPECT_EQ(oracleKeys(m_msbuild, app, "Debug", "x64"), expected);
    EXPECT_FALSE(exists(scratch.join(".frost-project-edit-journal")));
    expectBuilds(scratch, "Generated.sln");
    ASSERT_NO_FATAL_FAILURE(scratch.commit());

    // Rename (the file moved on disk first, as the IDE does): only the Include attributes change.
    const std::string renamed = scratch.join("App/more/app renamed.cpp");
    ASSERT_TRUE(io::realFileSystem().createDirectories(scratch.join("App/more")).has_value());
    std::filesystem::rename(*io::toWin32Path(scratch.join("App/app.cpp")), *io::toWin32Path(renamed));
    before = oracleKeys(m_msbuild, app, "Debug", "x64");
    auto rename = fileEdit(scratch, ProjectEditKind::RenameFiles, "App/App.vcxproj");
    rename.files = {scratch.join("App/app.cpp")};
    rename.renamedTo = {renamed};
    auto moved = test::runEdit(hostPath(), m_msbuild, rename);
    ASSERT_TRUE(moved) << moved.error().describe();
    EXPECT_EQ(sorted(scratch.changedLines(definitions)),
              sorted({"-    <ClCompile Include=\"app.cpp\" />", "+    <ClCompile Include=\"more\\app renamed.cpp\" />",
                      "-    <ClCompile Include=\"app.cpp\">", "+    <ClCompile Include=\"more\\app renamed.cpp\">"}));
    expected = before;
    expected.erase(key(scratch.join("App/app.cpp")));
    expected.insert(key(renamed));
    EXPECT_EQ(oracleKeys(m_msbuild, app, "Debug", "x64"), expected);
    expectBuilds(scratch, "Generated.sln");
    ASSERT_NO_FATAL_FAILURE(scratch.commit());

    // Remove two files of two item types; the Text item's group becomes empty and goes with it.
    before = oracleKeys(m_msbuild, app, "Release", "Win32");
    auto remove = fileEdit(scratch, ProjectEditKind::RemoveFiles, "App/App.vcxproj");
    remove.files = {zeta, scratch.join("App/notes.txt")};
    auto removed = test::runEdit(hostPath(), m_msbuild, remove);
    ASSERT_TRUE(removed) << removed.error().describe();
    EXPECT_EQ(sorted(scratch.changedLines(definitions)),
              sorted({"-    <ClCompile Include=\"zeta.cpp\" />", "-  <ItemGroup>", "-    <Text Include=\"notes.txt\" />",
                      "-  </ItemGroup>", "-    <ClCompile Include=\"zeta.cpp\">", "-      <Filter>Source Files</Filter>",
                      "-    </ClCompile>", "-  <ItemGroup>", "-    <Text Include=\"notes.txt\" />", "-  </ItemGroup>"}));
    expected = before;
    expected.erase(key(zeta));
    expected.erase(key(scratch.join("App/notes.txt")));
    EXPECT_EQ(oracleKeys(m_msbuild, app, "Release", "Win32"), expected);
    EXPECT_TRUE(exists(zeta)) << "removing from the project never deletes the file";
    expectBuilds(scratch, "Generated.sln");
}

TEST_F(ProjectEditReal, RefusesItemsTheEditCannotMeanExactlyAndWritesNothing) {
    EditScratch scratch("sln6 refusals");
    ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("msvc_generated")));
    writeFile(scratch.join("App/zeta.cpp"), "int zeta() { return 3; }\n");
    ASSERT_NO_FATAL_FAILURE(scratch.commit());
    const auto refused = [&](ProjectEdit edit, std::string_view reason, ErrorCode code = ErrorCode::IoError) {
        auto result = test::runEdit(hostPath(), m_msbuild, edit);
        ASSERT_FALSE(result) << reason;
        EXPECT_EQ(result.error().code(), code) << result.error().describe();
        EXPECT_NE(result.error().message().find(reason), std::string::npos) << result.error().describe();
    };
    // Core lists src\*.cpp (a wildcard) and debug_only.cpp only in Debug (a conditional item).
    auto wildcard = fileEdit(scratch, ProjectEditKind::RemoveFiles, "Core/Core.vcxproj");
    wildcard.files = {scratch.join("Core/src/a.cpp")};
    refused(wildcard, "wildcard");
    auto conditional = fileEdit(scratch, ProjectEditKind::RenameFiles, "Core/Core.vcxproj");
    conditional.files = {scratch.join("Core/debug_only.cpp")};
    conditional.renamedTo = {scratch.join("Core/src/a.cpp")};
    refused(conditional, "conditional item");
    auto twice = fileEdit(scratch, ProjectEditKind::AddFiles, "App/App.vcxproj");
    twice.files = {scratch.join("App/main.cpp")};
    refused(twice, "already includes main.cpp");
    auto filter = fileEdit(scratch, ProjectEditKind::AddFiles, "App/App.vcxproj");
    filter.files = {scratch.join("App/zeta.cpp")};
    filter.filter = "No Such Filter";
    refused(filter, "defines no filter");
    auto missing = fileEdit(scratch, ProjectEditKind::AddFiles, "App/App.vcxproj");
    missing.files = {scratch.join("App/missing.cpp")};
    refused(missing, "does not exist");
    // A Ready announcement that the caller does not admit writes nothing either.
    auto declined = fileEdit(scratch, ProjectEditKind::AddFiles, "App/App.vcxproj");
    declined.files = {scratch.join("App/zeta.cpp")};
    auto result = test::runEdit(hostPath(), m_msbuild, declined, false);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled) << result.error().describe();
    EXPECT_EQ(scratch.status(), "");
}

// A small static library the solution tests add (its own ProjectGuid; nothing references it).
constexpr std::string_view kExtraProject = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32"><Configuration>Debug</Configuration><Platform>Win32</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|Win32"><Configuration>Release</Configuration><Platform>Win32</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Profile|x64"><Configuration>Profile</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <ProjectGuid>{6A1D0C1E-0B0E-4C1B-9E51-0000000C0E09}</ProjectGuid>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ItemGroup>
    <ClCompile Include="extra.cpp" />
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
)";
constexpr std::string_view kExtraGuid = "{6A1D0C1E-0B0E-4C1B-9E51-0000000C0E09}";
constexpr std::string_view kRendererGuid = "{6A1D0C1E-0B0E-4C1B-9E51-0000000C0E02}";

std::vector<std::string> projectKeys(const std::string& solution) {
    auto model = SolutionModel::load(io::realFileSystem(), solution);
    EXPECT_TRUE(model.has_value()) << (model ? "" : model.error().describe());
    std::vector<std::string> keys;
    if (model) {
        for (const ProjectInfo& project : model->projects()) {
            keys.push_back(key(project.path));
        }
    }
    std::ranges::sort(keys);
    return keys;
}

// Every changed .sln line belongs to the project with `guid`: it names the GUID, or it is the
// project's one EndProject line or an emptied ProjectDependencies section of another project.
void expectOnlyProjectLines(const std::vector<std::string>& changed, std::string_view guid, char sign) {
    int endProject = 0;
    for (const std::string& line : changed) {
        ASSERT_EQ(line.front(), sign) << line;
        std::string text = line.substr(1);
        text.erase(0, text.find_first_not_of('\t'));
        if (text.find(guid) != std::string::npos) {
            continue;
        }
        if (text == "EndProject") {
            ++endProject;
            continue;
        }
        EXPECT_TRUE(text == "ProjectSection(ProjectDependencies) = postProject" || text == "EndProjectSection") << line;
    }
    EXPECT_EQ(endProject, 1);
}

TEST_F(ProjectEditReal, SolutionPersistenceAddsAndRemovesProjectsAndRefusesToRewriteASparseSln) {
    EditScratch scratch("sln6 solution edits");
    ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("msvc_generated")));
    writeFile(scratch.join("Extra/Extra.vcxproj"), kExtraProject);
    writeFile(scratch.join("Extra/extra.cpp"), "int extra() { return 9; }\n");
    /*
     * Full.sln is Generated.sln without its Profile configurations, so its configurations are the
     * full Debug/Release x x64/x86 matrix that SolutionPersistence writes back unchanged.
     * Generated.sln lacks Profile|x86, which SolutionPersistence would add (refused below).
     */
    {
        auto text = io::realFileSystem().readFile(scratch.join("Generated.sln"));
        ASSERT_TRUE(text);
        std::string full;
        std::istringstream in(*text);
        for (std::string line; std::getline(in, line);) {
            if (line.find("Profile|") == std::string::npos) {
                full += line + "\n";
            }
        }
        writeFile(scratch.join("Full.sln"), full);
    }
    ASSERT_NO_FATAL_FAILURE(scratch.commit());
    const auto edit = [&](ProjectEditKind kind, std::string_view solution, std::string_view project) {
        ProjectEdit out;
        out.kind = kind;
        out.root = scratch.root();
        out.definition = scratch.join(solution);
        out.project = scratch.join(project);
        return out;
    };
    const std::string renderer = key(scratch.join("Renderer/Renderer.vcxproj"));
    const std::string extra = key(scratch.join("Extra/Extra.vcxproj"));

    // .slnx: the project's element and the dependency on it go, nothing else.
    auto beforeProjects = projectKeys(scratch.join("Generated.slnx"));
    auto removed = test::runEdit(hostPath(), m_msbuild,
                                 edit(ProjectEditKind::RemoveProject, "Generated.slnx", "Renderer/Renderer.vcxproj"));
    ASSERT_TRUE(removed) << removed.error().describe();
    EXPECT_EQ(sorted(scratch.changedLines({"Generated.slnx"})),
              sorted({"-      <BuildDependency Project=\"Renderer/Renderer.vcxproj\" />",
                      "-    <Project Path=\"Renderer/Renderer.vcxproj\" Id=\"6a1d0c1e-0b0e-4c1b-9e51-0000000c0e02\">",
                      "-      <Build Solution=\"Profile|x86\" Project=\"false\" />", "-    </Project>"}));
    auto expectedProjects = beforeProjects;
    std::erase(expectedProjects, renderer);
    EXPECT_EQ(projectKeys(scratch.join("Generated.slnx")), expectedProjects);
    expectBuilds(scratch, "Generated.slnx");
    ASSERT_NO_FATAL_FAILURE(scratch.commit());

    auto added =
        test::runEdit(hostPath(), m_msbuild, edit(ProjectEditKind::AddProject, "Generated.slnx", "Extra/Extra.vcxproj"));
    ASSERT_TRUE(added) << added.error().describe();
    EXPECT_EQ(scratch.changedLines({"Generated.slnx"}),
              (std::vector<std::string>{"+  <Project Path=\"Extra/Extra.vcxproj\" Id=\"6a1d0c1e-0b0e-4c1b-9e51-0000000c0e09\" />"}));
    expectedProjects.push_back(extra);
    std::ranges::sort(expectedProjects);
    EXPECT_EQ(projectKeys(scratch.join("Generated.slnx")), expectedProjects);
    // Nothing references Extra: MSBuild builds it only because the solution now lists it.
    ASSERT_FALSE(exists(scratch.join("x64/Debug/Extra.lib")));
    expectBuilds(scratch, "Generated.slnx");
    EXPECT_TRUE(exists(scratch.join("x64/Debug/Extra.lib")));
    ASSERT_TRUE(io::realFileSystem().removeFile(scratch.join("x64/Debug/Extra.lib")).has_value());
    ASSERT_NO_FATAL_FAILURE(scratch.commit());

    // Generated.sln: SolutionPersistence would add Profile|x86; the edit is refused, the file stays.
    auto sparse = test::runEdit(hostPath(), m_msbuild,
                                edit(ProjectEditKind::RemoveProject, "Generated.sln", "Renderer/Renderer.vcxproj"));
    ASSERT_FALSE(sparse);
    EXPECT_NE(sparse.error().message().find("Profile|x86"), std::string::npos) << sparse.error().describe();
    EXPECT_TRUE(scratch.changedLines({"Generated.sln"}).empty());
    EXPECT_FALSE(exists(scratch.join(".frost-project-edit-journal")));

    // Full.sln: removing and adding change only the project's own lines.
    beforeProjects = projectKeys(scratch.join("Full.sln"));
    removed =
        test::runEdit(hostPath(), m_msbuild, edit(ProjectEditKind::RemoveProject, "Full.sln", "Renderer/Renderer.vcxproj"));
    ASSERT_TRUE(removed) << removed.error().describe();
    expectOnlyProjectLines(scratch.changedLines({"Full.sln"}), kRendererGuid, '-');
    expectedProjects = beforeProjects;
    std::erase(expectedProjects, renderer);
    EXPECT_EQ(projectKeys(scratch.join("Full.sln")), expectedProjects);
    expectBuilds(scratch, "Full.sln");
    ASSERT_NO_FATAL_FAILURE(scratch.commit());
    added = test::runEdit(hostPath(), m_msbuild, edit(ProjectEditKind::AddProject, "Full.sln", "Extra/Extra.vcxproj"));
    ASSERT_TRUE(added) << added.error().describe();
    expectOnlyProjectLines(scratch.changedLines({"Full.sln"}), kExtraGuid, '+');
    expectedProjects.push_back(extra);
    std::ranges::sort(expectedProjects);
    EXPECT_EQ(projectKeys(scratch.join("Full.sln")), expectedProjects);
    expectBuilds(scratch, "Full.sln");
    EXPECT_TRUE(exists(scratch.join("x64/Debug/Extra.lib")));
    // Adding a project the solution already lists is refused.
    auto again = test::runEdit(hostPath(), m_msbuild, edit(ProjectEditKind::AddProject, "Full.sln", "Extra/Extra.vcxproj"));
    ASSERT_FALSE(again);
    EXPECT_NE(again.error().message().find("already contains"), std::string::npos) << again.error().describe();
}

} // namespace
} // namespace frost::project
