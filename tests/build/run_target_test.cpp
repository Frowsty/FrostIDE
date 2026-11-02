// SPDX-License-Identifier: Apache-2.0
//
// Tests for what Run starts (frost/build/run_target.hpp) on every platform: argument splitting,
// $(Macro) and %VAR% expansion, the debugger environment, MSBuild's -getProperty JSON and command
// line, the static fallback over a MemFs project with a .vcxproj.user, the resolved command
// (defaults, relative working directories), the process spec of each console mode, the PE
// subsystem and the UTF-8 chunk decoder.

#include "frost/build/run_session.hpp"
#include "frost/build/run_target.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace frost::build {
namespace {

using test::MemTree;
using test::peImage;

// A fixed environment for the expansion tests.
project::EnvironmentLookup environment(std::map<std::string, std::string> values) {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        const auto it = values.find(std::string(name));
        return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
}

TEST(RunTarget, SplitsArgumentsLikeTheCrt) {
    using Args = std::vector<std::string>;
    EXPECT_EQ(splitWindowsArguments(""), Args{});
    EXPECT_EQ(splitWindowsArguments("  a  b\tc "), (Args{"a", "b", "c"}));
    EXPECT_EQ(splitWindowsArguments(R"(--print "two words" x)"), (Args{"--print", "two words", "x"}));
    EXPECT_EQ(splitWindowsArguments(R"("" a)"), (Args{"", "a"}));
    EXPECT_EQ(splitWindowsArguments(R"(a\\b c\\\"d "e\\")"), (Args{R"(a\\b)", R"(c\"d)", R"(e\)"}));
    EXPECT_EQ(splitWindowsArguments(R"("say ""hi""")"), (Args{R"(say "hi")"}));
    EXPECT_EQ(splitWindowsArguments(R"(pre"quoted part"post)"), (Args{"prequoted partpost"}));
    // The inverse of frost::io's quoting: every argv survives a round trip.
    const Args samples = {"plain", "two words", "", R"(trail\)", R"(q"uote)", R"(back\\"slash)", "tab\there"};
    std::string line;
    for (const std::string& argument : samples) {
        if (!line.empty()) {
            line += ' ';
        }
        io::appendWindowsArgument(line, argument);
    }
    EXPECT_EQ(splitWindowsArguments(line), samples);
}

TEST(RunTarget, ExpandsMacrosAndEnvironmentVariables) {
    const RunMacros macros = {{"projectdir", "C:\\s\\App\\"}, {"configuration", "Debug"}};
    const auto env = environment({{"MY_SDK", "D:\\sdk"}, {"HOME", "h"}});
    EXPECT_EQ(expandRunMacros("$(ProjectDir)data", macros, env), "C:\\s\\App\\data");
    EXPECT_EQ(expandRunMacros("$(projectdir)|$( Configuration )", macros, env), "C:\\s\\App\\|Debug");
    EXPECT_EQ(expandRunMacros("$(MY_SDK)\\bin", macros, env), "D:\\sdk\\bin") << "the environment comes second";
    EXPECT_EQ(expandRunMacros("[$(Unknown)]", macros, env), "[]") << "MSBuild expands unknown names to nothing";
    EXPECT_EQ(expandRunMacros("cost $5 and $(open", macros, env), "cost $5 and $(open");
    EXPECT_EQ(expandPercentVariables("%HOME%;%NOPE%;100%", env), "h;%NOPE%;100%");
}

TEST(RunTarget, ParsesTheDebuggerEnvironment) {
    std::vector<std::string> warnings;
    const auto variables = parseDebuggerEnvironment("A=1\r\n\nPATH=%BASE%;x\nbroken line\n=nameless\nB=x=y",
                                                    environment({{"BASE", "C:\\bin"}}), &warnings);
    using Pairs = std::vector<std::pair<std::string, std::string>>;
    EXPECT_EQ(variables, (Pairs{{"A", "1"}, {"PATH", "C:\\bin;x"}, {"B", "x=y"}}));
    EXPECT_EQ(warnings.size(), 2U);
}

TEST(RunTarget, ParsesMsBuildsGetPropertyJson) {
    // What MSBuild 17.14 printed for the tiny fixture (2026-11-02), shortened.
    const std::string json = R"({
  "Properties": {
    "TargetPath": "C:\\w\\x64\\Debug\\App.exe",
    "TargetDir": "C:\\w\\x64\\Debug\\",
    "ProjectDir": "C:\\w\\App\\",
    "SolutionDir": "C:\\w\\",
    "ConfigurationType": "Application",
    "LocalDebuggerCommand": "C:\\w\\x64\\Debug\\App.exe",
    "LocalDebuggerCommandArguments": "--print \u0022two words\u0022",
    "LocalDebuggerWorkingDirectory": "C:\\w\\App\\work",
    "LocalDebuggerEnvironment": "A=1\nB=2",
    "LocalDebuggerMergeEnvironment": "true"
  }
})";
    auto parsed = parseRunProperties(json);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    EXPECT_EQ(parsed->targetPath, "C:\\w\\x64\\Debug\\App.exe");
    EXPECT_EQ(parsed->projectDir, "C:\\w\\App\\");
    EXPECT_EQ(parsed->configurationType, "Application");
    EXPECT_EQ(parsed->commandArguments, "--print \"two words\"");
    EXPECT_EQ(parsed->environment, "A=1\nB=2");
    EXPECT_TRUE(parsed->mergeEnvironment);
    EXPECT_TRUE(parsed->fromMsBuild);

    auto noMerge = parseRunProperties(R"({"Properties": {"LocalDebuggerMergeEnvironment": "False"}})");
    ASSERT_TRUE(noMerge.has_value());
    EXPECT_FALSE(noMerge->mergeEnvironment);
    EXPECT_FALSE(parseRunProperties("C:\\w\\x64\\Debug\\App.exe").has_value()) << "one property prints no JSON";
    EXPECT_FALSE(parseRunProperties(R"({"Items": {}})").has_value());
}

TEST(RunTarget, TheQueryAsksMsBuildForEveryPropertyWithVisualStudiosSolutionProperties) {
    auto spec = runPropertyQuerySpec({.msbuildPath = "C:\\VS\\MSBuild.exe",
                                      .projectPath = "C:\\w\\App\\App.vcxproj",
                                      .solutionPath = "C:\\w\\Tiny.sln",
                                      .configuration = "Debug",
                                      .platform = "x64"});
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    EXPECT_EQ(spec->executable, "C:\\VS\\MSBuild.exe");
    EXPECT_EQ(spec->workingDirectory, "C:\\w\\App");
    const auto has = [&](std::string_view argument) {
        return std::ranges::find(spec->arguments, argument) != spec->arguments.end();
    };
    EXPECT_EQ(spec->arguments.front(), "C:\\w\\App\\App.vcxproj");
    for (const std::string_view name : runPropertyNames()) {
        EXPECT_TRUE(has(std::string("-getProperty:") + std::string(name))) << name;
    }
    EXPECT_TRUE(has("-p:Configuration=Debug"));
    EXPECT_TRUE(has("-p:Platform=x64"));
    EXPECT_TRUE(has("-p:SolutionDir=C:\\w\\"));
    EXPECT_TRUE(has("-p:SolutionPath=C:\\w\\Tiny.sln"));
    EXPECT_TRUE(has("-p:SolutionName=Tiny"));
    EXPECT_FALSE(std::ranges::any_of(spec->arguments, [](const std::string& a) { return a.starts_with("-t:"); }))
        << "evaluation only";
    EXPECT_FALSE(runPropertyQuerySpec({.msbuildPath = "MSBuild.exe",
                                       .projectPath = "C:\\w\\App.vcxproj",
                                       .solutionPath = {},
                                       .configuration = "Debug",
                                       .platform = "x64"})
                     .has_value());
}

// App.vcxproj with an OutDir only for Release, and App.vcxproj.user with debugger settings.
void writeApp(MemTree& tree) {
    tree.write("Sln/App/App.vcxproj", R"(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType></PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'"><OutDir>$(SolutionDir)out\</OutDir></PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'"><TargetName>App32</TargetName></PropertyGroup>
</Project>
)");
    tree.write("Sln/App/App.vcxproj.user", R"xml(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <LocalDebuggerCommandArguments>--name "$(ProjectName)"</LocalDebuggerCommandArguments>
    <LocalDebuggerWorkingDirectory>$(ProjectDir)work</LocalDebuggerWorkingDirectory>
    <LocalDebuggerEnvironment>MODE=$(Configuration)</LocalDebuggerEnvironment>
    <LocalDebuggerMergeEnvironment>false</LocalDebuggerMergeEnvironment>
  </PropertyGroup>
</Project>
)xml");
}

TEST(RunTarget, TheStaticFallbackAppliesMsBuildsDefaultsAndTheUserFile) {
    MemTree tree;
    writeApp(tree);
    RunPropertyRequest request{.msbuildPath = {},
                               .projectPath = MemTree::path("Sln/App/App.vcxproj"),
                               .solutionPath = MemTree::path("Sln/Sln.sln"),
                               .configuration = "Debug",
                               .platform = "x64"};
    auto debug = staticRunProperties(tree.fs, request, environment({}));
    ASSERT_TRUE(debug.has_value()) << debug.error().describe();
    EXPECT_FALSE(debug->fromMsBuild);
    EXPECT_FALSE(debug->warnings.empty());
    EXPECT_EQ(debug->targetPath, MemTree::path("Sln/x64/Debug/App.exe"));
    EXPECT_EQ(debug->configurationType, "Application");
    EXPECT_EQ(debug->commandArguments, "--name \"$(ProjectName)\"") << "the fallback keeps them for resolveRunCommand";
    EXPECT_FALSE(debug->mergeEnvironment);

    request.configuration = "Release";
    auto release = staticRunProperties(tree.fs, request, environment({}));
    ASSERT_TRUE(release.has_value());
    EXPECT_EQ(release->targetPath, MemTree::path("Sln/out/App.exe"));
    EXPECT_TRUE(release->commandArguments.empty()) << "the user file sets Debug|x64 only";

    request.configuration = "Debug";
    request.platform = "Win32";
    auto win32 = staticRunProperties(tree.fs, request, environment({}));
    ASSERT_TRUE(win32.has_value());
    EXPECT_EQ(win32->targetPath, MemTree::path("Sln/Debug/App32.exe")) << "Win32 has no platform folder";

    // The command it resolves to: macros expanded, the working directory and environment from the user file.
    request.platform = "x64";
    auto command = resolveRunCommand(*debug, runMacros(*debug, request), environment({}));
    ASSERT_TRUE(command.has_value()) << command.error().describe();
    EXPECT_EQ(command->executable, MemTree::path("Sln/x64/Debug/App.exe"));
    EXPECT_EQ(command->arguments, "--name \"App\"");
    EXPECT_EQ(command->workingDirectory, MemTree::path("Sln/App/work"));
    using Pairs = std::vector<std::pair<std::string, std::string>>;
    EXPECT_EQ(command->environment, (Pairs{{"MODE", "Debug"}}));
    EXPECT_FALSE(command->mergeEnvironment);
}

TEST(RunTarget, ResolvesTheCommandWithVisualStudiosDefaults) {
    RunProperties properties;
    properties.targetPath = MemTree::path("s/x64/Debug/App.exe");
    properties.projectDir = MemTree::path("s/App") + (test::kWindows ? "\\" : "/");
    auto command = resolveRunCommand(properties, {}, environment({}));
    ASSERT_TRUE(command.has_value()) << command.error().describe();
    EXPECT_EQ(command->executable, properties.targetPath) << "no LocalDebuggerCommand: TargetPath";
    EXPECT_EQ(command->workingDirectory, MemTree::path("s/App")) << "no working directory: ProjectDir";
    EXPECT_TRUE(command->arguments.empty());

    properties.workingDirectory = test::native("../data");
    command = resolveRunCommand(properties, {}, environment({}));
    ASSERT_TRUE(command.has_value());
    if constexpr (test::kWindows) {
        EXPECT_EQ(command->workingDirectory, MemTree::path("s/data")) << "relative to ProjectDir";
    } else {
        EXPECT_EQ(command->workingDirectory, MemTree::path("s/App/../data")) << "POSIX keeps '..'";
    }

    properties.command = "relative.exe";
    EXPECT_FALSE(resolveRunCommand(properties, {}, environment({})).has_value());
    properties.command.clear();
    properties.targetPath.clear();
    EXPECT_FALSE(resolveRunCommand(properties, {}, environment({})).has_value());
}

RunCommand sampleCommand() {
    RunCommand command;
    command.executable = MemTree::path("s/x64/Debug/App.exe");
    command.arguments = "--print \"two words\"";
    command.workingDirectory = MemTree::path("s/App");
    command.environment = {{"FROST_TINY_VALUE", "hello"}};
    return command;
}

TEST(RunTarget, TheRunConsoleSpecPipesEveryStreamAndMergesTheEnvironment) {
    auto spec = runProcessSpec(sampleCommand(), ConsoleMode::RunConsole);
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    EXPECT_EQ(spec->executable, sampleCommand().executable);
    EXPECT_EQ(spec->arguments, (std::vector<std::string>{"--print", "two words"}));
    EXPECT_FALSE(spec->rawArguments.has_value());
    EXPECT_EQ(spec->workingDirectory, MemTree::path("s/App"));
    EXPECT_EQ(spec->stdinMode, io::Stdio::Pipe);
    EXPECT_EQ(spec->stdoutMode, io::Stdio::Pipe);
    EXPECT_EQ(spec->stderrMode, io::Stdio::Pipe);
    EXPECT_TRUE(spec->killOnClose);
    EXPECT_FALSE(spec->newConsole);
    EXPECT_FALSE(spec->cleanEnvironment);
    using Env = std::vector<std::pair<std::string, std::optional<std::string>>>;
    EXPECT_EQ(spec->environment,
              (Env{{"NoDefaultCurrentDirectoryInExePath", std::nullopt}, {"FROST_TINY_VALUE", std::string("hello")}}));

    auto gui = runProcessSpec(sampleCommand(), ConsoleMode::NoConsole);
    ASSERT_TRUE(gui.has_value());
    EXPECT_EQ(gui->stdinMode, io::Stdio::Null);

    RunCommand alone = sampleCommand();
    alone.mergeEnvironment = false;
    auto clean = runProcessSpec(alone, ConsoleMode::RunConsole);
    ASSERT_TRUE(clean.has_value());
    EXPECT_TRUE(clean->cleanEnvironment);
    EXPECT_EQ(clean->environment, (Env{{"FROST_TINY_VALUE", std::string("hello")}}));
}

TEST(RunTarget, TheExternalConsoleGoesThroughTheCmdWrapper) {
    const std::string cmd = "C:\\Windows\\System32\\cmd.exe";
    auto spec = runProcessSpec(sampleCommand(), ConsoleMode::ExternalConsole, cmd);
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    EXPECT_EQ(spec->executable, cmd);
    EXPECT_TRUE(spec->arguments.empty());
    ASSERT_TRUE(spec->rawArguments.has_value());
    EXPECT_EQ(*spec->rawArguments,
              "/d /s /c \"\"" + sampleCommand().executable +
                  "\" --print \"two words\" & call set FROST_RUN_EXIT=%^errorlevel%& echo.& call echo Process "
                  "finished with exit code %^FROST_RUN_EXIT%.& pause & call exit %^FROST_RUN_EXIT%\"");
    EXPECT_TRUE(spec->newConsole);
    EXPECT_EQ(spec->stdinMode, io::Stdio::Inherit);
    EXPECT_EQ(spec->stdoutMode, io::Stdio::Inherit);
    EXPECT_EQ(spec->stderrMode, io::Stdio::Inherit);
    EXPECT_TRUE(spec->killOnClose);
    EXPECT_EQ(spec->workingDirectory, MemTree::path("s/App"));
    EXPECT_FALSE(runProcessSpec(sampleCommand(), ConsoleMode::ExternalConsole).has_value()) << "needs cmd.exe";
}

TEST(RunTarget, ReadsThePeSubsystem) {
    EXPECT_EQ(peSubsystem(peImage(3)), Subsystem::Console);
    EXPECT_EQ(peSubsystem(peImage(2)), Subsystem::Gui);
    EXPECT_EQ(peSubsystem(peImage(10)), Subsystem::Unknown);
    EXPECT_FALSE(peSubsystem("not an image").has_value());
    EXPECT_FALSE(peSubsystem(peImage(3).substr(0, 0x50)).has_value()) << "truncated";
    EXPECT_EQ(toString(Subsystem::Gui), "GUI");
}

TEST(RunTarget, ChunksNeverSplitACharacter) {
    Utf8ChunkDecoder decoder;
    const std::string euro = "\xE2\x82\xAC"; // U+20AC, three bytes.
    EXPECT_EQ(decoder.feed("a" + euro.substr(0, 1)), "a");
    EXPECT_EQ(decoder.feed(euro.substr(1, 1)), "");
    EXPECT_EQ(decoder.feed(euro.substr(2) + "b"), euro + "b");
    EXPECT_EQ(decoder.feed("\xFF"
                           "c"),
              "\xEF\xBF\xBD"
              "c")
        << "an invalid byte becomes U+FFFD";
    EXPECT_EQ(decoder.feed("\xC3"), "");
    EXPECT_EQ(decoder.finish(), "\xEF\xBF\xBD") << "an unfinished character at the end";
}

} // namespace
} // namespace frost::build
