// SPDX-License-Identifier: Apache-2.0
//
// SLN-6's safe edits against this machine's real tools (Windows; labels "integration" and
// "msbuild"): the shipped ProjectHost (FROST_PROJECT_HOST) adds, renames and removes files of a
// scratch copy of the synthetic generated solution (fixtures/msvc_generated) through the MSBuild
// construction API, and adds and removes projects of its .slnx and .sln through SolutionPersistence.
// The oracles are git (`git diff -U0` of the definitions shows only the intended elements), MSBuild's
// own evaluation (msbuild_oracle.hpp: the items change exactly as intended) and MSBuild's build of
// the edited solution, which must pass. The pinned fixture itself is never edited.

#include "frost/build/output_parsers.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/project_edit.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/solution_model.hpp"

#include "msbuild_oracle.hpp"
#include "paths.hpp"
#include "project_edit_rig.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <sstream>
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
    auto oracle = test::runOracle(
        msbuild, ProjectEvaluationRequest{
                     .projectPath = project, .configuration = configuration, .platform = platform, .solutionPath = {}});
    EXPECT_TRUE(oracle.has_value());
    const auto keys = oracle ? test::oraclePathKeys(*oracle) : std::vector<std::string>{};
    return {keys.begin(), keys.end()};
}

std::string key(const std::string& path) { return detail::pathKey(io::normalizePath(path).value_or(path)); }

bool exists(const std::string& path) { return io::realFileSystem().exists(path).value_or(false); }

void writeFile(const std::string& path, std::string_view content) {
    ASSERT_TRUE(io::realFileSystem().createDirectories(io::parentPath(path)).has_value()) << path;
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
        const auto built = test::msbuild(
            m_msbuild, {scratch.join(solution), "-p:Configuration=Debug", "-p:Platform=x64"}, scratch.root());
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

TEST_F(ProjectEditReal, PreMoveSdkPlanReturnsInverseAndImportBytesWithoutMovingOrWritingAnything) {
    EditScratch scratch("r8 pre-move SDK plan");
    ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("msvc_generated")));
    auto edit = fileEdit(scratch, ProjectEditKind::RenameFiles, "App/App.vcxproj");
    edit.files = {scratch.join("App/app.cpp")};
    edit.renamedTo = {scratch.join("App/renamed.cpp")};
    edit.commit = false;
    edit.beforeFileMove = true;
    edit.transactionId = test::newTransactionId();
    auto before = io::realFileSystem().readFile(edit.definition);
    auto filters = io::realFileSystem().readFile(edit.definition + ".filters");
    auto source = io::realFileSystem().readFile(edit.files[0]);
    ASSERT_TRUE(before && filters && source);
    ASSERT_FALSE(exists(edit.renamedTo[0]));
    auto plan = runProjectEditPlan(io::realProcessLauncher(), {.hostPath = hostPath(), .msbuildPath = m_msbuild}, edit);
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_EQ(plan->projectBefore, *before);
    EXPECT_EQ(plan->filtersBefore, *filters);
    EXPECT_NE(plan->projectAfter.find("renamed.cpp"), std::string::npos);
    EXPECT_FALSE(plan->imports.empty());
    EXPECT_EQ(plan->configurations.size(), 5);
    for (const auto& input : plan->imports)
        EXPECT_EQ(io::realFileSystem().readFile(input.path).value_or("unreadable"), input.bytes);
    EXPECT_EQ(io::realFileSystem().readFile(edit.definition).value_or("unreadable"), *before);
    EXPECT_EQ(io::realFileSystem().readFile(edit.definition + ".filters").value_or("unreadable"), *filters);
    EXPECT_EQ(io::realFileSystem().readFile(edit.files[0]).value_or("unreadable"), *source);
    EXPECT_FALSE(exists(edit.renamedTo[0]));
    EXPECT_FALSE(exists(scratch.join(".frost-project-edit-journal")));
}

TEST_F(ProjectEditReal, PreMoveImportRulesCannotDropOrDuplicateTheNewItem) {
    for (const std::string operation : {"Remove", "Include"}) {
        EditScratch scratch("r8 imported destination rule " + operation);
        ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("msvc_generated")));
        auto edit = fileEdit(scratch, ProjectEditKind::RenameFiles, "App/App.vcxproj");
        edit.files = {scratch.join("App/app.cpp")};
        edit.renamedTo = {scratch.join("App/renamed.cpp")};
        edit.commit = false;
        edit.beforeFileMove = true;
        edit.transactionId = test::newTransactionId();
        auto xml = io::realFileSystem().readFile(edit.definition);
        ASSERT_TRUE(xml);
        const auto at = xml->rfind("</Project>");
        ASSERT_NE(at, std::string::npos);
        xml->insert(at, "<Import Project=\"RenameRule.props\"/>\n");
        writeFile(edit.definition, *xml);
        writeFile(scratch.join("App/RenameRule.props"),
                  "<Project><ItemGroup><ClCompile " + operation + "=\"renamed.cpp\"/></ItemGroup></Project>");
        auto plan =
            runProjectEditPlan(io::realProcessLauncher(), {.hostPath = hostPath(), .msbuildPath = m_msbuild}, edit);
        EXPECT_FALSE(plan) << operation;
        EXPECT_EQ(io::realFileSystem().readFile(edit.definition).value_or("unreadable"), *xml);
        EXPECT_TRUE(exists(edit.files[0]));
        EXPECT_FALSE(exists(edit.renamedTo[0]));
    }
}

TEST_F(ProjectEditReal, PreMoveImportConditionsAndPropertyFunctionsCannotChangeAfterTheMove) {
    for (const std::string source :
         {"app.cpp", "renamed.cpp", "property-function", "property-reassigned", "instance-property"}) {
        EditScratch scratch("r8 file-dependent import " + source);
        ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("msvc_generated")));
        auto edit = fileEdit(scratch, ProjectEditKind::RenameFiles, "App/App.vcxproj");
        edit.files = {scratch.join("App/app.cpp")};
        edit.renamedTo = {scratch.join("App/renamed.cpp")};
        edit.commit = false;
        edit.beforeFileMove = true;
        edit.transactionId = test::newTransactionId();
        auto xml = io::realFileSystem().readFile(edit.definition);
        ASSERT_TRUE(xml);
        const auto at = xml->rfind("</Project>");
        ASSERT_NE(at, std::string::npos);
        const std::string rule =
            source == "instance-property"
                ? "<PropertyGroup><RenameInput>app.cpp</RenameInput><UseRenameRule "
                  "Condition=\"Exists('$(RenameInput.ToUpper())')\">true"
                  "</UseRenameRule><RenameInput>unrelated.cpp</RenameInput></PropertyGroup>"
                  "<Import Project=\"RenameRule.props\" Condition=\"'$(UseRenameRule)'=='true'\"/>"
            : source == "property-reassigned"
                ? "<PropertyGroup><RenameInput>app.cpp</RenameInput><UseRenameRule "
                  "Condition=\"Exists('$(RenameInput)')\">true"
                  "</UseRenameRule><RenameInput>unrelated.cpp</RenameInput></PropertyGroup>"
                  "<Import Project=\"RenameRule.props\" Condition=\"'$(UseRenameRule)'=='true'\"/>"
            : source == "property-function"
                ? "<PropertyGroup><UseRenameRule>$([System.IO.File]::Exists('$(MSBuildProjectDirectory)\\app.cpp'))"
                  "</UseRenameRule></PropertyGroup><Import Project=\"RenameRule.props\" "
                  "Condition=\"'$(UseRenameRule)'=='true'\"/>"
                : "<Import Project=\"RenameRule.props\" Condition=\"Exists('" + source + "')\"/>";
        xml->insert(at, rule);
        writeFile(edit.definition, *xml);
        writeFile(scratch.join("App/RenameRule.props"),
                  "<Project><PropertyGroup><RenameRule>true</RenameRule></PropertyGroup></Project>");
        auto plan =
            runProjectEditPlan(io::realProcessLauncher(), {.hostPath = hostPath(), .msbuildPath = m_msbuild}, edit);
        EXPECT_FALSE(plan) << source;
        EXPECT_EQ(io::realFileSystem().readFile(edit.definition).value_or("unreadable"), *xml);
        EXPECT_TRUE(exists(edit.files[0]));
        EXPECT_FALSE(exists(edit.renamedTo[0]));
    }
}

TEST_F(ProjectEditReal, BinaryLogReplayPreservesStructuredBuildErrors) {
    EditScratch scratch("r10 structured binlog");
    ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("msvc_generated")));
    writeFile(scratch.join("App/main.cpp"), "#error FROST_BINLOG_ERROR\n");
    const auto log = scratch.join("failure.binlog");
    const auto built =
        test::msbuild(m_msbuild,
                      {scratch.join("App/App.vcxproj"), "-t:ClCompile", "-p:Configuration=Debug", "-p:Platform=x64",
                       "-logger:FrostBuildLogger," + hostPath(), "-bl:" + log + ";ProjectImports=None"},
                      scratch.root());
    ASSERT_NE(built.exitCode, 0);
    ASSERT_TRUE(exists(log));
    const auto collect = [](const std::string& output) {
        frost::build::DiagnosticSet diagnostics;
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line))
            if (auto diagnostic = frost::build::parseStructuredMsBuildLine(line))
                diagnostics.add(std::move(*diagnostic));
        return diagnostics.diagnostics();
    };
    const auto live = collect(built.stdoutBytes);
    ASSERT_FALSE(live.empty()) << built.stdoutBytes << built.stderrBytes;
    io::ProcessSpec spec;
    spec.executable = hostPath();
    spec.arguments = {std::string(io::parentPath(m_msbuild)), "--replay-binlog", log};
    spec.workingDirectory = scratch.root();
    const auto replay = io::runToCompletion(io::realProcessLauncher(), spec, {}, std::chrono::seconds(120));
    ASSERT_TRUE(replay) << replay.error().describe();
    ASSERT_EQ(replay->exitCode, 0) << replay->stderrBytes;
    EXPECT_EQ(collect(replay->stdoutBytes), live);
}

TEST_F(ProjectEditReal, NMakeProjectsBuildAndExposeEvaluatedIntelligence) {
    EditScratch scratch("r10 nmake");
    ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("nmake_project")));
    auto client =
        ProjectHostClient::start(io::realProcessLauncher(), {.hostPath = hostPath(), .msbuildPath = m_msbuild});
    ASSERT_TRUE(client) << client.error().describe();
    auto lines = (*client)->clCommandLines(
        {.projectPath = scratch.join("NMake.vcxproj"), .configuration = "Debug", .platform = "x64"});
    ASSERT_TRUE(lines) << lines.error().describe();
    const auto commands = clangdCommands(*lines);
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_TRUE(
        std::ranges::any_of(commands[0].arguments, [](const auto& argument) { return argument == "/DFROST_NMAKE=7"; }));
    EXPECT_TRUE(std::ranges::any_of(commands[0].arguments, [](const auto& argument) {
        return argument.starts_with("/I") && argument.find("include") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(commands[0].arguments, [](const auto& argument) {
        return argument.starts_with("/FI") && argument.find("forced.h") != std::string::npos;
    }));
    EXPECT_TRUE(
        std::ranges::any_of(commands[0].arguments, [](const auto& argument) { return argument == "/std:c++20"; }));
    expectBuilds(scratch, "NMake.vcxproj");
    EXPECT_TRUE(exists(scratch.join("main.obj")));
}

TEST_F(ProjectEditReal, PropertyPagesEditMetadataInOneConfigurationAndRefreshClangdFlags) {
    EditScratch scratch("r10 property pages");
    ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("msvc_generated")));
    auto edit = fileEdit(scratch, ProjectEditKind::SetProperties, "App/App.vcxproj");
    edit.propertyUpdates = {
        {"Prop.CharacterSet", "Unicode"},
        {"ClCompile.AdditionalIncludeDirectories", "$(ProjectDir)page-includes;%(AdditionalIncludeDirectories)"},
        {"ClCompile.PreprocessorDefinitions", "FROST_PROPERTY_PAGE=1;%(PreprocessorDefinitions)"}};
    const auto filtersBefore = io::realFileSystem().readFile(scratch.join("App/App.vcxproj.filters"));
    auto edited = test::runEdit(hostPath(), m_msbuild, edit);
    ASSERT_TRUE(edited) << edited.error().describe();
    EXPECT_EQ(io::realFileSystem().readFile(scratch.join("App/App.vcxproj.filters")).value_or(""),
              filtersBefore.value_or(""));
    auto client =
        ProjectHostClient::start(io::realProcessLauncher(), {.hostPath = hostPath(), .msbuildPath = m_msbuild});
    ASSERT_TRUE(client) << client.error().describe();
    ProjectEvaluationRequest request{.projectPath = edit.definition, .configuration = "Debug", .platform = "x64"};
    request.properties = {"ClCompile.AdditionalIncludeDirectories", "ClCompile.PreprocessorDefinitions",
                          "Prop.ConfigurationType"};
    auto evaluated = (*client)->evaluate(request);
    ASSERT_TRUE(evaluated) << evaluated.error().describe();
    EXPECT_NE(evaluated->properties.at("ClCompile.AdditionalIncludeDirectories").find("page-includes"),
              std::string::npos);
    EXPECT_NE(evaluated->properties.at("ClCompile.PreprocessorDefinitions").find("FROST_PROPERTY_PAGE=1"),
              std::string::npos);
    auto commands = (*client)->clCommandLines(request);
    ASSERT_TRUE(commands) << commands.error().describe();
    const auto planned = io::realFileSystem().readFile(edit.definition);
    ASSERT_TRUE(planned);
    EXPECT_LT(planned->find("<CharacterSet>Unicode</CharacterSet>"), planned->find("Microsoft.Cpp.props"));
    const auto clangd = clangdCommands(*commands);
    ASSERT_FALSE(clangd.empty());
    EXPECT_TRUE(std::ranges::any_of(clangd.front().arguments, [](const auto& argument) {
        return argument.find("FROST_PROPERTY_PAGE=1") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(clangd.front().arguments, [](const auto& argument) {
        return argument.find("page-includes") != std::string::npos;
    }));
    request.configuration = "Release";
    auto release = (*client)->clCommandLines(request);
    ASSERT_TRUE(release) << release.error().describe();
    for (const auto& command : clangdCommands(*release))
        EXPECT_FALSE(std::ranges::any_of(command.arguments, [](const auto& argument) {
            return argument.find("FROST_PROPERTY_PAGE=1") != std::string::npos;
        }));
    expectBuilds(scratch, "Generated.sln");
}

TEST_F(ProjectEditReal, NativeDependencyPropertiesRefuseHooksFromDirectoryBuildPropsWithoutWriting) {
    EditScratch scratch("native dependency imported hook refusal");
    ASSERT_NO_FATAL_FAILURE(scratch.copyAndCommit(test::fixture("msvc_generated")));
    writeFile(scratch.join("existing-hook.props"),
              "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"/>\n");
    writeFile(scratch.join("existing-hook.targets"),
              "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"/>\n");
    writeFile(
        scratch.join("Directory.Build.props"),
        "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"><PropertyGroup>"
        "<ForceImportAfterCppProps>$(MSBuildThisFileDirectory)existing-hook.props</ForceImportAfterCppProps>"
        "<ForceImportAfterCppTargets>$(MSBuildThisFileDirectory)existing-hook.targets</ForceImportAfterCppTargets>"
        "</PropertyGroup></Project>\n");
    const auto projectBefore = io::realFileSystem().readFile(scratch.join("App/App.vcxproj"));
    ASSERT_TRUE(projectBefore);
    for (const auto* hook : {"ForceImportAfterCppProps", "ForceImportAfterCppTargets"}) {
        auto edit = fileEdit(scratch, ProjectEditKind::SetProperties, "App/App.vcxproj");
        edit.configuration = "Debug";
        edit.platform = "x64";
        edit.earlyProperties = true;
        edit.propertyUpdates = {{std::string("Prop.") + hook, scratch.join("replacement.props")}};
        auto refused = test::runEdit(hostPath(), m_msbuild, edit);
        ASSERT_FALSE(refused) << hook;
        EXPECT_NE(refused.error().message().find("evaluated import hook"), std::string::npos)
            << refused.error().describe();
        EXPECT_NE(refused.error().message().find(hook), std::string::npos);
        EXPECT_EQ(io::realFileSystem().readFile(scratch.join("App/App.vcxproj")).value_or(""), *projectBefore);
    }
}
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
    EXPECT_EQ(
        sorted(scratch.changedLines(definitions)),
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
        auto result = test::runEdit(hostPath(), m_msbuild, std::move(edit));
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

    auto added = test::runEdit(hostPath(), m_msbuild,
                               edit(ProjectEditKind::AddProject, "Generated.slnx", "Extra/Extra.vcxproj"));
    ASSERT_TRUE(added) << added.error().describe();
    EXPECT_EQ(scratch.changedLines({"Generated.slnx"}),
              (std::vector<std::string>{
                  "+  <Project Path=\"Extra/Extra.vcxproj\" Id=\"6a1d0c1e-0b0e-4c1b-9e51-0000000c0e09\" />"}));
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
    removed = test::runEdit(hostPath(), m_msbuild,
                            edit(ProjectEditKind::RemoveProject, "Full.sln", "Renderer/Renderer.vcxproj"));
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
    auto again =
        test::runEdit(hostPath(), m_msbuild, edit(ProjectEditKind::AddProject, "Full.sln", "Extra/Extra.vcxproj"));
    ASSERT_FALSE(again);
    EXPECT_NE(again.error().message().find("already contains"), std::string::npos) << again.error().describe();
}

} // namespace
} // namespace frost::project
