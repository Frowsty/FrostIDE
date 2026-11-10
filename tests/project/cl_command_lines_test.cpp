// SPDX-License-Identifier: Apache-2.0
//
// SLN-5 without MSBuild: the MSVC command-line splitter, the switches the translation drops, the
// clangd entries built from GetClCommandLines, and the ProjectHost ClCommandLines request and
// answer (frost/project/cl_command_lines.hpp, project_host.hpp). Label "unit"; the real MSBuild and
// clangd run in cl_command_lines_real_test.cpp and tests/lang/cl_command_lines_clangd_test.cpp.

#include "frost/project/cl_command_lines.hpp"
#include "frost/project/project_host.hpp"

#include "json.hpp"
#include "paths.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace frost::project {
namespace {

using Args = std::vector<std::string>;

TEST(ClCommandLines, TheSplitterFollowsTheMsvcRuntimeRules) {
    EXPECT_EQ(splitMsvcCommandLine(""), Args{});
    EXPECT_EQ(splitMsvcCommandLine("  /c\t/W3  "), (Args{"/c", "/W3"}));
    // Quotes group and vanish; a quote may start mid-argument.
    EXPECT_EQ(splitMsvcCommandLine(R"(/I"C:\Program Files\inc" /D X=1)"),
              (Args{R"(/IC:\Program Files\inc)", "/D", "X=1"}));
    // 2n backslashes before a quote give n and the quote toggles; 2n+1 give n and a literal quote.
    EXPECT_EQ(splitMsvcCommandLine(R"(/Fo"x64\Debug\\" /c)"), (Args{R"(/Fox64\Debug\)", "/c"}));
    EXPECT_EQ(splitMsvcCommandLine(R"(/DS=\"v\")"), (Args{R"(/DS="v")"}));
    EXPECT_EQ(splitMsvcCommandLine(R"(a\\\"b)"), (Args{R"(a\"b)"}));
    // Backslashes not before a quote are literal; "" inside quotes is a literal quote.
    EXPECT_EQ(splitMsvcCommandLine(R"(C:\a\b "x""y")"), (Args{R"(C:\a\b)", R"(x"y)"}));
    EXPECT_EQ(splitMsvcCommandLine(R"("" x)"), (Args{"", "x"}));
}

TEST(ClCommandLines, OnlyOutputPchParallelAndErrorReportSwitchesAreDropped) {
    for (const char* dropped :
         {"/Fox64\\Debug\\", "/Fdvc143.pdb", "/Fpx.pch", "/Faout", "/FAcs", "/Feapp.exe", "/Fmmap", "/FRbrowse",
          "/Frbrowse", "/Fipre.i", "/Yupch.h", "/Ycpch.h", "/Y-", "/MP", "/MP8", "/errorReport:queue", "-Fofoo"}) {
        EXPECT_TRUE(isDroppedClSwitch(dropped)) << dropped;
    }
    for (const char* kept :
         {"/FIforced.h", "/FUusing.dll", "/c", "/D", "X=1", "/Iinc", "/MDd", "/EHsc", "/std:c++20", "/permissive-",
          "/Zc:wchar_t", "/external:W1", "/W3", "/WX-", "/sdl", "/Gd", "/TP", "/fp:precise", "Fo"}) {
        EXPECT_FALSE(isDroppedClSwitch(kept)) << kept;
    }
}

TEST(ClCommandLines, PlatformsMapToClangTargets) {
    EXPECT_EQ(clangTargetForPlatform("x64"), "x86_64-pc-windows-msvc");
    EXPECT_EQ(clangTargetForPlatform("WIN32"), "i686-pc-windows-msvc");
    EXPECT_EQ(clangTargetForPlatform("x86"), "i686-pc-windows-msvc");
    EXPECT_EQ(clangTargetForPlatform("ARM64"), "aarch64-pc-windows-msvc");
    EXPECT_EQ(clangTargetForPlatform("Xbox"), "");
}

TEST(ClCommandLines, EachSourceGetsTheCommandLineAsWrittenPlusTargetSystemDirectoriesAndItself) {
    const std::string project = test::MemTree::path("Core/Core.vcxproj");
    const std::string a = test::MemTree::path("Core/src/a.cpp");
    const std::string b = test::MemTree::path("Core/src/b.cpp");
    const std::string inc = test::MemTree::path("vc/include");
    ProjectClCommandLines lines{
        .projectPath = project,
        .configuration = "Debug",
        .platform = "x64",
        .commandLines =
            {{.commandLine = R"(/c /I"inc dir" /D X=1 /EHsc /MDd /Fo"x64\Debug\\" /errorReport:queue)",
              .files = {a, b},
              .workingDirectory = "",
              .configurationOptions = true},
             {.commandLine = "/c /D ONLY_A=1", .files = {a}, .workingDirectory = "", .configurationOptions = false}},
        .includePath = {inc, inc},
        .externalIncludePath = {inc}};
    const auto commands = clangdCommands(lines);
    ASSERT_EQ(commands.size(), 2U) << "a.cpp keeps its first command line";
    const Args expected{
        "clang-cl", "--target=x86_64-pc-windows-msvc", "/c", "/Iinc dir", "/D", "X=1", "/EHsc", "/MDd", "/imsvc" + inc,
        a};
    EXPECT_EQ(commands[0].file, a);
    EXPECT_EQ(commands[0].directory, test::MemTree::path("Core"));
    EXPECT_EQ(commands[0].arguments, expected);
    EXPECT_EQ(commands[1].file, b);
    EXPECT_EQ(commands[1].arguments.back(), b);
    EXPECT_EQ(translatedClArguments(lines.commandLines[0].commandLine),
              (Args{"/c", "/Iinc dir", "/D", "X=1", "/EHsc", "/MDd"}));
}

TEST(ClCommandLines, TheRequestCarriesTheDesignTimeGlobalPropertiesAndTheAnswerParses) {
    const ProjectEvaluationRequest request{.projectPath = test::MemTree::path("Core/Core.vcxproj"),
                                           .configuration = "Release",
                                           .platform = "x64",
                                           .solutionPath = test::MemTree::path("G.sln"),
                                           .itemTypes = {},
                                           .metadata = {},
                                           .properties = {}};
    const auto globals = clCommandLinesGlobalProperties(request);
    EXPECT_EQ(globals.size(), evaluationGlobalProperties(request).size() + 2);
    EXPECT_EQ(globals[globals.size() - 2], (std::pair<std::string, std::string>{"DesignTimeBuild", "true"}));
    EXPECT_EQ(globals.back(), (std::pair<std::string, std::string>{"BuildProjectReferences", "false"}));
    auto line = projectHostClCommandLinesRequest(request, "7");
    ASSERT_TRUE(line.has_value());
    auto json = detail::parseJson(*line);
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ(json->stringAt("Op"), "ClCommandLines");
    EXPECT_EQ(json->stringAt("Id"), "7");
    EXPECT_EQ(json->stringAt("Trusted"), "true");
    EXPECT_EQ(json->stringAt("Property:Configuration"), "Release");
    EXPECT_EQ(json->stringAt("Property:DesignTimeBuild"), "true");
    EXPECT_EQ(json->stringAt("Properties"), "IncludePath;ExternalIncludePath");
    auto relative = request;
    relative.projectPath = "Core.vcxproj";
    EXPECT_EQ(projectHostClCommandLinesRequest(relative, "1").error().code(), ErrorCode::InvalidArgument);

    std::string a = test::MemTree::path("Core/src/a.cpp");
    std::string escaped;
    for (const char c : a) {
        escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
    }
    const std::string answer =
        R"({"Op":"ClCommandLines","Id":"7","Error":null,"Project":")" + escaped +
        R"(","Configuration":"Release","Platform":"x64","CommandLines":[{"CommandLine":"/c /D NDEBUG","Files":")" +
        escaped +
        R"(;__temporary.cpp","WorkingDirectory":"","ConfigurationOptions":"true"}],"Properties":{"IncludePath":")" +
        escaped + R"(;..\\inc;","ExternalIncludePath":""}})";
    auto parsed = parseProjectHostClCommandLines(answer);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    ASSERT_EQ(parsed->commandLines.size(), 1U);
    EXPECT_EQ(parsed->commandLines[0].commandLine, "/c /D NDEBUG");
    EXPECT_EQ(parsed->commandLines[0].files, (Args{a})) << "the target's placeholder source is not listed";
    EXPECT_TRUE(parsed->commandLines[0].configurationOptions);
    // A relative directory is relative to the project's directory (here the "project" is a.cpp).
    EXPECT_EQ(parsed->includePath, (Args{a, test::MemTree::path("Core/inc")}));
    EXPECT_TRUE(parsed->externalIncludePath.empty());
    EXPECT_EQ(parseProjectHostClCommandLines(R"({"Op":"ClCommandLines","Error":"GetClCommandLines failed: x"})")
                  .error()
                  .code(),
              ErrorCode::IoError);
    EXPECT_EQ(parseProjectHostClCommandLines(R"({"Op":"ClCommandLines","Error":null})").error().code(),
              ErrorCode::Corrupt);
    EXPECT_EQ(parseProjectHostClCommandLines(R"({"Op":"Evaluate","Error":null,"CommandLines":[]})").error().code(),
              ErrorCode::Corrupt);
}

TEST(ClCommandLines, TheEvaluateAnswerCarriesItsGlobs) {
    auto parsed = parseProjectHostEvaluation(
        R"({"Op":"Evaluate","Id":"1","Error":null,"Project":"C:\\p\\P.vcxproj","Items":[],"Globs":["src\\*.cpp","include\\**\\*.h"]})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->globs, (Args{"src\\*.cpp", "include\\**\\*.h"}));
}

} // namespace
} // namespace frost::project
