// SPDX-License-Identifier: Apache-2.0
//
// Tests for the static .vcxproj and .vcxproj.filters readers (frost/project/vcxproj.hpp):
// properties, configurations, items (wildcards, Exclude, ../ paths, $(Name) expansion,
// duplicates), references, filters, BOM and CRLF, and the error cases.
#include "frost/io/file_system.hpp"
#include "frost/project/vcxproj.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace frost::project {
namespace {

using test::MemTree;
using test::sdk;

std::vector<std::string> itemPaths(const VcxProject& project) {
    std::vector<std::string> out;
    for (const ProjectItem& item : project.items) {
        out.push_back(item.path);
    }
    return out;
}

const ProjectItem* findItem(const VcxProject& project, std::string_view path) {
    for (const ProjectItem& item : project.items) {
        if (item.path == path) {
            return &item;
        }
    }
    return nullptr;
}

TEST(VcxprojReader, ReadsCoreWithWildcardsExcludesAndRelativePaths) {
    auto core = readVcxproj(io::realFileSystem(), sdk("Libraries/Core/Core.vcxproj"),
                            VcxprojOptions{.solutionDirectory = sdk()});
    ASSERT_TRUE(core.has_value()) << core.error().describe();
    EXPECT_EQ(core->projectGuid, "{C0C0C0C0-0002-4000-8000-000000000002}");
    EXPECT_EQ(core->rootNamespace, "Core");
    EXPECT_EQ(core->configurationType, "StaticLibrary");
    EXPECT_EQ(core->platformToolset, "v143");
    EXPECT_EQ(core->directory, sdk("Libraries/Core"));

    // src\**\*.cpp minus src\legacy\** (sorted), the conditioned duplicate of core.cpp dropped,
    // include\core\*.h, a ../ path, $(CoreGenerated) and $(MSBuildThisFileDirectory) expanded.
    const std::vector<std::string> expected{
        sdk("Libraries/Core/src/core.cpp"),
        sdk("Libraries/Core/src/detail/strings.cpp"),
        sdk("Libraries/Core/include/core/core.h"),
        sdk("Libraries/Core/include/core/version.h"),
        sdk("shared/common.h"),
        sdk("Libraries/Core/generated/config.h"),
        sdk("Libraries/Core/core.natvis"),
        sdk("Libraries/Core/tools/gen.py"),
    };
    EXPECT_EQ(itemPaths(*core), expected);
    const ProjectItem* cpp = findItem(*core, sdk("Libraries/Core/src/core.cpp"));
    ASSERT_NE(cpp, nullptr);
    EXPECT_EQ(cpp->type, ItemType::ClCompile);
    EXPECT_TRUE(cpp->fromWildcard);
    EXPECT_EQ(cpp->include, "src\\**\\*.cpp");
    EXPECT_EQ(findItem(*core, sdk("shared/common.h"))->type, ItemType::ClInclude);
    EXPECT_FALSE(findItem(*core, sdk("shared/common.h"))->fromWildcard);
    EXPECT_EQ(findItem(*core, sdk("Libraries/Core/core.natvis"))->type, ItemType::Natvis);
    EXPECT_EQ(findItem(*core, sdk("Libraries/Core/tools/gen.py"))->type, ItemType::CustomBuild);

    // $(SdkDocs) comes from nowhere the static reader can see.
    EXPECT_EQ(core->unresolvedIncludes, std::vector<std::string>{"$(SdkDocs)notes.txt"});

    ASSERT_EQ(core->configurations.size(), 4u);
    EXPECT_EQ(core->configurations[0].name(), "Debug|Win32");
    EXPECT_EQ(core->configurations[3].name(), "Release|x64");
    EXPECT_EQ(core->configurations[3].configurationType, "StaticLibrary");
    EXPECT_EQ(core->configurations[3].platformToolset, "v143");
    EXPECT_TRUE(core->references.empty());
    EXPECT_EQ(core->properties.at("coregenerated"), "generated\\");
}

TEST(VcxprojReader, ReadsAppWithBomCrlfAndReferences) {
    auto app = readVcxproj(io::realFileSystem(), sdk("App/App.vcxproj"), VcxprojOptions{.solutionDirectory = sdk()});
    ASSERT_TRUE(app.has_value()) << app.error().describe();
    EXPECT_EQ(app->projectGuid, "{A1A1A1A1-0001-4000-8000-000000000001}");
    EXPECT_EQ(app->configurationType, "Application");
    ASSERT_EQ(app->items.size(), 6u);
    EXPECT_EQ(app->items[0].path, sdk("App/src/main.cpp"));
    EXPECT_EQ(app->items[3].type, ItemType::ResourceCompile);
    EXPECT_EQ(app->items[4].type, ItemType::Image);
    EXPECT_EQ(app->items[5].path, sdk("README.md")); // $(SolutionDir)README.md
    EXPECT_EQ(app->items[5].type, ItemType::None);

    ASSERT_EQ(app->references.size(), 3u);
    EXPECT_EQ(app->references[0].path, sdk("Libraries/Engine/Engine.vcxproj"));
    EXPECT_EQ(app->references[0].projectGuid, "{E0E0E0E0-0003-4000-8000-000000000003}");
    EXPECT_EQ(app->references[1].path, sdk("Libraries/Core/Core.vcxproj"));
    EXPECT_EQ(app->references[2].path, test::fixture("Elsewhere/Outside.vcxproj"));
    EXPECT_EQ(app->references[2].projectGuid, "");
}

TEST(VcxprojReader, KeepsPerConfigurationProperties) {
    auto engine = readVcxproj(io::realFileSystem(), sdk("Libraries/Engine/Engine.vcxproj"));
    ASSERT_TRUE(engine.has_value()) << engine.error().describe();
    EXPECT_EQ(engine->configurationType, "DynamicLibrary"); // The first value in the file.
    for (const ProjectConfiguration& config : engine->configurations) {
        EXPECT_EQ(config.configurationType, config.configuration == "Debug" ? "DynamicLibrary" : "StaticLibrary")
            << config.name();
    }
    // Without a solution directory $(SolutionDir) stays undefined.
    EXPECT_FALSE(engine->properties.contains("solutiondir"));
}

TEST(VcxprojReader, ReadsFilters) {
    auto core = readVcxproj(io::realFileSystem(), sdk("Libraries/Core/Core.vcxproj"));
    ASSERT_TRUE(core.has_value()) << core.error().describe();
    auto filters = readVcxprojFilters(io::realFileSystem(), *core);
    ASSERT_TRUE(filters.has_value()) << filters.error().describe();
    ASSERT_EQ(filters->filters.size(), 5u);
    EXPECT_EQ(filters->filters[1].path, "Source Files\\Detail");
    EXPECT_EQ(filters->filters[1].uniqueIdentifier, "{0A000000-0000-4000-8000-000000000002}");
    ASSERT_EQ(filters->assignments.size(), 6u);
    EXPECT_EQ(filters->assignments[1].itemPath, sdk("Libraries/Core/src/detail/strings.cpp"));
    EXPECT_EQ(filters->assignments[1].filter, "Source Files\\Detail");
    EXPECT_EQ(filters->assignments[3].itemPath, sdk("shared/common.h"));
    EXPECT_EQ(filters->assignments[3].filter, "Header Files\\Shared");
    EXPECT_EQ(filters->assignments[5].filter, ""); // core.natvis has no <Filter>.

    // App's filters file has a BOM and CRLF line ends.
    auto app = readVcxproj(io::realFileSystem(), sdk("App/App.vcxproj"));
    ASSERT_TRUE(app.has_value());
    auto appFilters = readVcxprojFilters(io::realFileSystem(), *app);
    ASSERT_TRUE(appFilters.has_value()) << appFilters.error().describe();
    EXPECT_EQ(appFilters->filters.size(), 3u);
    EXPECT_EQ(appFilters->assignments.size(), 5u);
    EXPECT_EQ(appFilters->filters[0].extensions, "cpp;c;cc;cxx");

    // Engine has no .filters file: empty, not an error.
    auto engine = readVcxproj(io::realFileSystem(), sdk("Libraries/Engine/Engine.vcxproj"));
    ASSERT_TRUE(engine.has_value());
    auto none = readVcxprojFilters(io::realFileSystem(), *engine);
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->filters.empty());
    EXPECT_TRUE(none->assignments.empty());
}

TEST(VcxprojReader, ReadsChooseBranchesAndSplitsIncludeLists) {
    MemTree tree;
    tree.write("p/a.cpp", "");
    tree.write("p/sub/b.cpp", "");
    tree.write("p/sub/deeper/c.cpp", "");
    tree.write("p/sub/skip.h", "");
    const std::string text = R"(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Choose>
    <When Condition="'$(Platform)'=='x64'">
      <ItemGroup><clcompile Include="a.cpp ; one.cpp;;two.cpp" /></ItemGroup>
    </When>
    <Otherwise>
      <ItemGroup><ClCompile Include="sub\*.cpp" /></ItemGroup>
    </Otherwise>
  </Choose>
  <ItemGroup>
    <ClCompile Include="sub/**/?.cpp" />
    <ClInclude Include="%(Identity).h" />
    <MASM Include="asm\x.asm" />
    <ClCompile Remove="a.cpp" />
  </ItemGroup>
</Project>
)";
    auto project = parseVcxproj(tree.fs, text, MemTree::path("p/P.vcxproj"));
    ASSERT_TRUE(project.has_value()) << project.error().describe();
    const std::vector<std::string> expected{
        MemTree::path("p/a.cpp"),     MemTree::path("p/one.cpp"),          MemTree::path("p/two.cpp"),
        MemTree::path("p/sub/b.cpp"), MemTree::path("p/sub/deeper/c.cpp"), MemTree::path("p/asm/x.asm"),
    };
    EXPECT_EQ(itemPaths(*project), expected);
    EXPECT_EQ(project->items[5].type, ItemType::Other);
    EXPECT_EQ(project->items[5].itemName, "MASM");
    EXPECT_EQ(project->unresolvedIncludes, std::vector<std::string>{"%(Identity).h"});
}

TEST(VcxprojReader, RefusesRecursiveWildcardsAtTheRoot) {
    MemTree tree;
    const std::string root = io::kNativePathStyle == io::PathStyle::Windows ? "C:\\" : "/";
    const std::string text =
        "<Project><ItemGroup><ClCompile Include=\"" + root + "**\\*.cpp\" /></ItemGroup></Project>";
    auto project = parseVcxproj(tree.fs, text, MemTree::path("P.vcxproj"));
    ASSERT_TRUE(project.has_value()) << project.error().describe();
    EXPECT_TRUE(project->items.empty());
    ASSERT_EQ(project->warnings.size(), 1u);
    EXPECT_NE(project->warnings[0].find("refused"), std::string::npos) << project->warnings[0];
}

TEST(VcxprojReader, ReportsMalformedProjects) {
    MemTree tree;
    auto malformed = parseVcxproj(tree.fs, "<Project><ItemGroup></Project>", MemTree::path("P.vcxproj"));
    ASSERT_FALSE(malformed.has_value());
    EXPECT_EQ(malformed.error().code(), ErrorCode::Corrupt);

    auto other = parseVcxproj(tree.fs, "<Solution />", MemTree::path("P.vcxproj"));
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().code(), ErrorCode::Corrupt);

    auto missing = readVcxproj(tree.fs, MemTree::path("Missing.vcxproj"));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(toString(ItemType::ClInclude), "ClInclude");
    EXPECT_EQ(toString(ItemType::Other), "Other");
}

} // namespace
} // namespace frost::project
