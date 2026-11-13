// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the MSBuild-evaluated compile-commands provider (msbuild_compile_commands.hpp)
// without MSBuild: reading the evaluation JSON, the clang-cl flags from evaluated metadata, the
// import list of a -pp output, the evaluation command line, the trust rule (no launch when the
// solution is untrusted) and the fallback to the static synthesizer when evaluation fails
// (scripted launcher; Windows only, because off Windows the provider never evaluates).

#include "frost/io/file_system.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/sched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using frost::lang::CompileCommandsSource;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;

std::string fixture(std::string relative) {
    if (kWindows) {
        std::ranges::replace(relative, '/', '\\');
    }
    const std::string base = frost::io::normalizePath(FROST_LANG_FIXTURE_DIR).value();
    return frost::io::normalizePath(frost::io::joinPath(base, relative).value()).value();
}

bool has(const std::vector<std::string>& args, std::string_view value) {
    return std::ranges::find(args, value) != args.end();
}

// What MSBuild 17.14 prints for -getItem:ClCompile -getProperty:... (trimmed).
constexpr std::string_view kEvaluation = R"({
  "Properties": {
    "IncludePath": "C:\\VS\\VC\\include;;C:\\Kits\\ucrt;",
    "ExternalIncludePath": "C:\\VS\\VC\\include;C:\\Kits\\um",
    "MSBuildAllProjects": "C:\\p\\App.vcxproj;;C:\\VS\\x.targets",
    "Platform": "x64"
  },
  "Items": {
    "ClCompile": [
      {
        "Identity": "src\\main.cpp",
        "FullPath": "C:\\p\\src\\main.cpp",
        "PreprocessorDefinitions": "SDK_FEATURE=1;_UNICODE;UNICODE;",
        "UndefinePreprocessorDefinitions": "OLD",
        "AdditionalIncludeDirectories": "C:\\sdk\\include;rel\\inc;",
        "LanguageStandard": "stdcpp20",
        "RuntimeLibrary": "MultiThreadedDebugDLL",
        "ExceptionHandling": "Async",
        "ConformanceMode": "true",
        "TreatWChar_tAsBuiltInType": "false",
        "PrecompiledHeader": "Use",
        "PrecompiledHeaderFile": "pch.h",
        "ForcedIncludeFiles": "force.h",
        "AdditionalOptions": "/Zc:__cplusplus /MP4 /Yupch.h \"/DQUOTED=a b\"",
        "CompileAs": "Default"
      },
      {
        "Identity": "src\\c.c",
        "FullPath": "C:\\p\\src\\c.c",
        "LanguageStandard_C": "stdc17",
        "CompileAs": "Default"
      },
      {
        "Identity": "src\\skip.cpp",
        "FullPath": "C:\\p\\src\\skip.cpp",
        "ExcludedFromBuild": "true"
      }
    ]
  }
})";

TEST(MsBuildEvaluation, ParsesTheQueryOutput) {
    auto parsed = frost::lang::parseMsBuildEvaluation(std::string("noise before\n") + std::string(kEvaluation));
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    EXPECT_EQ(parsed->properties.at("platform"), "x64");
    ASSERT_EQ(parsed->clCompile.size(), 3u);
    EXPECT_EQ(parsed->clCompile[0].fullPath, "C:\\p\\src\\main.cpp");
    EXPECT_EQ(parsed->clCompile[0].metadata.at("languagestandard"), "stdcpp20");
    EXPECT_FALSE(frost::lang::parseMsBuildEvaluation("error MSB4019: not found").has_value());
}

TEST(MsBuildEvaluation, ParsesHeaderMembershipWhenProvided) {
    auto parsed = frost::lang::parseMsBuildEvaluation(
        R"({"Items":{"ClCompile":[],"ClInclude":[{"FullPath":"C:\\p\\include\\a.h"}]}})");
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(parsed->clInclude);
    ASSERT_EQ(parsed->clInclude->size(), 1u);
    EXPECT_EQ(parsed->clInclude->front().fullPath, "C:\\p\\include\\a.h");
    auto absent = frost::lang::parseMsBuildEvaluation(R"({"Items":{"ClCompile":[]}})");
    ASSERT_TRUE(absent);
    EXPECT_FALSE(absent->clInclude);
}

TEST(MsBuildEvaluation, RejectsMalformedItemMembershipButAcceptsExplicitEmpty) {
    for (const std::string_view invalid :
         {R"({})", R"({"Items":{}})", R"({"Items":{"ClCompile":{}}})", R"({"Items":{"ClCompile":[null]}})",
          R"({"Items":{"ClCompile":[{}]}})", R"({"Items":{"ClCompile":[{"FullPath":17}]}})",
          R"({"Items":{"ClCompile":[{"FullPath":"relative.cpp"}]}})", R"({"Items":{"ClCompile":[],"ClInclude":{}}})",
          R"({"Properties":[],"Items":{"ClCompile":[]}})"}) {
        EXPECT_FALSE(frost::lang::parseMsBuildEvaluation(invalid)) << invalid;
    }
    auto empty = frost::lang::parseMsBuildEvaluation(R"({"Items":{"ClCompile":[],"ClInclude":[]}})");
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->clCompile.empty());
    ASSERT_TRUE(empty->clInclude);
    EXPECT_TRUE(empty->clInclude->empty());
}

TEST(MsBuildEvaluation, MapsEvaluatedMetadataToClangClFlags) {
    if (!kWindows) {
        GTEST_SKIP() << "the canned evaluation uses Windows paths";
    }
    const auto evaluation = frost::lang::parseMsBuildEvaluation(kEvaluation).value();
    const auto commands = frost::lang::commandsFromEvaluation(evaluation, "C:\\p", "Debug", "x64");
    ASSERT_EQ(commands.size(), 2u); // ExcludedFromBuild is skipped.
    const auto& args = commands[0].arguments;
    EXPECT_EQ(args[0], "clang-cl");
    EXPECT_TRUE(has(args, "--driver-mode=cl"));
    EXPECT_TRUE(has(args, "--target=x86_64-pc-windows-msvc"));
    EXPECT_TRUE(has(args, "/MDd"));
    EXPECT_TRUE(has(args, "/EHa"));
    EXPECT_TRUE(has(args, "/permissive-"));
    EXPECT_TRUE(has(args, "/Zc:wchar_t-"));
    EXPECT_TRUE(has(args, "/DSDK_FEATURE=1"));
    EXPECT_TRUE(has(args, "/DUNICODE"));
    EXPECT_TRUE(has(args, "/UOLD"));
    EXPECT_TRUE(has(args, "/IC:\\sdk\\include"));
    EXPECT_TRUE(has(args, "/IC:\\p\\rel\\inc"));
    EXPECT_TRUE(has(args, "/imsvcC:\\VS\\VC\\include"));
    EXPECT_TRUE(has(args, "/imsvcC:\\Kits\\ucrt"));
    EXPECT_TRUE(has(args, "/imsvcC:\\Kits\\um"));
    EXPECT_EQ(std::ranges::count(args, "/imsvcC:\\VS\\VC\\include"), 1); // Deduplicated.
    EXPECT_TRUE(has(args, "/FIforce.h"));
    EXPECT_TRUE(has(args, "/FIpch.h"));
    EXPECT_TRUE(has(args, "/std:c++20"));
    EXPECT_TRUE(has(args, "/Zc:__cplusplus"));
    EXPECT_TRUE(has(args, "/DQUOTED=a b"));
    EXPECT_FALSE(has(args, "/MP4"));
    EXPECT_FALSE(has(args, "/Yupch.h"));
    EXPECT_EQ(args[args.size() - 2], "/c");
    EXPECT_EQ(args.back(), "C:\\p\\src\\main.cpp");
    EXPECT_TRUE(has(commands[1].arguments, "/std:c17"));
    EXPECT_FALSE(has(commands[1].arguments, "/std:c++14"));
}

TEST(MsBuildEvaluation, ReadsTheImportsOfAPreprocessedProject) {
    constexpr std::string_view kPreprocessed = R"(<?xml version="1.0" encoding="utf-8"?>
<!--
============================================================================================================================================
C:\p\App\App.vcxproj
============================================================================================================================================
-->
<Project>
  <!--
============================================================================================================================================
  <Import Project="..\sdk\Sdk.props">

C:\p\sdk\Sdk.props
============================================================================================================================================
-->
  <!-- an ordinary comment C:\not\a\banner.props -->
  <!--
============================================================================================================================================
  </Import>

C:\p\App\App.vcxproj
============================================================================================================================================
-->
  <!--
============================================================================================================================================
  <Import Project="x.props">

C:\Users\a\E__dev\x.props
============================================================================================================================================
-->
</Project>)";
    const auto imports = frost::lang::parsePreprocessedImports(kPreprocessed);
    EXPECT_EQ(imports, (std::vector<std::string>{"C:\\p\\App\\App.vcxproj", "C:\\p\\sdk\\Sdk.props",
                                                 "C:\\Users\\a\\E__dev\\x.props"}));
}

TEST(MsBuildEvaluation, CommandLine) {
    const std::string root = kWindows ? "C:\\w" : "/w";
    const auto in = [&](const std::string& name) { return frost::io::joinPath(root, name).value(); };
    frost::lang::MsBuildEvaluationRequest request{.msbuildPath = in("MSBuild.exe"),
                                                  .projectPath = in("App.vcxproj"),
                                                  .solutionPath = in("My;App.sln"),
                                                  .configuration = "Debug",
                                                  .platform = "x64",
                                                  .preprocessTo = {},
                                                  .environment = {{"FROST_TEST_SDK_ROOT", in("sdk")}}};
    auto spec = frost::lang::msbuildEvaluationSpec(request);
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    const auto& args = spec->arguments;
    EXPECT_EQ(args[0], in("App.vcxproj"));
    EXPECT_TRUE(has(args, "-getItem:ClCompile;ClInclude"));
    EXPECT_TRUE(has(args, "-getProperty:IncludePath;ExternalIncludePath;MSBuildAllProjects;Configuration;Platform;"
                          "ProjectDir"));
    EXPECT_TRUE(has(args, "-p:Configuration=Debug"));
    EXPECT_TRUE(has(args, "-p:Platform=x64"));
    EXPECT_TRUE(has(args, "-nodeReuse:false"));
    EXPECT_TRUE(has(args, "-p:SolutionName=My%3BApp")); // MSBuild-escaped.
    EXPECT_TRUE(has(args, "-p:SolutionExt=.sln"));
    EXPECT_EQ(spec->workingDirectory, root);
    EXPECT_TRUE(std::ranges::any_of(spec->environment, [](const auto& entry) {
        return entry.first == "DOTNET_CLI_UI_LANGUAGE" && entry.second == "en-US";
    }));
    EXPECT_TRUE(
        std::ranges::any_of(spec->environment, [](const auto& entry) { return entry.first == "FROST_TEST_SDK_ROOT"; }));

    request.preprocessTo = in("out.pp.xml");
    auto pp = frost::lang::msbuildEvaluationSpec(request);
    ASSERT_TRUE(pp.has_value());
    EXPECT_TRUE(has(pp->arguments, "-pp:" + in("out.pp.xml")));
    EXPECT_FALSE(has(pp->arguments, "-getItem:ClCompile;ClInclude"));

    request.configuration.clear();
    EXPECT_FALSE(frost::lang::msbuildEvaluationSpec(request).has_value());
}

TEST(MsBuildProvider, UntrustedSolutionsAreNeverEvaluated) {
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(), fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    frost::io::ScriptedProcessLauncher launcher;
    frost::io::MemFs fs(frost::systemClock());
    frost::lang::MsBuildProviderOptions options;
    options.msbuildPath = kWindows ? "C:\\VS\\MSBuild.exe" : "/vs/MSBuild";
    options.trusted = [] { return false; };
    frost::lang::MsBuildCompileCommandsProvider provider(
        launcher, fs, std::make_shared<const frost::project::SolutionModel>(std::move(*model)),
        kWindows ? "C:\\cache" : "/cache", options);
    auto located = provider.locate();
    ASSERT_TRUE(located.has_value());
    EXPECT_FALSE(located->has_value());
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(MsBuildProvider, StreamsFirstSolutionProjectWhileNextEvaluationIsHeld) {
    if (!kWindows)
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(),
                                                     fixture("../../project/fixtures/sdk_solution/SdkApp.sln"));
    ASSERT_TRUE(model) << model.error().describe();
    const auto shared = std::make_shared<const frost::project::SolutionModel>(std::move(*model));
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = std::string(kEvaluation)});
    launcher.expect({.neverExits = true});
    frost::io::MemFs fs(frost::systemClock());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(pool);
    auto streamed = std::make_shared<frost::lang::StreamedProjectContexts>(retirement, shared);
    frost::lang::MsBuildProviderOptions options;
    options.msbuildPath = "C:\\VS\\MSBuild.exe";
    options.trusted = [] { return true; };
    options.configuration = "Debug|x64";
    options.streamedProjects = streamed;
    frost::lang::MsBuildCompileCommandsProvider provider(launcher, fs, shared, "C:\\cache", options);
    std::jthread worker([&](std::stop_token stop) { (void)provider.locate(std::move(stop)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline &&
           (streamed->snapshot().size() < 2 || launcher.launches().size() < 2))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto first = streamed->snapshot();
    const auto launches = launcher.launches();
    worker.request_stop();
    worker.join();
    EXPECT_TRUE(streamed->snapshot().empty());
    ASSERT_GE(first.size(), 2u);
    EXPECT_EQ(first[0]->state, frost::lang::ProjectEvaluationState::Evaluated);
    EXPECT_EQ(first[1]->state, frost::lang::ProjectEvaluationState::Pending);
    ASSERT_GE(launches.size(), 2u);
    ASSERT_FALSE(first[0]->commands.empty());
    const auto id = shared->findProjectByPath(first[0]->projectPath);
    ASSERT_TRUE(id);
    const auto config = shared->findConfiguration("Debug|x64");
    ASSERT_TRUE(config);
    ASSERT_TRUE(shared->projectConfiguration(*id, *config));
    for (std::size_t p = 0; p < shared->projects().size(); ++p) {
        const frost::project::ProjectId candidate{static_cast<std::uint32_t>(p)};
        const auto& info = shared->project(candidate);
        const auto& mapping = shared->projectConfiguration(candidate, *config);
        if (info.vcxproj && mapping && !info.path.empty()) {
            EXPECT_EQ(*id, candidate);
            break;
        }
    }
    EXPECT_EQ(launches[0].spec.arguments.front(), first[0]->projectPath);
}

TEST(MsBuildProvider, EarlierNonBuildingProjectPrecedesBuildingProjectInPartialDatabase) {
    if (!kWindows)
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    frost::io::MemFs fs(frost::systemClock());
    fs.addRoot("C:\\");
    ASSERT_TRUE(fs.createDirectories("C:\\solution"));
    ASSERT_TRUE(
        fs.writeFileAtomic("C:\\solution\\test.sln", R"(Microsoft Visual Studio Solution File, Format Version 12.00
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "First", "first.vcxproj", "{A7A7A7A7-0001-4000-8000-000000000001}"
EndProject
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "Second", "second.vcxproj", "{A7A7A7A7-0002-4000-8000-000000000002}"
EndProject
Global
 GlobalSection(SolutionConfigurationPlatforms) = preSolution
  Debug|x64 = Debug|x64
 EndGlobalSection
 GlobalSection(ProjectConfigurationPlatforms) = postSolution
  {A7A7A7A7-0001-4000-8000-000000000001}.Debug|x64.ActiveCfg = Debug|x64
  {A7A7A7A7-0002-4000-8000-000000000002}.Debug|x64.ActiveCfg = Debug|x64
  {A7A7A7A7-0002-4000-8000-000000000002}.Debug|x64.Build.0 = Debug|x64
 EndGlobalSection
EndGlobal
)"));
    constexpr std::string_view project =
        R"(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemDefinitionGroup><ClCompile><PreprocessorDefinitions>FIRST_FALLBACK=1</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup><ItemGroup><ClCompile Include="C:\p\src\main.cpp" /></ItemGroup></Project>)";
    ASSERT_TRUE(fs.writeFileAtomic("C:\\solution\\first.vcxproj", project));
    ASSERT_TRUE(fs.writeFileAtomic("C:\\solution\\second.vcxproj", project));
    auto model = frost::project::SolutionModel::load(fs, "C:\\solution\\test.sln");
    ASSERT_TRUE(model);
    auto shared = std::make_shared<const frost::project::SolutionModel>(std::move(*model));
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = std::string(kEvaluation)});
    launcher.expect({.neverExits = true, .outputHeld = true});
    frost::lang::MsBuildProviderOptions options;
    options.msbuildPath = "C:\\VS\\MSBuild.exe";
    options.configuration = "Debug|x64";
    options.trusted = [] { return true; };
    auto evaluation =
        std::make_shared<frost::lang::IndependentProjectEvaluation>(launcher, fs, shared, "C:\\cache", options);
    auto first = evaluation->locate();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && launcher.launches().size() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto launches = launcher.launches();
    evaluation->stopAndJoin();
    ASSERT_TRUE(first);
    ASSERT_TRUE(*first);
    ASSERT_GE(launches.size(), 2u);
    EXPECT_EQ(launches[0].spec.arguments.front(), "C:\\solution\\first.vcxproj");
    EXPECT_EQ(launches[1].spec.arguments.front(), "C:\\solution\\second.vcxproj");
    auto database = fs.readFile((**first).file);
    ASSERT_TRUE(database);
    EXPECT_NE(database->find("SDK_FEATURE=1"), std::string::npos);

    // If the first evaluation fails, its static command must still precede the later
    // project's evaluated command in both publications for their shared source file.
    frost::io::ScriptedProcessLauncher fallbackLauncher;
    fallbackLauncher.expect({.exitCode = 1});
    fallbackLauncher.expect({.stdoutBytes = std::string(kEvaluation)});
    std::string partial;
    options.onFirstDatabase = [&](const frost::lang::CompileCommandsLocation& location) {
        if (auto content = fs.readFile(location.file))
            partial = std::move(*content);
    };
    frost::lang::MsBuildCompileCommandsProvider provider(fallbackLauncher, fs, shared, "C:\\fallback-cache", options);
    auto final = provider.locate();
    ASSERT_TRUE(final);
    ASSERT_TRUE(*final);
    auto complete = fs.readFile((**final).file);
    ASSERT_TRUE(complete);
    for (const auto* content : {&partial, &*complete}) {
        const auto fallback = content->find("FIRST_FALLBACK=1");
        const auto evaluated = content->find("SDK_FEATURE=1");
        ASSERT_NE(fallback, std::string::npos);
        ASSERT_NE(evaluated, std::string::npos);
        EXPECT_LT(fallback, evaluated);
    }
}

TEST(MsBuildProvider, InvalidatedStreamRejectsLateProjectRecords) {
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(pool);
    frost::lang::StreamedProjectContexts streamed(retirement, nullptr);
    streamed.publish({.projectPath = "first", .configuration = {}, .platform = {}, .commands = {}});
    ASSERT_EQ(streamed.snapshot().size(), 1u);
    streamed.invalidate();
    streamed.publish({.projectPath = "late", .configuration = {}, .platform = {}, .commands = {}});
    EXPECT_TRUE(streamed.snapshot().empty());
}

TEST(MsBuildProvider, FailedProjectKeepsStaticTreeWhileNextIsPending) {
    if (!kWindows)
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(),
                                                     fixture("../../project/fixtures/sdk_solution/SdkApp.sln"));
    ASSERT_TRUE(model);
    auto shared = std::make_shared<const frost::project::SolutionModel>(std::move(*model));
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.exitCode = 1});
    launcher.expect({.neverExits = true, .outputHeld = true});
    frost::io::MemFs fs(frost::systemClock());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(pool);
    auto streamed = std::make_shared<frost::lang::StreamedProjectContexts>(retirement, shared);
    frost::lang::MsBuildProviderOptions options;
    options.msbuildPath = "C:\\VS\\MSBuild.exe";
    options.trusted = [] { return true; };
    options.configuration = "Debug|x64";
    options.streamedProjects = streamed;
    frost::lang::MsBuildCompileCommandsProvider provider(launcher, fs, shared, "C:\\cache", options);
    std::jthread worker([&](std::stop_token stop) { (void)provider.locate(std::move(stop)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && launcher.launches().size() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto records = streamed->snapshot();
    const auto overlay = streamed->modelSnapshot();
    worker.request_stop();
    worker.join();
    ASSERT_GE(records.size(), 2u);
    EXPECT_EQ(records[0]->state, frost::lang::ProjectEvaluationState::Failed);
    EXPECT_EQ(records[1]->state, frost::lang::ProjectEvaluationState::Pending);
    EXPECT_EQ(overlay, shared);
}

TEST(MsBuildProvider, MalformedEvaluationKeepsTheStaticProjectTree) {
    if (!kWindows)
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(),
                                                     fixture("../../project/fixtures/sdk_solution/SdkApp.sln"));
    ASSERT_TRUE(model);
    auto shared = std::make_shared<const frost::project::SolutionModel>(std::move(*model));
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = R"({"Items":{"ClCompile":[{}]}})"});
    launcher.expect({.neverExits = true, .outputHeld = true});
    frost::io::MemFs fs(frost::systemClock());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(pool);
    auto streamed = std::make_shared<frost::lang::StreamedProjectContexts>(retirement, shared);
    frost::lang::MsBuildProviderOptions options;
    options.msbuildPath = "C:\\VS\\MSBuild.exe";
    options.trusted = [] { return true; };
    options.configuration = "Debug|x64";
    options.streamedProjects = streamed;
    frost::lang::MsBuildCompileCommandsProvider provider(launcher, fs, shared, "C:\\cache", options);
    std::jthread worker([&](std::stop_token stop) { (void)provider.locate(std::move(stop)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && launcher.launches().size() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto records = streamed->snapshot();
    const auto overlay = streamed->modelSnapshot();
    worker.request_stop();
    worker.join();
    ASSERT_GE(records.size(), 2u);
    EXPECT_EQ(records[0]->state, frost::lang::ProjectEvaluationState::Failed);
    EXPECT_EQ(records[1]->state, frost::lang::ProjectEvaluationState::Pending);
    EXPECT_EQ(overlay, shared);
}

TEST(MsBuildProvider, StreamedHeaderAbsenceAndAuthoritativeEmptyRemainDistinctAcrossRepublication) {
    auto loaded = frost::project::SolutionModel::load(frost::io::realFileSystem(),
                                                      fixture("../../project/fixtures/sdk_solution/SdkApp.sln"));
    ASSERT_TRUE(loaded);
    auto base = std::make_shared<const frost::project::SolutionModel>(std::move(*loaded));
    const std::string project = fixture("../../project/fixtures/sdk_solution/Libraries/Core/Core.vcxproj");
    const std::string header = fixture("../../project/fixtures/sdk_solution/Libraries/Core/include/core/core.h");
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 1});
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(pool);
    frost::lang::StreamedProjectContexts streamed(retirement, base);
    frost::lang::StreamedProjectContext record{.projectPath = project,
                                               .configuration = "Debug",
                                               .platform = "x64",
                                               .state = frost::lang::ProjectEvaluationState::Evaluated,
                                               .commands = {}};
    streamed.publish(record); // No ClInclude result: the static headers remain authoritative fallback.
    ASSERT_TRUE(streamed.modelSnapshot()->findFileNode(header));
    record.headers = std::vector<std::string>{};
    streamed.publish(record); // An actual empty result must remove this project's headers.
    EXPECT_FALSE(streamed.modelSnapshot()->findFileNode(header));
    record.headers = std::vector<std::string>{header};
    streamed.publish(record);
    ASSERT_TRUE(streamed.modelSnapshot()->findFileNode(header))
        << "a later successful evaluation must make a previously removed header reachable again";
    streamed.clear();
    EXPECT_EQ(streamed.modelSnapshot(), base); // Normal refresh still resets to its static base.
}

TEST(MsBuildProvider, MissingMsBuildMarksProjectsFailed) {
    if (!kWindows)
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(),
                                                     fixture("../../project/fixtures/sdk_solution/SdkApp.sln"));
    ASSERT_TRUE(model);
    auto shared = std::make_shared<const frost::project::SolutionModel>(std::move(*model));
    frost::io::ScriptedProcessLauncher launcher;
    frost::io::MemFs fs(frost::systemClock());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(pool);
    auto streamed = std::make_shared<frost::lang::StreamedProjectContexts>(retirement, shared);
    frost::lang::MsBuildProviderOptions options;
    options.trusted = [] { return true; };
    options.configuration = "Debug|x64";
    options.streamedProjects = streamed;
    frost::lang::MsBuildCompileCommandsProvider provider(launcher, fs, shared, "C:\\cache", options);
    EXPECT_FALSE(provider.locate());
    const auto records = streamed->snapshot();
    ASSERT_GE(records.size(), 2u);
    for (const auto& record : records)
        EXPECT_EQ(record->state, frost::lang::ProjectEvaluationState::Failed);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(MsBuildProvider, IndependentWorkerOffersFirstDatabaseWhileSecondIsHeld) {
    if (!kWindows)
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(),
                                                     fixture("../../project/fixtures/sdk_solution/SdkApp.sln"));
    ASSERT_TRUE(model);
    auto shared = std::make_shared<const frost::project::SolutionModel>(std::move(*model));
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = std::string(kEvaluation)});
    launcher.expect({.neverExits = true, .outputHeld = true});
    frost::io::MemFs fs(frost::systemClock());
    std::atomic<bool> trusted{true};
    frost::lang::MsBuildProviderOptions options;
    options.msbuildPath = "C:\\VS\\MSBuild.exe";
    options.configuration = "Debug|x64";
    options.trusted = [&] { return trusted.load(); };
    auto evaluation =
        std::make_shared<frost::lang::IndependentProjectEvaluation>(launcher, fs, shared, "C:\\cache", options);
    std::atomic<bool> ready{false};
    std::optional<frost::lang::CompileCommandsLocation> location;
    std::jthread consumer([&](std::stop_token stop) {
        auto result = evaluation->locate(std::move(stop));
        if (result && *result)
            location = **result;
        ready.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && (!ready.load() || launcher.launches().size() < 2))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool firstReady = ready.load();
    const auto launches = launcher.launches();
    consumer.request_stop();
    consumer.join();
    trusted.store(false);
    EXPECT_FALSE(evaluation->locate());
    evaluation->requestStop();
    evaluation->stopAndJoin();
    EXPECT_TRUE(firstReady);
    ASSERT_TRUE(location);
    EXPECT_EQ(location->source, CompileCommandsSource::Evaluated);
    EXPECT_TRUE(fs.exists(location->file).value_or(false));
    ASSERT_GE(launches.size(), 2u);
    EXPECT_TRUE(launcher.launches()[1].terminated);
}

TEST(MsBuildProvider, RevokedTrustPreventsPublicationAfterEvaluation) {
    if (!kWindows)
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(), fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model);
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = std::string(kEvaluation)});
    frost::io::MemFs fs(frost::systemClock());
    unsigned checks = 0;
    frost::lang::MsBuildProviderOptions options;
    options.msbuildPath = "C:\\VS\\MSBuild.exe";
    options.trusted = [&] { return ++checks <= 2; };
    frost::lang::MsBuildCompileCommandsProvider provider(
        launcher, fs, std::make_shared<const frost::project::SolutionModel>(std::move(*model)), "C:\\cache", options);
    EXPECT_FALSE(provider.locate());
    ASSERT_EQ(launcher.launches().size(), 1u);
    EXPECT_FALSE(fs.exists("C:\\cache").value_or(true));
}

TEST(MsBuildProvider, ExitedChildWithHeldOutputTimesOutAndAbandonsPipes) {
    if (!kWindows)
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(), fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model);
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.outputHeld = true}); // Child exits, but a descendant retains both pipes.
    frost::io::MemFs fs(frost::systemClock());
    frost::lang::MsBuildProviderOptions options;
    options.msbuildPath = "C:\\VS\\MSBuild.exe";
    options.trusted = [] { return true; };
    options.timeout = std::chrono::milliseconds(150);
    frost::lang::MsBuildCompileCommandsProvider provider(
        launcher, fs, std::make_shared<const frost::project::SolutionModel>(std::move(*model)), "C:\\cache", options);
    const auto start = std::chrono::steady_clock::now();
    const auto result = provider.locate();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Timeout);
    ASSERT_EQ(launcher.launches().size(), 1U);
    EXPECT_TRUE(launcher.launches()[0].terminated);
    EXPECT_TRUE(launcher.launches()[0].outputAbandoned);
}

TEST(MsBuildProvider, FailedEvaluationFallsBackToTheStaticModel) {
    if (!kWindows) {
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    }
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(), fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model.has_value());
    const auto shared = std::make_shared<const frost::project::SolutionModel>(std::move(*model));
    frost::io::ScriptedProcessLauncher launcher;
    frost::io::ScriptedProcess failing;
    failing.exitCode = 1;
    failing.stdoutBytes = "App.vcxproj : error MSB4019: The imported project was not found.\r\n";
    launcher.expect(failing);
    frost::io::MemFs fs(frost::systemClock());
    ASSERT_TRUE(fs.createDirectories("C:\\cache").has_value());

    frost::lang::MsBuildProviderOptions options;
    options.msbuildPath = "C:\\VS\\MSBuild.exe";
    options.trusted = [] { return true; };
    options.configuration = "Debug|x64";
    frost::lang::MsBuildCompileCommandsProvider provider(launcher, fs, shared, "C:\\cache\\evaluated", options);
    auto located = provider.locate();
    // The only project failed, so the provider defers to the static synthesizer of the chain.
    ASSERT_FALSE(located.has_value());
    EXPECT_NE(located.error().describe().find("MSB4019"), std::string::npos);
    EXPECT_EQ(provider.lastStats().fellBack, 1u);
    ASSERT_EQ(provider.warnings().size(), 1u);
    EXPECT_NE(provider.warnings()[0].find("static project model"), std::string::npos);

    // The chain then answers with the synthesized database.
    launcher.expect(failing);
    auto chain = frost::lang::solutionCompileCommandsChain(launcher, fs, shared, "C:\\nowhere", "C:\\cache", options);
    ASSERT_TRUE(fs.createDirectories("C:\\nowhere").has_value());
    auto fromChain = chain->locate();
    ASSERT_TRUE(fromChain.has_value()) << fromChain.error().describe();
    ASSERT_TRUE(fromChain->has_value());
    EXPECT_EQ((*fromChain)->source, CompileCommandsSource::Synthesized);
}

TEST(MsBuildProvider, LateFallbackCannotOverwriteAnotherChainDatabase) {
    if (!kWindows)
        GTEST_SKIP() << "MSBuild evaluation runs on Windows only";
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(), fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model);
    auto shared = std::make_shared<const frost::project::SolutionModel>(std::move(*model));
    frost::io::ScriptedProcessLauncher launcher;
    frost::io::ScriptedProcess failing;
    failing.exitCode = 1;
    launcher.expect(failing);
    launcher.expect(failing);
    frost::io::MemFs fs(frost::systemClock());
    frost::lang::MsBuildProviderOptions oldOptions;
    oldOptions.msbuildPath = "C:\\VS\\MSBuild.exe";
    oldOptions.trusted = [] { return true; };
    oldOptions.configuration = "Debug|x64";
    auto newOptions = oldOptions;
    newOptions.configuration = "Release|x64";
    auto oldChain =
        frost::lang::solutionCompileCommandsChain(launcher, fs, shared, "C:\\none", "C:\\cache", oldOptions);
    auto newChain =
        frost::lang::solutionCompileCommandsChain(launcher, fs, shared, "C:\\none", "C:\\cache", newOptions);
    auto newer = newChain->locate();
    ASSERT_TRUE(newer && newer->has_value());
    ASSERT_EQ((*newer)->source, CompileCommandsSource::Synthesized);
    const auto before = fs.readFile((*newer)->file);
    ASSERT_TRUE(before);
    auto older = oldChain->locate();
    ASSERT_TRUE(older && older->has_value());
    EXPECT_NE((*older)->file, (*newer)->file);
    EXPECT_EQ(fs.readFile((*newer)->file).value_or(""), *before);
}

} // namespace
