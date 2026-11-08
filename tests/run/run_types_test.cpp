// SPDX-License-Identifier: Apache-2.0
/*
 * Unit tests (label unit) for the generated run configuration types (frost/run/run_types.hpp,
 * RUN-2) and RunConfigurationSet (frost/run/run_configuration_set.hpp): ids, the CMake generator
 * over a File API model (one per EXECUTABLE target), the program of a built target, the MSBuild
 * generator over a solution in a MemFs (Application projects in the active configuration), native
 * configurations, and the merge and store rules. The real File API reply (cmake's own JSON as the
 * oracle) is tests/app/workbench_run_test.cpp's.
 */

#include "frost/project/cmake_file_api.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/run/run_configuration_set.hpp"
#include "frost/run/run_types.hpp"

#include "build/build_test_util.hpp"
#include <gtest/gtest.h>

#include <format>
#include <string>
#include <vector>

namespace frost::run {
namespace {

using build::test::MemTree;

project::CMakeTarget target(std::string name, project::CMakeTargetType type, std::vector<std::string> artifacts = {}) {
    project::CMakeTarget out;
    out.name = std::move(name);
    out.type = type;
    out.artifacts = std::move(artifacts);
    return out;
}

TEST(RunTypes, IdsKeepTheAlphabetAndStayUnique) {
    EXPECT_EQ(runConfigurationIdFor(kCMakeIdPrefix, "Frost Editor (x64)"), "cmake.Frost_Editor__x64_");
    EXPECT_EQ(runConfigurationIdFor(kMSBuildIdPrefix, "App"), "msbuild.App");
    EXPECT_EQ(runConfigurationIdFor(kNativeIdPrefix, std::string(300, 'a')).size(), 128U);
    std::vector<RunConfiguration> taken(2);
    taken[0].id = "native.Tool";
    taken[1].id = "native.Tool-2";
    EXPECT_EQ(uniqueRunConfigurationId("native.Tool", taken), "native.Tool-3");
    EXPECT_EQ(uniqueRunConfigurationId("native.Other", taken), "native.Other");
    // A long base is cut so that the suffix fits.
    taken[0].id = std::string(128, 'b');
    EXPECT_EQ(uniqueRunConfigurationId(std::string(128, 'b'), taken), std::string(126, 'b') + "-2");
    for (const RunConfiguration& configuration : taken) {
        RunConfiguration valid = configuration;
        valid.name = "n";
        valid.program = MemTree::path("p.exe");
        EXPECT_TRUE(validateRunConfiguration(valid).has_value()) << valid.id;
    }
}

TEST(RunTypes, OneCMakeConfigurationPerExecutableTarget) {
    project::CMakeProject model;
    model.targets = {target("engine", project::CMakeTargetType::StaticLibrary),
                     target("editor", project::CMakeTargetType::Executable),
                     target("all-tools", project::CMakeTargetType::Utility),
                     target("plugin", project::CMakeTargetType::SharedLibrary),
                     target("runtime", project::CMakeTargetType::Executable)};
    const std::string source = MemTree::path("src");
    const auto generated = cmakeRunConfigurations(model, source);
    ASSERT_EQ(generated.size(), 2U);
    EXPECT_EQ(generated[0].id, "cmake.editor");
    EXPECT_EQ(generated[0].name, "editor");
    EXPECT_EQ(generated[0].type, RunType::CMakeTarget);
    EXPECT_EQ(generated[0].target, "editor");
    EXPECT_EQ(generated[0].workingDirectory, source);
    EXPECT_EQ(generated[0].beforeLaunch, BeforeLaunch::Build);
    EXPECT_TRUE(generated[0].program.empty()) << "the program is known once the target is built";
    EXPECT_EQ(generated[1].id, "cmake.runtime");
    for (const RunConfiguration& configuration : generated)
        EXPECT_TRUE(validateRunConfiguration(configuration).has_value()) << configuration.id;
    EXPECT_TRUE(cmakeRunConfigurations(project::CMakeProject{}, source).empty());
}

TEST(RunTypes, TheProgramOfABuiltTarget) {
    const std::string exe = MemTree::path("build/Editor.EXE");
    const std::string pdb = MemTree::path("build/Editor.pdb");
    auto program = cmakeTargetProgram(target("editor", project::CMakeTargetType::Executable, {pdb, exe}));
    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(*program, build::test::kWindows ? exe : pdb) << "Windows takes the .exe; POSIX the first artifact";
    // Windows: no .exe (a .com companion) takes the first artifact that is not a .pdb.
    const std::string com = MemTree::path("build/Editor.com");
    program = cmakeTargetProgram(target("console", project::CMakeTargetType::Executable, {pdb, com}));
    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(*program, build::test::kWindows ? com : pdb);
    const auto none = cmakeTargetProgram(target("empty", project::CMakeTargetType::Executable));
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code(), ErrorCode::NotFound);
}

std::string vcxproj(std::string_view debugType, std::string_view releaseType) {
    return std::format(R"(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration"><ConfigurationType>{}</ConfigurationType></PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'" Label="Configuration"><ConfigurationType>{}</ConfigurationType></PropertyGroup>
</Project>
)",
                       debugType, releaseType);
}

// Run.sln: Core (a library), App (an application) and Flip (an application in Debug, a library in
// Release); each solution configuration maps to the project configuration of the same name.
project::SolutionModel solution(MemTree& tree) {
    constexpr std::string_view kCpp = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";
    std::string sln = "Microsoft Visual Studio Solution File, Format Version 12.00\r\n";
    std::string mappings;
    int n = 1;
    const std::pair<std::string_view, std::pair<std::string_view, std::string_view>> projects[] = {
        {"Core", {"StaticLibrary", "StaticLibrary"}},
        {"App", {"Application", "Application"}},
        {"Flip", {"Application", "DynamicLibrary"}}};
    for (const auto& [name, types] : projects) {
        const std::string guid = std::format("{{00000000-0000-4000-8000-00000000000{}}}", n++);
        sln += std::format("Project(\"{}\") = \"{}\", \"{}\\{}.vcxproj\", \"{}\"\r\nEndProject\r\n", kCpp, name, name,
                           name, guid);
        for (const std::string_view config : {"Debug", "Release"})
            mappings += std::format("\t\t{0}.{1}|x64.ActiveCfg = {1}|x64\r\n", guid, config);
        tree.write(std::format("Run/{}/{}.vcxproj", name, name), vcxproj(types.first, types.second));
    }
    sln += "Global\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n\t\tDebug|x64 = Debug|x64\r\n"
           "\t\tRelease|x64 = Release|x64\r\n\tEndGlobalSection\r\n"
           "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n" +
           mappings + "\tEndGlobalSection\r\nEndGlobal\r\n";
    tree.write("Run/Run.sln", sln);
    auto loaded = project::SolutionModel::load(tree.fs, MemTree::path("Run/Run.sln"));
    EXPECT_TRUE(loaded.has_value()) << (loaded ? std::string() : loaded.error().describe());
    return std::move(loaded).value();
}

std::size_t configurationIndex(const project::SolutionModel& model, std::string_view name) {
    const auto all = model.configurations();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].name() == name)
            return i;
    ADD_FAILURE() << "no solution configuration " << name;
    return 0;
}

TEST(RunTypes, OneMSBuildConfigurationPerApplicationProject) {
    MemTree tree;
    const project::SolutionModel model = solution(tree);
    const auto debug = msbuildRunConfigurations(model, configurationIndex(model, "Debug|x64"));
    ASSERT_EQ(debug.size(), 2U);
    EXPECT_EQ(debug[0].id, "msbuild.App");
    EXPECT_EQ(debug[0].name, "App");
    EXPECT_EQ(debug[0].type, RunType::MSBuild);
    EXPECT_EQ(debug[0].target, MemTree::path("Run/App/App.vcxproj"));
    EXPECT_EQ(debug[0].beforeLaunch, BeforeLaunch::Build);
    EXPECT_EQ(debug[1].name, "Flip");
    for (const RunConfiguration& configuration : debug)
        EXPECT_TRUE(validateRunConfiguration(configuration).has_value()) << configuration.id;
    // Release maps Flip to a DLL: not a run configuration there.
    const auto release = msbuildRunConfigurations(model, configurationIndex(model, "Release|x64"));
    ASSERT_EQ(release.size(), 1U);
    EXPECT_EQ(release[0].name, "App");
}

TEST(RunTypes, ANativeConfigurationGetsAFreeId) {
    std::vector<RunConfiguration> taken(1);
    taken[0].id = "native.Tool";
    const std::string program = MemTree::path("bin/tool.exe");
    const RunConfiguration configuration = nativeRunConfiguration("Tool", program, taken);
    EXPECT_EQ(configuration.id, "native.Tool-2");
    EXPECT_EQ(configuration.type, RunType::Native);
    EXPECT_EQ(configuration.program, program);
    EXPECT_TRUE(validateRunConfiguration(configuration).has_value());
    EXPECT_FALSE(validateRunConfiguration(nativeRunConfiguration("Relative", "tool.exe", {})).has_value());
}

RunConfiguration generatedTarget(std::string name) {
    RunConfiguration configuration;
    configuration.id = runConfigurationIdFor(kCMakeIdPrefix, name);
    configuration.name = name;
    configuration.type = RunType::CMakeTarget;
    configuration.target = std::move(name);
    configuration.workingDirectory = MemTree::path("src");
    configuration.beforeLaunch = BeforeLaunch::Build;
    return configuration;
}

TEST(RunConfigurationSet, StoredEditsOverlayGeneratedConfigurationsAndNativeOnesFollow) {
    RunConfigurationSet set;
    set.setGenerated({generatedTarget("editor"), generatedTarget("runtime")});
    ASSERT_EQ(set.all().size(), 2U);
    EXPECT_TRUE(set.isGenerated("cmake.editor"));
    EXPECT_TRUE(set.toStore(set.all()).empty()) << "unedited generated configurations are not stored";

    std::vector<RunConfiguration> edited = set.all();
    edited[0].arguments = {"--offscreen"};
    edited[0].environment = {{"FROST_LOG", std::string("debug")}, {"UNSET_ME", std::nullopt}};
    edited[0].type = RunType::Native; // The model decides the type: the merge keeps cmake-target.
    edited.push_back(nativeRunConfiguration("Tool", MemTree::path("tool.exe"), edited));
    set.replace(edited);
    ASSERT_EQ(set.stored().size(), 2U);
    EXPECT_EQ(set.stored()[0].id, "cmake.editor");
    EXPECT_EQ(set.stored()[1].id, "native.Tool");
    ASSERT_EQ(set.all().size(), 3U);
    EXPECT_EQ(set.all()[0].arguments, std::vector<std::string>{"--offscreen"});
    EXPECT_EQ(set.all()[0].type, RunType::CMakeTarget);
    EXPECT_EQ(set.all()[1].id, "cmake.runtime");
    EXPECT_EQ(set.all()[2].id, "native.Tool");
    EXPECT_FALSE(set.isGenerated("native.Tool"));
    ASSERT_NE(set.find("native.Tool"), nullptr);
    EXPECT_EQ(set.find("missing"), nullptr);

    // The model loses editor (a reconfigure in progress): its edit stays, after the generated ones.
    set.setGenerated({generatedTarget("runtime")});
    ASSERT_EQ(set.all().size(), 3U);
    EXPECT_EQ(set.all()[0].id, "cmake.runtime");
    EXPECT_EQ(set.all()[1].id, "cmake.editor");
    EXPECT_EQ(set.all()[1].arguments, std::vector<std::string>{"--offscreen"});
    EXPECT_FALSE(set.isGenerated("cmake.editor"));

    // Removing a generated configuration in the dialog: it comes back with the next generation.
    set.setGenerated({generatedTarget("editor"), generatedTarget("runtime")});
    std::vector<RunConfiguration> without;
    for (const RunConfiguration& configuration : set.all())
        if (configuration.id != "cmake.runtime")
            without.push_back(configuration);
    set.replace(without);
    EXPECT_NE(set.find("cmake.runtime"), nullptr);
}

} // namespace
} // namespace frost::run
