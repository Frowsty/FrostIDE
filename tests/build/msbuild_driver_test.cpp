// SPDX-License-Identifier: Apache-2.0
//
// Tests for the MSBuild driver's command line and environment (msbuildProcessSpec, portable) and
// for startMsBuild's preconditions (trust everywhere, Unsupported off Windows, the PDB endpoint on
// Windows against the scripted launcher).

#include "frost/build/msbuild_driver.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::build::BuildAction;
using frost::build::MsBuildRequest;

constexpr std::string_view kMsBuild = "C:\\VS\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe";

MsBuildRequest solutionRequest() {
    return {.msbuildPath = std::string(kMsBuild),
            .solutionPath = "C:\\src\\My Game\\Game.sln",
            .configuration = "Debug",
            .platform = "x64",
            .trusted = true,
            .uiLanguage = "en-US"};
}

std::optional<std::string> environmentValue(const frost::io::ProcessSpec& spec, std::string_view name,
                                            bool* removed = nullptr) {
    for (const auto& [key, value] : spec.environment) {
        if (key == name) {
            if (removed != nullptr) {
                *removed = !value.has_value();
            }
            return value.value_or("");
        }
    }
    return std::nullopt;
}

TEST(MsBuildCommandLine, WholeSolution) {
    auto spec = frost::build::msbuildProcessSpec(solutionRequest());
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    EXPECT_EQ(spec->executable, kMsBuild);
    EXPECT_EQ(spec->arguments,
              (std::vector<std::string>{"C:\\src\\My Game\\Game.sln", "-t:Build", "-p:Configuration=Debug",
                                        "-p:Platform=x64", "-m", "-nologo", "-nodeReuse:false", "-v:minimal",
                                        "-clp:NoSummary;ForceNoAlign;DisableConsoleColor"}));
    EXPECT_EQ(spec->workingDirectory, "C:\\src\\My Game");
    EXPECT_EQ(spec->stdoutMode, frost::io::Stdio::Pipe);
    EXPECT_EQ(spec->stderrMode, frost::io::Stdio::Pipe);
    EXPECT_EQ(spec->stdinMode, frost::io::Stdio::Null);
    EXPECT_TRUE(spec->killOnClose);

    EXPECT_EQ(environmentValue(*spec, "DOTNET_CLI_UI_LANGUAGE"), "en-US");
    EXPECT_EQ(environmentValue(*spec, "MSBUILDTERMINALLOGGER"), "off");
    EXPECT_EQ(environmentValue(*spec, "MSBUILDDISABLENODEREUSE"), "1");
    bool removed = false;
    EXPECT_TRUE(environmentValue(*spec, "NoDefaultCurrentDirectoryInExePath", &removed).has_value());
    EXPECT_TRUE(removed);
    // The PDB server endpoint is per build, so startMsBuild adds it, not the pure spec.
    EXPECT_FALSE(environmentValue(*spec, "_MSPDBSRV_ENDPOINT_").has_value());
}

TEST(MsBuildCommandLine, ActionsAndOneProjectOfTheSolution) {
    MsBuildRequest request = solutionRequest();
    request.action = BuildAction::Rebuild;
    EXPECT_EQ(frost::build::msbuildProcessSpec(request)->arguments[1], "-t:Rebuild");
    request.action = BuildAction::Clean;
    EXPECT_EQ(frost::build::msbuildProcessSpec(request)->arguments[1], "-t:Clean");

    request.projectName = "Engine\\My.Core (old)";
    request.action = BuildAction::Build;
    EXPECT_EQ(frost::build::msbuildProcessSpec(request)->arguments[1], "-t:Engine\\My_Core _old_");
    request.action = BuildAction::Rebuild;
    EXPECT_EQ(frost::build::msbuildProcessSpec(request)->arguments[1], "-t:Engine\\My_Core _old_:Rebuild");
    request.action = BuildAction::Clean;
    EXPECT_EQ(frost::build::msbuildProcessSpec(request)->arguments[1], "-t:Engine\\My_Core _old_:Clean");

    EXPECT_EQ(frost::build::toString(BuildAction::Build), "Build");
    EXPECT_EQ(frost::build::solutionTargetName("a%b$c@d;e.f(g)h'i"), "a_b_c_d_e_f_g_h_i");
}

TEST(MsBuildCommandLine, ProjectFileWithSolutionProperties) {
    MsBuildRequest request = solutionRequest();
    request.solutionPath = "C:\\src\\Game.slnx";
    request.projectPath = "C:/src/Engine/Engine.vcxproj";
    request.action = BuildAction::Rebuild;
    request.maxCpuCount = 4;
    request.verbosity = frost::build::MsBuildVerbosity::Normal;
    auto spec = frost::build::msbuildProcessSpec(request);
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    EXPECT_EQ(spec->arguments,
              (std::vector<std::string>{
                  "C:\\src\\Engine\\Engine.vcxproj", "-t:Rebuild", "-p:Configuration=Debug", "-p:Platform=x64",
                  "-p:SolutionDir=C:\\src\\", "-p:SolutionPath=C:\\src\\Game.slnx", "-p:SolutionName=Game",
                  "-p:SolutionFileName=Game.slnx", "-p:SolutionExt=.slnx", "-m:4", "-nologo", "-nodeReuse:false",
                  "-v:normal", "-clp:NoSummary;ForceNoAlign;DisableConsoleColor"}));
    EXPECT_EQ(spec->workingDirectory, "C:\\src\\Engine");

    // A project file on its own gets no Solution* properties.
    request.solutionPath.clear();
    auto alone = frost::build::msbuildProcessSpec(request);
    ASSERT_TRUE(alone.has_value());
    EXPECT_TRUE(std::ranges::none_of(alone->arguments, [](const std::string& a) { return a.starts_with("-p:Sol"); }));
}

TEST(MsBuildCommandLine, PropertiesAreEscaped) {
    MsBuildRequest request = solutionRequest();
    request.platform = "Any CPU";
    request.properties = {{"OutDir", "C:\\out;50%,x\\"}, {"Flag_2-b", "1"}};
    auto spec = frost::build::msbuildProcessSpec(request);
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->arguments[3], "-p:Platform=Any CPU");
    EXPECT_EQ(spec->arguments[4], "-p:OutDir=C:\\out%3B50%25%2Cx\\");
    EXPECT_EQ(spec->arguments[5], "-p:Flag_2-b=1");

    for (const std::string_view bad : {"", "1st", "a b", "a=b", "a;b"}) {
        request.properties = {{std::string(bad), "x"}};
        auto refused = frost::build::msbuildProcessSpec(request);
        ASSERT_FALSE(refused.has_value()) << bad;
        EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument) << bad;
    }
}

TEST(MsBuildCommandLine, RefusesIncompleteRequests) {
    std::vector<MsBuildRequest> bad(7, solutionRequest());
    bad[0].solutionPath.clear();
    bad[1].configuration.clear();
    bad[2].platform.clear();
    bad[3].projectName = "Lib";
    bad[3].projectPath = "C:\\src\\Lib\\Lib.vcxproj";
    bad[4].solutionPath = "src\\Game.sln";
    bad[5].msbuildPath = "MSBuild.exe";
    bad[6].projectPath = "Lib.vcxproj";
    for (std::size_t i = 0; i < bad.size(); ++i) {
        auto spec = frost::build::msbuildProcessSpec(bad[i]);
        ASSERT_FALSE(spec.has_value()) << i;
        EXPECT_EQ(spec.error().code(), ErrorCode::InvalidArgument) << i;
    }
}

TEST(MsBuildCommandLine, DefaultUiLanguageIsSet) {
    MsBuildRequest request = solutionRequest();
    request.uiLanguage.clear();
    auto spec = frost::build::msbuildProcessSpec(request);
    ASSERT_TRUE(spec.has_value());
    const std::optional<std::string> language = environmentValue(*spec, "DOTNET_CLI_UI_LANGUAGE");
    ASSERT_TRUE(language.has_value());
    EXPECT_FALSE(language->empty());
}

TEST(StartMsBuild, RefusesAnUntrustedFolderBeforeAnythingElse) {
    frost::io::ScriptedProcessLauncher launcher;
    MsBuildRequest request = solutionRequest();
    request.trusted = false;
    request.configuration.clear(); // Trust is checked first, on every platform.
    auto session = frost::build::startMsBuild(launcher, request, {});
    ASSERT_FALSE(session.has_value());
    EXPECT_EQ(session.error().code(), ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
}

#ifdef _WIN32

TEST(StartMsBuild, RunsTheSpecWithAPdbServerEndpointOfItsOwn) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = std::string(kMsBuild), .stdoutBytes = "  Lib.vcxproj -> C:\\x\\Lib.lib\r\n"});
    launcher.expect({.expectExecutable = std::string(kMsBuild)});
    for (int i = 0; i < 2; ++i) {
        auto session = frost::build::startMsBuild(launcher, solutionRequest(), {});
        ASSERT_TRUE(session.has_value()) << session.error().describe();
        EXPECT_TRUE((*session)->wait().success);
    }
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    const auto first = environmentValue(launches[0].spec, "_MSPDBSRV_ENDPOINT_");
    const auto second = environmentValue(launches[1].spec, "_MSPDBSRV_ENDPOINT_");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(first->starts_with("FrostIDE-"));
    EXPECT_NE(*first, *second);
}

#else

TEST(StartMsBuild, IsUnsupportedOffWindows) {
    frost::io::ScriptedProcessLauncher launcher;
    auto session = frost::build::startMsBuild(launcher, solutionRequest(), {});
    ASSERT_FALSE(session.has_value());
    EXPECT_EQ(session.error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(launcher.launches().empty());
}

#endif

} // namespace
