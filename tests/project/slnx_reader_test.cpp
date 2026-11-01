// SPDX-License-Identifier: Apache-2.0
//
// Tests for the static .slnx reader (frost/project/solution_file.hpp): the SDK-shaped fixture,
// folder nesting by name, per-project rules, defaults and the error cases.
#include "frost/io/file_system.hpp"
#include "frost/project/solution_file.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>

namespace frost::project {
namespace {

using test::sdk;

const SolutionProjectEntry* findProject(const SolutionFile& file, std::string_view name) {
    for (const SolutionProjectEntry& project : file.projects) {
        if (project.name == name) {
            return &project;
        }
    }
    return nullptr;
}

TEST(SlnxReader, ReadsTheSdkFixture) {
    auto file = readSolutionFile(io::realFileSystem(), sdk("SdkApp.slnx"));
    ASSERT_TRUE(file.has_value()) << file.error().describe();
    EXPECT_EQ(file->format, SolutionFormat::Slnx);
    EXPECT_TRUE(file->warnings.empty());

    // Build types outer, platforms inner.
    ASSERT_EQ(file->configurations.size(), 4u);
    EXPECT_EQ(file->configurations[0].name(), "Debug|x64");
    EXPECT_EQ(file->configurations[1].name(), "Debug|x86");
    EXPECT_EQ(file->configurations[2].name(), "Release|x64");

    ASSERT_EQ(file->folders.size(), 2u);
    EXPECT_EQ(file->folders[0].id, "/Libraries/");
    EXPECT_EQ(file->folders[0].name, "Libraries");
    EXPECT_EQ(file->folders[1].id, "/Tools/");
    ASSERT_EQ(file->folders[1].files.size(), 1u);
    EXPECT_EQ(file->folders[1].files[0], sdk("README.md"));

    ASSERT_EQ(file->projects.size(), 5u);
    const SolutionProjectEntry* core = findProject(*file, "Core");
    ASSERT_NE(core, nullptr);
    EXPECT_EQ(core->parentId, "/Libraries/");
    EXPECT_EQ(core->projectGuid, "{C0C0C0C0-0002-4000-8000-000000000002}");
    EXPECT_EQ(core->path, sdk("Libraries/Core/Core.vcxproj"));

    const SolutionProjectEntry* engine = findProject(*file, "Engine");
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->projectGuid, ""); // No Id attribute.
    ASSERT_EQ(engine->slnxRules.size(), 1u);
    EXPECT_EQ(engine->slnxRules[0],
              (ConfigurationRule{
                  .kind = ConfigurationRuleKind::Build, .solutionPattern = "Release|x86", .projectValue = "false"}));

    const SolutionProjectEntry* app = findProject(*file, "App");
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->parentId, "");
    ASSERT_EQ(app->dependencies.size(), 1u);
    EXPECT_EQ(app->dependencies[0], sdk("Tools/Packager/Packager.csproj"));
    EXPECT_EQ(findProject(*file, "Packager")->parentId, "/Tools/");
}

TEST(SlnxReader, NestsFoldersByNameAndDefaultsConfigurations) {
    const std::string text = R"(<?xml version="1.0" encoding="utf-8"?>
<Solution Version="1.0">
  <Folder Name="/Libraries/Third Party/">
    <Project Path="ext\zlib\zlib.vcxproj" Type="{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}" DisplayName="ZLib" />
  </Folder>
  <folder name="/Libraries/">
    <file path="docs/notes.md" />
  </folder>
  <Project Path="tool/Tool.csproj" Type="C#">
    <BuildType Solution="Release" Project="Debug" />
    <Platform Project="x64" />
  </Project>
</Solution>
)";
    auto file = parseSlnx(text, test::MemTree::path("Demo.slnx"));
    ASSERT_TRUE(file.has_value()) << file.error().describe();
    EXPECT_EQ(file->formatVersion, "1.0");
    ASSERT_EQ(file->folders.size(), 2u);
    EXPECT_EQ(file->folders[0].id, "/Libraries/");
    EXPECT_EQ(file->folders[0].parentId, "");
    EXPECT_EQ(file->folders[1].id, "/Libraries/Third Party/");
    EXPECT_EQ(file->folders[1].name, "Third Party");
    EXPECT_EQ(file->folders[1].parentId, "/Libraries/");
    // The lower-case <folder> names the same folder; names are matched case-insensitively.
    ASSERT_EQ(file->folders[0].files.size(), 1u);
    EXPECT_EQ(file->folders[0].files[0], test::MemTree::path("docs/notes.md"));

    ASSERT_EQ(file->projects.size(), 2u);
    EXPECT_EQ(file->projects[0].name, "ZLib");
    EXPECT_EQ(file->projects[0].typeGuid, kCppProjectTypeGuid);
    EXPECT_EQ(file->projects[0].parentId, "/Libraries/Third Party/");
    EXPECT_EQ(file->projects[0].path, test::MemTree::path("ext/zlib/zlib.vcxproj"));
    EXPECT_EQ(file->projects[1].name, "Tool");
    EXPECT_EQ(file->projects[1].typeName, "C#");
    ASSERT_EQ(file->projects[1].slnxRules.size(), 2u);
    EXPECT_EQ(file->projects[1].slnxRules[1].kind, ConfigurationRuleKind::Platform);
    EXPECT_EQ(file->projects[1].slnxRules[1].solutionPattern, "");

    // No <Configurations>: Debug and Release on "Any CPU".
    ASSERT_EQ(file->configurations.size(), 2u);
    EXPECT_EQ(file->configurations[0].name(), "Debug|Any CPU");
    EXPECT_EQ(file->configurations[1].name(), "Release|Any CPU");
}

TEST(SlnxReader, RejectsMalformedXmlAndOtherRoots) {
    auto malformed =
        parseSlnx("<Solution>\n  <Project Path=\"a.vcxproj\">\n</Solution>\n", test::MemTree::path("Demo.slnx"));
    ASSERT_FALSE(malformed.has_value());
    EXPECT_EQ(malformed.error().code(), ErrorCode::Corrupt);
    EXPECT_NE(malformed.error().message().find("line"), std::string::npos) << malformed.error().message();

    auto other = parseSlnx("<Project />", test::MemTree::path("Demo.slnx"));
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().code(), ErrorCode::Corrupt);

    auto relative = parseSlnx("<Solution />", "Demo.slnx");
    ASSERT_FALSE(relative.has_value());
    EXPECT_EQ(relative.error().code(), ErrorCode::InvalidArgument);
}

TEST(SlnxReader, SkipsProjectsWithoutAPath) {
    auto file =
        parseSlnx("<Solution><Project /><Project Path=\"a/A.vcxproj\" /></Solution>", test::MemTree::path("Demo.slnx"));
    ASSERT_TRUE(file.has_value()) << file.error().describe();
    ASSERT_EQ(file->projects.size(), 1u);
    EXPECT_EQ(file->projects[0].name, "A");
    EXPECT_EQ(file->warnings.size(), 1u);
}

} // namespace
} // namespace frost::project
