// SPDX-License-Identifier: Apache-2.0
//
// The ProjectHost protocol (frost/project/project_host.hpp) without a helper: request lines,
// answer parsing, the MSBuild.exe fallback's command line and output, the conversion into the
// Solution Explorer's items, and SolutionModel built from evaluated items on the synthetic
// generated solution (fixtures/msvc_generated, SLN-0). Label "unit"; the real MSBuild runs in
// project_host_test.cpp.

#include "frost/io/file_system.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/solution_model.hpp"

#include "json.hpp"
#include "paths.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace frost::project {
namespace {

using test::fixture;

std::string generated(std::string_view relative = {}) {
    return relative.empty() ? fixture("msvc_generated")
                            : fixture(std::string("msvc_generated/") + std::string(relative));
}

ProjectEvaluationRequest coreRequest() {
    return {.projectPath = generated("Core/Core.vcxproj"),
            .configuration = "Debug",
            .platform = "x64",
            .solutionPath = generated("Generated.sln"),
            .itemTypes = {},
            .metadata = {"PreprocessorDefinitions"},
            .properties = {"ConfigurationType"}};
}

TEST(ProjectHostProtocol, GlobalPropertiesAreTheOnesVisualStudioSetsForASolutionProject) {
    const auto properties = evaluationGlobalProperties(coreRequest());
    ASSERT_EQ(properties.size(), 7U);
    EXPECT_EQ(properties[0], (std::pair<std::string, std::string>{"Configuration", "Debug"}));
    EXPECT_EQ(properties[1], (std::pair<std::string, std::string>{"Platform", "x64"}));
    EXPECT_EQ(properties[2].first, "SolutionDir");
    EXPECT_TRUE(properties[2].second.ends_with('\\') || properties[2].second.ends_with('/'));
    EXPECT_EQ(properties[3], (std::pair<std::string, std::string>{"SolutionPath", generated("Generated.sln")}));
    EXPECT_EQ(properties[4], (std::pair<std::string, std::string>{"SolutionName", "Generated"}));
    EXPECT_EQ(properties[5], (std::pair<std::string, std::string>{"SolutionFileName", "Generated.sln"}));
    EXPECT_EQ(properties[6], (std::pair<std::string, std::string>{"SolutionExt", ".sln"}));
    auto alone = coreRequest();
    alone.solutionPath.clear();
    EXPECT_EQ(evaluationGlobalProperties(alone).size(), 2U);
}

TEST(ProjectHostProtocol, TheEvaluateRequestIsOneJsonLineWithEveryGlobalProperty) {
    auto request = coreRequest();
    request.projectPath = generated("Core") + "\\we\"ird\\Core.vcxproj";
    auto line = projectHostEvaluateRequest(request, "7");
    ASSERT_TRUE(line.has_value()) << line.error().describe();
    EXPECT_EQ(line->find('\n'), std::string::npos);
    auto json = detail::parseJson(*line);
    ASSERT_TRUE(json.has_value()) << *line;
    EXPECT_EQ(json->stringAt("Op"), "Evaluate");
    EXPECT_EQ(json->stringAt("Id"), "7");
    EXPECT_EQ(json->stringAt("Trusted"), "true");
    EXPECT_EQ(json->stringAt("Project"), request.projectPath); // The quote survives the escaping.
    EXPECT_EQ(json->stringAt("Property:Configuration"), "Debug");
    EXPECT_EQ(json->stringAt("Property:SolutionName"), "Generated");
    EXPECT_EQ(json->stringAt("Metadata"), "PreprocessorDefinitions");
    // Every file item type is asked for when the request names none.
    const std::string types = json->stringAt("ItemTypes");
    for (std::string_view name : fileItemTypeNames()) {
        EXPECT_NE(types.find(name), std::string::npos) << name;
    }
    request.configuration.clear();
    EXPECT_FALSE(projectHostEvaluateRequest(request, "8").has_value());
    request = coreRequest();
    request.projectPath = "Core.vcxproj";
    EXPECT_EQ(projectHostEvaluateRequest(request, "9").error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectHostProtocol, HelloAnswersAreCheckedForTheProtocolVersion) {
    auto hello = parseProjectHostHello(
        R"({"Op":"Hello","Protocol":1,"Error":null,"MSBuildVersion":"17.14.40.60911","MSBuildAssembly":"C:\\vs\\Microsoft.Build.dll","VisualStudioVersion":"17.0","ProcessId":4242})");
    ASSERT_TRUE(hello.has_value()) << hello.error().describe();
    EXPECT_EQ(hello->msbuildVersion, "17.14.40.60911");
    EXPECT_EQ(hello->visualStudioVersion, "17.0");
    EXPECT_EQ(hello->processId, 4242U);
    EXPECT_EQ(parseProjectHostHello(R"({"Op":"Hello","Protocol":2})").error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(parseProjectHostHello("not json").error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(parseProjectHostHello(R"({"Op":"Evaluate","Protocol":1})").error().code(), ErrorCode::Corrupt);
}

TEST(ProjectHostProtocol, EvaluateAnswersCarryItemsImportsAndTheHelpersErrors) {
    auto evaluation = parseProjectHostEvaluation(
        R"({"Op":"Evaluate","Id":"1","Error":null,"Project":"C:\\s\\A.vcxproj","Configuration":"Debug","Platform":"x64",)"
        R"("Items":[{"Type":"ClCompile","Include":"a.cpp","FullPath":"C:\\s\\a.cpp","DefiningProject":"C:\\s\\A.vcxproj","Metadata":{"PreprocessorDefinitions":"X=1;"}}],)"
        R"("Imports":["C:\\s\\a.props"],"Properties":{"ConfigurationType":"Application"},"ProjectConfigurations":["Debug|x64"]})");
    ASSERT_TRUE(evaluation.has_value()) << evaluation.error().describe();
    ASSERT_EQ(evaluation->items.size(), 1U);
    EXPECT_EQ(evaluation->items[0].type, "ClCompile");
    EXPECT_EQ(evaluation->items[0].metadata.at("PreprocessorDefinitions"), "X=1;");
    EXPECT_EQ(evaluation->imports, std::vector<std::string>{"C:\\s\\a.props"});
    EXPECT_EQ(evaluation->properties.at("ConfigurationType"), "Application");
    EXPECT_EQ(evaluation->projectConfigurations, std::vector<std::string>{"Debug|x64"});
    auto failed = parseProjectHostEvaluation(
        R"({"Op":"Evaluate","Id":"2","Error":"MSB4025: The project file could not be loaded."})");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::IoError);
    EXPECT_NE(failed.error().message().find("MSB4025"), std::string::npos);
    EXPECT_EQ(parseProjectHostEvaluation(R"({"Op":"Evaluate","Id":"3","Error":null})").error().code(),
              ErrorCode::Corrupt);
}

TEST(ProjectHostProtocol, TheFallbackRunsMsBuildExeWithTheSameGlobalProperties) {
    const std::string msbuild = generated("MSBuild.exe"); // Any absolute path: nothing runs here.
    auto spec = msbuildCliEvaluationSpec(msbuild, coreRequest());
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    EXPECT_EQ(spec->executable, msbuild);
    EXPECT_EQ(spec->arguments[0], coreRequest().projectPath);
    EXPECT_NE(std::ranges::find(spec->arguments, "-nodeReuse:false"), spec->arguments.end());
    EXPECT_NE(std::ranges::find(spec->arguments, "-p:Configuration=Debug"), spec->arguments.end());
    EXPECT_NE(std::ranges::find(spec->arguments, "-p:SolutionName=Generated"), spec->arguments.end());
    EXPECT_NE(std::ranges::find(spec->arguments, "-getProperty:ConfigurationType"), spec->arguments.end());
    EXPECT_TRUE(std::ranges::any_of(spec->arguments,
                                    [](const std::string& a) { return a.starts_with("-getItem:ClCompile;"); }));
    EXPECT_EQ(spec->workingDirectory, generated("Core"));
    EXPECT_FALSE(msbuildCliEvaluationSpec("MSBuild.exe", coreRequest()).has_value());

    // MSBuild's -getItem JSON: grouped by type, metadata names as MSBuild spells them.
    const std::string output =
        "warning banner\n"
        R"({"Properties":{"ConfigurationType":"StaticLibrary"},"Items":{"ClInclude":[{"Identity":"x.h","FullPath":"C:\\s\\x.h","DefiningProjectFullPath":"C:\\s\\Core.vcxproj"}],)"
        R"("ClCompile":[{"Identity":"a.cpp","FullPath":"C:\\s\\a.cpp","preprocessordefinitions":"A=1;","DefiningProjectFullPath":"C:\\s\\Core.vcxproj"}]}})";
    auto parsed = parseMsBuildCliEvaluation(output, coreRequest());
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    ASSERT_EQ(parsed->items.size(), 2U);
    EXPECT_EQ(parsed->items[0].type, "ClCompile"); // fileItemTypeNames() order, not the output's.
    EXPECT_EQ(parsed->items[0].metadata.at("PreprocessorDefinitions"), "A=1;");
    EXPECT_EQ(parsed->items[1].definingProject, "C:\\s\\Core.vcxproj");
    EXPECT_EQ(parsed->properties.at("ConfigurationType"), "StaticLibrary");
    EXPECT_TRUE(parsed->imports.empty()); // -getItem cannot list imports.
    EXPECT_EQ(parseMsBuildCliEvaluation("MSB1009: Project file does not exist.", coreRequest()).error().code(),
              ErrorCode::Corrupt);
}

TEST(ProjectHostProtocol, TheExplorerGetsFileItemsOnceEachWithNormalisedPaths) {
    HostEvaluation evaluation;
    evaluation.projectPath = generated("Core/Core.vcxproj");
    evaluation.configuration = "Debug";
    evaluation.platform = "x64";
    const std::string a = generated("Core/src/a.cpp");
    evaluation.items = {
        {.type = "ClCompile", .include = "src\\a.cpp", .fullPath = a, .definingProject = {}, .metadata = {}},
        {.type = "ProjectReference", .include = "x", .fullPath = generated("x"), .definingProject = {}, .metadata = {}},
        {.type = "Text", .include = "src\\a.cpp", .fullPath = a, .definingProject = {}, .metadata = {}},
        {.type = "MASM",
         .include = "b.asm",
         .fullPath = generated("Core/b.asm"),
         .definingProject = {},
         .metadata = {}},
        {.type = "None", .include = "", .fullPath = "", .definingProject = {}, .metadata = {}}};
    evaluation.imports = {generated("props/Common.props"), generated("props/Common.props")};
    const auto items = toEvaluatedProjectItems(evaluation, EvaluationSource::ProjectHost, "17.14");
    ASSERT_EQ(items.items.size(), 2U);
    EXPECT_EQ(items.items[0].type, ItemType::ClCompile);
    EXPECT_EQ(items.items[0].path, a);
    EXPECT_EQ(items.items[1].type, ItemType::Other);
    EXPECT_EQ(items.items[1].itemName, "MASM");
    EXPECT_EQ(items.imports.size(), 1U);
    EXPECT_EQ(items.warnings.size(), 1U); // The None item without a path.
    EXPECT_EQ(items.msbuildVersion, "17.14");
}

// The synthetic generated solution, statically: both formats list the same projects and folders,
// and Profile|x64 maps Core to its Release configuration.
TEST(GeneratedSolution, SlnAndSlnxDescribeTheSameSolution) {
    for (const char* name : {"Generated.sln", "Generated.slnx"}) {
        SCOPED_TRACE(name);
        auto model = SolutionModel::load(io::realFileSystem(), generated(name));
        ASSERT_TRUE(model.has_value()) << model.error().describe();
        ASSERT_EQ(model->projects().size(), 3U);
        const auto core = model->findProjectByPath(generated("Core/Core.vcxproj"));
        const auto app = model->findProjectByPath(generated("App/App.vcxproj"));
        ASSERT_TRUE(core && app);
        EXPECT_EQ(model->project(*core).nodeKind, NodeKind::Project);
        const auto profile = model->findConfiguration("Profile|x64");
        ASSERT_TRUE(profile.has_value());
        const auto& coreProfile = model->projectConfiguration(*core, *profile);
        ASSERT_TRUE(coreProfile.has_value());
        EXPECT_EQ(coreProfile->configuration, "Release");
        EXPECT_EQ(model->projectConfiguration(*app, *profile)->configuration, "Profile");
        const auto debugX86 = model->findConfiguration("Debug|x86");
        ASSERT_TRUE(debugX86.has_value());
        EXPECT_EQ(model->projectConfiguration(*core, *debugX86)->platform, "Win32");
        // The App depends on Renderer (solution dependency) and Core and Renderer (references).
        EXPECT_EQ(model->project(*app).dependencies.size(), 2U);
        // Static reading cannot see the property-sheet items.
        const auto staticFiles = model->projectFilePaths(*core);
        EXPECT_EQ(std::ranges::count(staticFiles, generated("shared/deep_shared.cpp")), 0);
        EXPECT_FALSE(model->project(*core).evaluation.has_value());
    }
}

// Evaluated items replace the static ones and are placed by the .filters file.
TEST(GeneratedSolution, EvaluatedItemsReplaceTheStaticOnesAndFollowTheFilters) {
    EvaluatedProjectItems core{.projectPath = generated("Core/Core.vcxproj"),
                               .configuration = "Release",
                               .platform = "x64",
                               .source = EvaluationSource::ProjectHost,
                               .msbuildVersion = "17.14",
                               .items = {},
                               .imports = {generated("Directory.Build.props")},
                               .warnings = {}};
    const auto add = [&core](ItemType type, std::string name, std::string_view relative) {
        core.items.push_back(ProjectItem{.type = type,
                                         .itemName = std::move(name),
                                         .include = std::string(relative),
                                         .path = generated(relative),
                                         .fromWildcard = false});
    };
    add(ItemType::ClCompile, "ClCompile", "shared/deep_shared.cpp");
    add(ItemType::ClCompile, "ClCompile", "Core/src/a.cpp");
    add(ItemType::ClCompile, "ClCompile", "Core/src/a.cpp"); // Listed twice, shown once.
    add(ItemType::ClInclude, "ClInclude", "shared/config.h");
    add(ItemType::Text, "Text", "Core/unlisted.txt");
    auto model = SolutionModel::load(io::realFileSystem(), generated("Generated.sln"),
                                     {.cppHierarchy = CppHierarchyMode::Filters,
                                      .unloadedProjectPaths = {},
                                      .evaluated = {core},
                                      .mode = SolutionModelMode::NativeStatic});
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    const auto id = model->findProjectByPath(generated("Core/Core.vcxproj"));
    ASSERT_TRUE(id.has_value());
    const ProjectInfo& info = model->project(*id);
    ASSERT_TRUE(info.evaluation.has_value());
    EXPECT_EQ(info.evaluation->itemCount, 4U);
    EXPECT_EQ(info.evaluation->configuration, "Release");
    EXPECT_EQ(info.evaluation->imports.size(), 1U);
    const auto files = model->projectFilePaths(*id);
    EXPECT_EQ(files.size(), 4U);
    // debug_only.cpp and b.cpp are static items the evaluation did not list: gone.
    EXPECT_EQ(std::ranges::count(files, generated("Core/debug_only.cpp")), 0);
    EXPECT_EQ(std::ranges::count(files, generated("Core/src/b.cpp")), 0);
    const auto shared = model->findFileNode(generated("shared/deep_shared.cpp"));
    ASSERT_TRUE(shared.has_value());
    const auto parent = model->node(*shared).parent;
    ASSERT_TRUE(parent.has_value());
    EXPECT_EQ(model->node(*parent).kind, NodeKind::Filter);
    EXPECT_EQ(model->node(*parent).displayName, "Shared"); // From Core.vcxproj.filters.
    const auto unlisted = model->findFileNode(generated("Core/unlisted.txt"));
    ASSERT_TRUE(unlisted.has_value());
    EXPECT_EQ(model->node(*unlisted).parent, info.node); // No filter entry: the project root.
    // Other projects keep their static items.
    const auto app = model->findProjectByPath(generated("App/App.vcxproj"));
    EXPECT_FALSE(model->project(*app).evaluation.has_value());
    EXPECT_FALSE(model->projectFilePaths(*app).empty());
}

TEST(GeneratedSolution, EvaluationSourcesHaveDisplayNames) {
    EXPECT_EQ(toString(EvaluationSource::ProjectHost), "ProjectHost");
    EXPECT_EQ(toString(EvaluationSource::MsBuildCli), "MSBuild.exe");
}

} // namespace
} // namespace frost::project
