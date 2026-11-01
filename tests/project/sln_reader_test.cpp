// SPDX-License-Identifier: Apache-2.0
//
// Tests for the static .sln reader (frost/project/solution_file.hpp): the SDK-shaped fixture
// (UTF-8 BOM, CRLF), LF and UTF-16 text, configurations, nesting and the error cases.
#include "frost/io/file_system.hpp"
#include "frost/project/solution_file.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <string>

namespace frost::project {
namespace {

using test::sdk;

constexpr std::string_view kApp = "{A1A1A1A1-0001-4000-8000-000000000001}";
constexpr std::string_view kCore = "{C0C0C0C0-0002-4000-8000-000000000002}";
constexpr std::string_view kEngine = "{E0E0E0E0-0003-4000-8000-000000000003}";
constexpr std::string_view kPackager = "{B0B0B0B0-0004-4000-8000-000000000004}";
constexpr std::string_view kLibraries = "{F0F0F0F0-0006-4000-8000-000000000006}";
constexpr std::string_view kTools = "{F1F1F1F1-0007-4000-8000-000000000007}";

const SolutionProjectEntry* findProject(const SolutionFile& file, std::string_view name) {
    for (const SolutionProjectEntry& project : file.projects) {
        if (project.name == name) {
            return &project;
        }
    }
    return nullptr;
}

const ProjectConfigurationMapping* findMapping(const SolutionProjectEntry& project, std::string_view config) {
    for (const auto& [name, mapping] : project.slnConfigurations) {
        if (name == config) {
            return &mapping;
        }
    }
    return nullptr;
}

TEST(SlnReader, ReadsTheSdkFixture) {
    auto file = readSolutionFile(io::realFileSystem(), sdk("SdkApp.sln"));
    ASSERT_TRUE(file.has_value()) << file.error().describe();
    EXPECT_EQ(file->format, SolutionFormat::Sln);
    EXPECT_EQ(file->formatVersion, "12.00");
    EXPECT_EQ(file->visualStudioVersion, "17.10.35027.167");
    EXPECT_EQ(file->directory, sdk());
    EXPECT_TRUE(file->warnings.empty());

    ASSERT_EQ(file->configurations.size(), 4u);
    EXPECT_EQ(file->configurations[0].name(), "Debug|x64");
    EXPECT_EQ(file->configurations[1].name(), "Debug|x86");
    EXPECT_EQ(file->configurations[3].name(), "Release|x86");

    ASSERT_EQ(file->folders.size(), 2u);
    EXPECT_EQ(file->folders[0].name, "Libraries");
    EXPECT_EQ(file->folders[0].id, kLibraries);
    EXPECT_EQ(file->folders[1].name, "Tools");
    ASSERT_EQ(file->folders[1].files.size(), 1u);
    EXPECT_EQ(file->folders[1].files[0], sdk("README.md"));

    ASSERT_EQ(file->projects.size(), 5u);
    const SolutionProjectEntry* app = findProject(*file, "App");
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->typeGuid, kCppProjectTypeGuid);
    EXPECT_EQ(app->projectGuid, kApp);
    EXPECT_EQ(app->relativePath, "App\\App.vcxproj");
    EXPECT_EQ(app->path, sdk("App/App.vcxproj"));
    EXPECT_EQ(app->parentId, "");
    ASSERT_EQ(app->dependencies.size(), 1u);
    EXPECT_EQ(app->dependencies[0], kPackager);

    const SolutionProjectEntry* core = findProject(*file, "Core");
    ASSERT_NE(core, nullptr);
    EXPECT_EQ(core->projectGuid, kCore);
    EXPECT_EQ(core->parentId, kLibraries);
    EXPECT_EQ(core->path, sdk("Libraries/Core/Core.vcxproj"));
    EXPECT_EQ(findProject(*file, "Engine")->projectGuid, kEngine);
    EXPECT_EQ(findProject(*file, "Packager")->parentId, kTools);
    EXPECT_EQ(findProject(*file, "Gone")->parentId, kTools);
}

TEST(SlnReader, MapsProjectConfigurations) {
    auto file = readSolutionFile(io::realFileSystem(), sdk("SdkApp.sln"));
    ASSERT_TRUE(file.has_value()) << file.error().describe();
    const SolutionProjectEntry& app = *findProject(*file, "App");
    const ProjectConfigurationMapping* debug64 = findMapping(app, "Debug|x64");
    ASSERT_NE(debug64, nullptr);
    EXPECT_EQ(*debug64, (ProjectConfigurationMapping{
                            .configuration = "Debug", .platform = "x64", .build = true, .deploy = true}));
    const ProjectConfigurationMapping* debug86 = findMapping(app, "Debug|x86");
    ASSERT_NE(debug86, nullptr);
    EXPECT_EQ(debug86->platform, "Win32");
    EXPECT_TRUE(debug86->build);
    EXPECT_FALSE(debug86->deploy);

    // Engine has an ActiveCfg but no Build.0 for Release|x86: mapped, not built.
    const ProjectConfigurationMapping* engine = findMapping(*findProject(*file, "Engine"), "Release|x86");
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->configuration, "Release");
    EXPECT_EQ(engine->platform, "Win32");
    EXPECT_FALSE(engine->build);

    const ProjectConfigurationMapping* packager = findMapping(*findProject(*file, "Packager"), "Debug|x64");
    ASSERT_NE(packager, nullptr);
    EXPECT_EQ(packager->platform, "Any CPU");
    EXPECT_FALSE(packager->build);
}

// A minimal LF solution written in place, with lower-case GUIDs and an unknown section.
TEST(SlnReader, AcceptsLfAndLowerCaseGuids) {
    const std::string text = "Microsoft Visual Studio Solution File, Format Version 12.00\n"
                             "Project(\"{8bc9ceb8-8b4a-11d0-8d11-00a0c91bc942}\") = \"Lib\", \"lib\\Lib.vcxproj\", "
                             "\"{01234567-89ab-4cde-8f01-23456789abcd}\"\n"
                             "\tProjectSection(WebsiteProperties) = preProject\n"
                             "\t\tSomething = Else\n"
                             "\tEndProjectSection\n"
                             "EndProject\n"
                             "Global\n"
                             "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n"
                             "\t\tDebug|x64 = Debug|x64\n"
                             "\tEndGlobalSection\n"
                             "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n"
                             "\t\t{01234567-89AB-4CDE-8F01-23456789ABCD}.Debug|x64.ActiveCfg = Debug|x64\n"
                             "\t\t{01234567-89ab-4cde-8f01-23456789abcd}.Debug|x64.Build.0 = Debug|x64\n"
                             "\tEndGlobalSection\n"
                             "EndGlobal\n";
    auto file = parseSln(text, test::MemTree::path("Demo.sln"));
    ASSERT_TRUE(file.has_value()) << file.error().describe();
    ASSERT_EQ(file->projects.size(), 1u);
    const SolutionProjectEntry& lib = file->projects[0];
    EXPECT_EQ(lib.typeGuid, kCppProjectTypeGuid);
    EXPECT_EQ(lib.projectGuid, "{01234567-89AB-4CDE-8F01-23456789ABCD}");
    EXPECT_EQ(lib.path, test::MemTree::path("lib/Lib.vcxproj"));
    ASSERT_EQ(lib.slnConfigurations.size(), 1u);
    EXPECT_TRUE(lib.slnConfigurations[0].second.build);
}

TEST(SlnReader, DecodesUtf16WithBom) {
    const std::u16string text = u"Microsoft Visual Studio Solution File, Format Version 12.00\r\n"
                                u"Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"Café\", "
                                u"\"Cafe\\Cafe.vcxproj\", \"{01234567-89AB-4CDE-8F01-23456789ABCD}\"\r\n"
                                u"EndProject\r\n";
    std::string bytes = "\xFF\xFE";
    for (char16_t unit : text) {
        bytes += static_cast<char>(unit & 0xFF);
        bytes += static_cast<char>(unit >> 8);
    }
    auto file = parseSln(bytes, test::MemTree::path("Demo.sln"));
    ASSERT_TRUE(file.has_value()) << file.error().describe();
    ASSERT_EQ(file->projects.size(), 1u);
    EXPECT_EQ(file->projects[0].name, "Caf\xC3\xA9");
}

TEST(SlnReader, RejectsFilesWithoutTheHeader) {
    auto file = parseSln("<Solution />\n", test::MemTree::path("Demo.sln"));
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().code(), ErrorCode::Corrupt);

    auto empty = parseSln("\xEF\xBB\xBF\r\n\r\n", test::MemTree::path("Demo.sln"));
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().code(), ErrorCode::Corrupt);
}

TEST(SlnReader, WarnsAboutUnknownNestingAndTruncatedSections) {
    const std::string text = "Microsoft Visual Studio Solution File, Format Version 12.00\n"
                             "Project(\"{2150E333-8FDC-42A3-9474-1A3956D46DE8}\") = \"Folder\", \"Folder\", "
                             "\"{00000000-0000-4000-8000-000000000001}\"\n"
                             "EndProject\n"
                             "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"A\", \"A\\A.vcxproj\", "
                             "\"{00000000-0000-4000-8000-0000000000AA}\"\n"
                             "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"B\", \"B\\B.vcxproj\", "
                             "\"{00000000-0000-4000-8000-0000000000BB}\"\n"
                             "EndProject\n"
                             "Global\n"
                             "\tGlobalSection(NestedProjects) = preSolution\n"
                             "\t\t{00000000-0000-4000-8000-0000000000AA} = {00000000-0000-4000-8000-000000000099}\n"
                             "\t\t{00000000-0000-4000-8000-0000000000BB} = {00000000-0000-4000-8000-000000000001}\n";
    auto file = parseSln(text, test::MemTree::path("Demo.sln"));
    ASSERT_TRUE(file.has_value()) << file.error().describe();
    ASSERT_EQ(file->projects.size(), 2u);
    EXPECT_EQ(file->projects[0].parentId, "");
    EXPECT_EQ(file->projects[1].parentId, "{00000000-0000-4000-8000-000000000001}");
    // "Project without EndProject", the unknown parent and the unterminated Global.
    EXPECT_EQ(file->warnings.size(), 3u);
}

TEST(SlnReader, RefusesOtherExtensions) {
    auto file = readSolutionFile(io::realFileSystem(), sdk("App/App.vcxproj"));
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().code(), ErrorCode::Unsupported);

    auto missing = readSolutionFile(io::realFileSystem(), sdk("Missing.sln"));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

} // namespace
} // namespace frost::project
