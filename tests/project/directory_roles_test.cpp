// SPDX-License-Identifier: Apache-2.0
//
// Tests for the directory roles (frost/project/directory_roles.hpp, UX-23): each rule of the
// header comment on an in-memory tree (version-control folders, overrides, CMake preset binary
// directories from a real CMakePresets.json, the exact-case build names, CMakeCache.txt, MSBuild
// output folders only beside a project file, node_modules, Unity's generated folders only beside
// Assets and ProjectSettings), the effective role down a path, the watcher's
// belowUntraversed filter, and the override store's round trip through a real file.
#include "frost/io/file_system.hpp"
#include "frost/project/directory_roles.hpp"

#include "project_test_util.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace frost::project {
namespace {

using test::MemTree;

std::shared_ptr<const DirectoryRoles> loadRoles(MemTree& tree, std::vector<DirectoryRoleOverride> overrides = {}) {
    auto roles = DirectoryRoles::load(tree.fs, MemTree::root(), std::move(overrides));
    EXPECT_TRUE(roles.has_value()) << roles.error().describe();
    return roles ? *roles : nullptr;
}

TEST(DirectoryRoles, NamesRoundTrip) {
    for (const DirectoryRole role :
         {DirectoryRole::Source, DirectoryRole::Library, DirectoryRole::Generated, DirectoryRole::Excluded})
        EXPECT_EQ(parseDirectoryRole(directoryRoleName(role)), role);
    EXPECT_FALSE(parseDirectoryRole("Source").has_value());
    EXPECT_TRUE(isTraversed(DirectoryRole::Library));
    EXPECT_FALSE(isTraversed(DirectoryRole::Generated));
}

TEST(DirectoryRoles, DefaultsExcludeBuildFoldersAndVersionControlButNotSourceFoldersWithTheSameNames) {
    MemTree tree;
    tree.write("src/main.cpp", "");
    tree.write("build/windows-debug/CMakeCache.txt", "");
    tree.write("out/x.obj", "");
    tree.write(".cache/cpm/x.txt", "");
    tree.write("cmake-build-debug/x.txt", "");
    tree.write(".git/HEAD", "");
    tree.write(".vs/state.json", "");
    tree.write("Frost/src/Frost/Build/Export.cpp", "");         // FrostEngine's game export module.
    tree.write("Frost/src/Frost/Renderer/Debug/Stats.cpp", ""); // FrostEngine's renderer debug tools.
    tree.write("tools/bin/run.py", "");                         // No MSBuild project beside it.
    const auto roles = loadRoles(tree);
    ASSERT_NE(roles, nullptr);
    const auto decide = [&](std::string_view relative) { return roles->decide(MemTree::path(relative)); };
    EXPECT_EQ(decide("src"), (DirectoryRoleDecision{DirectoryRole::Source, DirectoryRoleReason::Default}));
    EXPECT_EQ(decide("build"), (DirectoryRoleDecision{DirectoryRole::Generated, DirectoryRoleReason::BuildName}));
    EXPECT_EQ(decide("out").role, DirectoryRole::Generated);
    EXPECT_EQ(decide(".cache").role, DirectoryRole::Generated);
    EXPECT_EQ(decide("cmake-build-debug").role, DirectoryRole::Generated);
    EXPECT_EQ(decide(".git"), (DirectoryRoleDecision{DirectoryRole::Excluded, DirectoryRoleReason::VersionControl}));
    EXPECT_EQ(decide(".vs").role, DirectoryRole::Excluded);
    EXPECT_TRUE(roles->traversed(MemTree::path("Frost/src/Frost/Build")));
    EXPECT_TRUE(roles->traversed(MemTree::path("Frost/src/Frost/Renderer/Debug")));
    EXPECT_TRUE(roles->traversed(MemTree::path("tools/bin")));
    // The root is Source; paths outside it are never claimed.
    EXPECT_EQ(roles->decide(MemTree::root()).role, DirectoryRole::Source);
    EXPECT_EQ(roles->decide(test::native("C:/elsewhere/build")).reason, DirectoryRoleReason::Default);
}

TEST(DirectoryRoles, CMakePresetBinaryDirectoriesAndCMakeCachesAreGenerated) {
    MemTree tree;
    tree.write("CMakeLists.txt", "project(x)\n");
    // A preset binaryDir with a name no name rule knows.
    tree.write("CMakePresets.json",
               R"({"version":6,"configurePresets":[
                   {"name":"base","hidden":true,"binaryDir":"${sourceDir}/artifacts/${presetName}"},
                   {"name":"dev","inherits":"base"}]})");
    tree.write("artifacts/dev/build.ninja", "");
    tree.write("elsewhere/CMakeCache.txt", ""); // Configured by hand, outside every preset.
    tree.write("src/a.cpp", "");
    const auto roles = loadRoles(tree);
    ASSERT_NE(roles, nullptr);
    EXPECT_TRUE(roles->presetsProblem().empty()) << roles->presetsProblem();
    EXPECT_EQ(roles->decide(MemTree::path("artifacts/dev")),
              (DirectoryRoleDecision{DirectoryRole::Generated, DirectoryRoleReason::CMakePreset}));
    EXPECT_EQ(roles->decide(MemTree::path("artifacts")).role, DirectoryRole::Source);
    EXPECT_EQ(roles->decide(MemTree::path("elsewhere")),
              (DirectoryRoleDecision{DirectoryRole::Generated, DirectoryRoleReason::CMakeCache}));
    EXPECT_EQ(roles->decide(MemTree::path("src")).role, DirectoryRole::Source);

    // A presets file that does not parse: no preset folders, the problem is reported.
    MemTree broken;
    broken.write("CMakePresets.json", "{");
    const auto fallback = loadRoles(broken);
    ASSERT_NE(fallback, nullptr);
    EXPECT_FALSE(fallback->presetsProblem().empty());
    EXPECT_TRUE(fallback->generatedDirectories().empty());
}

TEST(DirectoryRoles, MSBuildOutputFoldersAreGeneratedOnlyBesideAProjectFile) {
    MemTree tree;
    tree.write("App/App.vcxproj", "<Project/>");
    tree.write("App/x64/Debug/App.obj", "");
    tree.write("App/Debug/App.pdb", "");
    tree.write("App/obj/x.txt", "");
    tree.write("Tool/Tool.csproj", "<Project/>");
    tree.write("Tool/bin/Tool.dll", "");
    tree.write("App.sln", "");
    tree.write("x64/Release/App.exe", "");
    tree.write("Lib/Debug/Trace.cpp", ""); // Lib has no project file: a source folder.
    const auto roles = loadRoles(tree);
    ASSERT_NE(roles, nullptr);
    for (const char* generated : {"App/x64", "App/Debug", "App/obj", "Tool/bin", "x64"})
        EXPECT_EQ(roles->decide(MemTree::path(generated)),
                  (DirectoryRoleDecision{DirectoryRole::Generated, DirectoryRoleReason::MSBuildOutput}))
            << generated;
    EXPECT_EQ(roles->decide(MemTree::path("Lib/Debug")).role, DirectoryRole::Source);
}

// The old name excludes (io::defaultProjectExcludes) hid node_modules and Unity's root folders;
// with roles present those excludes are not applied, so the roles must hide them (review blocker).
TEST(DirectoryRoles, NodeModulesAndAUnityProjectsGeneratedFoldersAreGenerated) {
    MemTree tree;
    tree.write("node_modules/left-pad/index.js", "");
    tree.write("web/app/node_modules/react/index.js", "");
    tree.write("web/app/Node_Modules/notes.md", ""); // Not npm's spelling: a source folder.
    // A Unity project below the root (Unity writes no .gitignore).
    tree.write("Game/Assets/Player.cs", "");
    tree.write("Game/ProjectSettings/ProjectVersion.txt", "");
    for (const char* generated : {"Library/PackageCache/a.cs", "Temp/b.tmp", "Logs/c.log", "UserSettings/d.asset",
                                  "MemoryCaptures/e.snap", "obj/Debug/f.dll"})
        tree.write(std::string("Game/") + generated, "");
    // A C++ project's own Library and Logs folders: no Assets/ProjectSettings beside them.
    tree.write("Engine/Library/Core.cpp", "");
    tree.write("Engine/Logs/Logger.cpp", "");
    tree.write("Engine/Assets/icon.png", ""); // Assets alone is not a Unity project.
    const auto roles = loadRoles(tree);
    ASSERT_NE(roles, nullptr);
    const auto decide = [&](std::string_view relative) { return roles->decide(MemTree::path(relative)); };
    EXPECT_EQ(decide("node_modules"),
              (DirectoryRoleDecision{DirectoryRole::Generated, DirectoryRoleReason::DependencyName}));
    EXPECT_EQ(decide("web/app/node_modules").role, DirectoryRole::Generated);
    EXPECT_EQ(decide("web/app/Node_Modules").role, DirectoryRole::Source);
    for (const char* generated : {"Library", "Temp", "Logs", "UserSettings", "MemoryCaptures", "obj"})
        EXPECT_EQ(decide(std::string("Game/") + generated),
                  (DirectoryRoleDecision{DirectoryRole::Generated, DirectoryRoleReason::UnityProject}))
            << generated;
    EXPECT_EQ(decide("Game/Assets").role, DirectoryRole::Source);
    EXPECT_EQ(decide("Game/ProjectSettings").role, DirectoryRole::Source);
    EXPECT_EQ(decide("Engine/Library").role, DirectoryRole::Source);
    EXPECT_EQ(decide("Engine/Logs").role, DirectoryRole::Source);
    EXPECT_TRUE(roles->belowUntraversed(MemTree::path("Game/Library/PackageCache/a.cs")));
    EXPECT_TRUE(roles->belowUntraversed(MemTree::path("node_modules/left-pad/index.js")));
    EXPECT_FALSE(roles->belowUntraversed(MemTree::path("Engine/Library/Core.cpp")));

    // The Unity project at the root itself; an override still wins.
    MemTree unity;
    unity.write("Assets/Player.cs", "");
    unity.write("ProjectSettings/ProjectVersion.txt", "");
    unity.write("Library/ArtifactDB", "");
    unity.write("temp/x.tmp", ""); // ASCII case ignored, as the old [Tt]emp exclude.
    const auto unityRoles = loadRoles(unity, {{MemTree::path("Logs"), DirectoryRole::Source}});
    ASSERT_NE(unityRoles, nullptr);
    EXPECT_EQ(unityRoles->decide(MemTree::path("Library")).reason, DirectoryRoleReason::UnityProject);
    EXPECT_EQ(unityRoles->decide(MemTree::path("temp")).role, DirectoryRole::Generated);
    EXPECT_EQ(unityRoles->decide(MemTree::path("Logs")).role, DirectoryRole::Source);
}

TEST(DirectoryRoles, OverridesWinExceptOverVersionControlAndRolesFollowThePath) {
    MemTree tree;
    tree.write("build/generated/api.h", "");
    tree.write("ThirdParty/lua/lua.h", "");
    tree.write("docs/old/a.md", "");
    tree.write(".git/HEAD", "");
    const auto roles = loadRoles(tree, {{MemTree::path("build"), DirectoryRole::Source},
                                        {MemTree::path("ThirdParty"), DirectoryRole::Library},
                                        {MemTree::path("docs/old"), DirectoryRole::Excluded},
                                        {MemTree::path(".git"), DirectoryRole::Source},
                                        {test::native("C:/elsewhere"), DirectoryRole::Excluded}});
    ASSERT_NE(roles, nullptr);
    EXPECT_EQ(roles->decide(MemTree::path("build")),
              (DirectoryRoleDecision{DirectoryRole::Source, DirectoryRoleReason::Override}));
    EXPECT_EQ(roles->decide(MemTree::path(".git")).role, DirectoryRole::Excluded);
    EXPECT_EQ(roles->roleOf(MemTree::path("ThirdParty/lua")), DirectoryRole::Library);
    EXPECT_EQ(roles->roleOf(MemTree::path("docs/old")), DirectoryRole::Excluded);
    EXPECT_EQ(roles->roleOf(MemTree::path("docs")), DirectoryRole::Source);
    EXPECT_EQ(roles->overrides().size(), 4U); // The one outside the root is ignored.

    // The watcher's filter: strictly below an untraversed folder, never the folder itself.
    EXPECT_TRUE(roles->belowUntraversed(MemTree::path("docs/old/a.md")));
    EXPECT_FALSE(roles->belowUntraversed(MemTree::path("docs/old")));
    EXPECT_FALSE(roles->belowUntraversed(MemTree::path("build/generated/api.h")));
    EXPECT_TRUE(roles->belowUntraversed(MemTree::path(".git/HEAD")));
    EXPECT_FALSE(roles->belowUntraversed(MemTree::path("ThirdParty/lua/lua.h")));
    EXPECT_FALSE(roles->belowUntraversed(MemTree::root()));
}

#ifdef _WIN32
TEST(DirectoryRoles, WindowsPathsCompareWithoutCaseAndEitherSeparator) {
    MemTree tree;
    tree.write("Docs/Old/a.md", "");
    const auto roles = loadRoles(tree, {{"c:/W/docs/old", DirectoryRole::Excluded}});
    ASSERT_NE(roles, nullptr);
    EXPECT_EQ(roles->decide("C:\\w\\Docs\\Old").role, DirectoryRole::Excluded);
    EXPECT_TRUE(roles->belowUntraversed("C:/w/Docs/Old/a.md"));
}
#endif

TEST(DirectoryRoleStore, RoundTripsThroughTheFileAndKeepsOtherInstancesOverrides) {
    frost::test::ScratchDir scratch("roles");
    const std::string storePath = scratch.join(test::native("data/FrostIDE/directory-roles.txt"));
    const std::string project = scratch.join("project");
    const std::string vendor = scratch.join(test::native("project/vendor"));
    const std::string build = scratch.join(test::native("project/build"));
    const std::string other = scratch.join(test::native("other/gen"));
    auto first = DirectoryRoleStore::open(io::realFileSystem(), storePath);
    ASSERT_TRUE(first.has_value()) << first.error().describe();
    auto second = DirectoryRoleStore::open(io::realFileSystem(), storePath);
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE((*first)->set(vendor, DirectoryRole::Library).has_value());
    ASSERT_TRUE((*second)->set(build, DirectoryRole::Source).has_value()); // Re-reads: vendor is kept.
    ASSERT_TRUE((*first)->set(other, DirectoryRole::Excluded).has_value());
    auto content = io::realFileSystem().readFile(storePath);
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(*content, "# FrostIDE directory roles v1\nlibrary\t" + vendor + "\nsource\t" + build + "\nexcluded\t" +
                            other + "\n");

    auto reopened = DirectoryRoleStore::open(io::realFileSystem(), storePath);
    ASSERT_TRUE(reopened.has_value());
    const std::vector<DirectoryRoleOverride> expected{{build, DirectoryRole::Source}, {vendor, DirectoryRole::Library}};
    EXPECT_EQ((*reopened)->overridesFor(project), expected);
    ASSERT_TRUE((*reopened)->set(vendor, std::nullopt).has_value());
    EXPECT_EQ((*reopened)->overridesFor(project), (std::vector<DirectoryRoleOverride>{{build, DirectoryRole::Source}}));
    EXPECT_FALSE((*reopened)->set("relative/dir", DirectoryRole::Excluded).has_value());

    // Another version is refused and never rewritten; bad lines are skipped with a warning.
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(storePath, "# FrostIDE directory roles v9\n").has_value());
    auto newer = DirectoryRoleStore::open(io::realFileSystem(), storePath);
    ASSERT_FALSE(newer.has_value());
    EXPECT_EQ(newer.error().code(), ErrorCode::Unsupported);
    ASSERT_TRUE(io::realFileSystem()
                    .writeFileAtomic(storePath, "# FrostIDE directory roles v1\nweird\t" + vendor + "\nsource\n")
                    .has_value());
    auto tolerant = DirectoryRoleStore::open(io::realFileSystem(), storePath);
    ASSERT_TRUE(tolerant.has_value());
    EXPECT_EQ((*tolerant)->warnings().size(), 2U);
    EXPECT_TRUE((*tolerant)->overridesFor(project).empty());

    auto memory = DirectoryRoleStore::inMemory(io::realFileSystem());
    ASSERT_TRUE(memory->set(vendor, DirectoryRole::Excluded).has_value());
    EXPECT_EQ(memory->overridesFor(project).size(), 1U);
}

} // namespace
} // namespace frost::project
