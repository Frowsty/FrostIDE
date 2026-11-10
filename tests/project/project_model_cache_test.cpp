// SPDX-License-Identifier: Apache-2.0
//
// ProjectModelCache (SLN-4) on an in-memory tree with a manual clock: an entry round-trips, and
// every input the header lists invalidates it: the project file, an import, a file a glob could
// match, the global properties, the MSBuild identity and the environment; damaged and foreign
// entries read as misses. Label "unit"; the real MSBuild runs in cl_command_lines_real_test.cpp.

#include "frost/project/project_model_cache.hpp"

#include "paths.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace frost::project {
namespace {

using test::MemTree;

class ProjectModelCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        tree.write("App/App.vcxproj", "<Project/>");
        tree.write("props/Common.props", "<Project/>");
        tree.write("App/src/a.cpp", "int a;");
        tree.write("App/src/b.cpp", "int b;");
        tree.write("App/include/deep/x.h", "");
    }

    [[nodiscard]] ProjectModelCacheKey key() const {
        return {.solutionPath = MemTree::path("G.sln"),
                .projectPath = MemTree::path("App/App.vcxproj"),
                .configuration = "Debug",
                .platform = "x64"};
    }

    [[nodiscard]] static EvaluationInputs inputs() {
        return {.globalProperties = {{"Configuration", "Debug"}, {"Platform", "x64"}},
                .msbuildIdentity = "msbuild|1|2",
                .environment = "env:1"};
    }

    [[nodiscard]] static CachedProjectModel model() {
        CachedProjectModel out;
        out.items.projectPath = MemTree::path("App/App.vcxproj");
        out.items.configuration = "Debug";
        out.items.platform = "x64";
        out.items.msbuildVersion = "17.14.1";
        out.items.items = {ProjectItem{.type = ItemType::ClCompile,
                                       .itemName = "ClCompile",
                                       .include = "src\\*.cpp",
                                       .path = MemTree::path("App/src/a.cpp"),
                                       .fromWildcard = false}};
        out.items.imports = {MemTree::path("props/Common.props")};
        out.clCommandLines = ProjectClCommandLines{.projectPath = MemTree::path("App/App.vcxproj"),
                                                   .configuration = "Debug",
                                                   .platform = "x64",
                                                   .commandLines = {{.commandLine = R"(/c /I"x y" /D A=1)",
                                                                     .files = {MemTree::path("App/src/a.cpp")},
                                                                     .workingDirectory = MemTree::path("App"),
                                                                     .configurationOptions = true}},
                                                   .includePath = {MemTree::path("vc/include")},
                                                   .externalIncludePath = {}};
        return out;
    }

    // A cache with the entry stored, globs "src\*.cpp" and "include\**\*.h".
    ProjectModelCache stored() {
        ProjectModelCache cache(tree.fs, MemTree::path("cache"));
        const std::vector<std::string> globs{"src\\*.cpp", "include\\**\\*.h"};
        EXPECT_TRUE(cache.store(key(), inputs(), model(), globs).has_value());
        return cache;
    }

    void later() { tree.clock.advance(std::chrono::seconds(1)); }

    MemTree tree;
};

TEST_F(ProjectModelCacheTest, AStoredEntryReadsBackUnchanged) {
    ProjectModelCache cache = stored();
    std::string why;
    auto hit = cache.lookup(key(), inputs(), &why);
    ASSERT_TRUE(hit.has_value()) << why;
    const CachedProjectModel expected = model();
    EXPECT_EQ(hit->items.projectPath, expected.items.projectPath);
    EXPECT_EQ(hit->items.msbuildVersion, "17.14.1");
    ASSERT_EQ(hit->items.items.size(), 1U);
    EXPECT_EQ(hit->items.items[0].path, expected.items.items[0].path);
    EXPECT_EQ(hit->items.items[0].type, ItemType::ClCompile);
    EXPECT_EQ(hit->items.items[0].include, "src\\*.cpp");
    EXPECT_EQ(hit->items.imports, expected.items.imports);
    EXPECT_EQ(hit->clCommandLines, expected.clCommandLines);
    // A different key is a different entry.
    auto other = key();
    other.configuration = "Release";
    EXPECT_FALSE(cache.lookup(other, inputs(), &why).has_value());
    EXPECT_EQ(why, "no entry");
}

TEST_F(ProjectModelCacheTest, ChangingAnImportOrTheProjectInvalidatesTheEntry) {
    {
        ProjectModelCache cache = stored();
        later();
        tree.write("props/Common.props", "<Project><!-- edited --></Project>");
        std::string why;
        EXPECT_FALSE(cache.lookup(key(), inputs(), &why).has_value());
        EXPECT_EQ(why, "Common.props changed");
    }
    {
        ProjectModelCache cache = stored();
        later();
        tree.write("App/App.vcxproj", "<Project />"); // Same size? No: one byte longer, and later.
        std::string why;
        EXPECT_FALSE(cache.lookup(key(), inputs(), &why).has_value());
        EXPECT_EQ(why, "App.vcxproj changed");
    }
    {
        ProjectModelCache cache = stored();
        ASSERT_TRUE(tree.fs.removeFile(MemTree::path("props/Common.props")).has_value());
        EXPECT_FALSE(cache.lookup(key(), inputs()).has_value()) << "a deleted import";
    }
}

TEST_F(ProjectModelCacheTest, AFileAGlobCouldMatchInvalidatesTheEntry) {
    {
        ProjectModelCache cache = stored();
        tree.write("App/src/c.cpp", "int c;");
        std::string why;
        EXPECT_FALSE(cache.lookup(key(), inputs(), &why).has_value());
        EXPECT_TRUE(why.starts_with("the files below")) << why;
    }
    {
        ProjectModelCache cache = stored();
        tree.write("App/include/deep/deeper/y.h", ""); // "**" walks the whole subtree.
        EXPECT_FALSE(cache.lookup(key(), inputs()).has_value());
    }
    {
        // Editing a matched file's content changes no name: still valid.
        ProjectModelCache cache = stored();
        later();
        tree.write("App/src/a.cpp", "int a = 1;");
        EXPECT_TRUE(cache.lookup(key(), inputs()).has_value());
    }
}

TEST_F(ProjectModelCacheTest, OtherGlobalPropertiesMsBuildOrEnvironmentMiss) {
    ProjectModelCache cache = stored();
    auto changed = inputs();
    changed.globalProperties.emplace_back("SolutionDir", "C:\\x\\");
    EXPECT_FALSE(cache.lookup(key(), changed).has_value());
    changed = inputs();
    changed.msbuildIdentity = "msbuild|1|3";
    EXPECT_FALSE(cache.lookup(key(), changed).has_value());
    changed = inputs();
    changed.environment = "env:2";
    EXPECT_FALSE(cache.lookup(key(), changed).has_value());
    EXPECT_TRUE(cache.lookup(key(), inputs()).has_value());
}

TEST_F(ProjectModelCacheTest, DamagedOrForeignEntriesAreMissesAndRemoveDeletes) {
    ProjectModelCache cache = stored();
    const std::string entry = cache.entryPath(key());
    ASSERT_TRUE(tree.fs.writeFileAtomic(entry, "{not json").has_value());
    std::string why;
    EXPECT_FALSE(cache.lookup(key(), inputs(), &why).has_value());
    EXPECT_EQ(why, "the entry is damaged or of another format");
    ASSERT_TRUE(tree.fs.writeFileAtomic(entry, R"({"format":99})").has_value());
    EXPECT_FALSE(cache.lookup(key(), inputs()).has_value());
    ProjectModelCache again = stored();
    ASSERT_TRUE(again.lookup(key(), inputs()).has_value());
    ASSERT_TRUE(again.remove(key()).has_value());
    EXPECT_FALSE(again.lookup(key(), inputs()).has_value());
    EXPECT_TRUE(again.remove(key()).has_value()) << "removing a missing entry is fine";
}

TEST(ProjectModelCacheInputs, TheEnvironmentFingerprintFollowsTheVariables) {
    const std::string base = environmentFingerprint();
    EXPECT_EQ(environmentFingerprint(), base) << "stable for one environment";
    const std::vector<std::pair<std::string, std::optional<std::string>>> extra{{"FROST_CACHE_TEST_VARIABLE", "1"}};
    const std::string withExtra = environmentFingerprint(extra);
    EXPECT_NE(withExtra, base);
    const std::vector<std::pair<std::string, std::optional<std::string>>> other{{"FROST_CACHE_TEST_VARIABLE", "2"}};
    EXPECT_NE(environmentFingerprint(other), withExtra);
    const std::vector<std::pair<std::string, std::optional<std::string>>> removed{
        {"FROST_CACHE_TEST_VARIABLE", std::nullopt}};
    EXPECT_EQ(environmentFingerprint(removed), base);
}

TEST(ProjectModelCacheInputs, TheMsBuildIdentityNamesBothFilesStamps) {
    MemTree tree;
    tree.write("vs/Bin/MSBuild.exe", "exe");
    tree.write("vs/Bin/Microsoft.Build.dll", "dll");
    auto identity = msbuildIdentity(tree.fs, MemTree::path("vs/Bin/MSBuild.exe"));
    ASSERT_TRUE(identity.has_value()) << identity.error().describe();
    tree.clock.advance(std::chrono::seconds(1));
    tree.write("vs/Bin/Microsoft.Build.dll", "dll2");
    auto updated = msbuildIdentity(tree.fs, MemTree::path("vs/Bin/MSBuild.exe"));
    ASSERT_TRUE(updated.has_value());
    EXPECT_NE(*identity, *updated) << "a Visual Studio update changes the identity";
    EXPECT_EQ(msbuildIdentity(tree.fs, "MSBuild.exe").error().code(), ErrorCode::InvalidArgument);
}

} // namespace
} // namespace frost::project
