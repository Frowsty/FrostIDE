// SPDX-License-Identifier: Apache-2.0
//
// CMK-4 unit tests (frost/project/cmake_compile_database.hpp) on hand-built CMake models: fragment
// splitting by the Windows C runtime's and POSIX sh rules, the compiler front end, the command
// layout per front end (the spelling CMake 4.4's Ninja generator writes, checked against real
// configures by lang_cmake_tests and the corpus tier), the multi-config CMAKE_INTDIR define, the
// cache's compiler as a fallback, skipped languages, header commands in a target's context and
// the targets that list a file.

#include "frost/io/path.hpp"
#include "frost/project/cmake_compile_database.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace frost::project {
namespace {

using Args = std::vector<std::string>;

constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;

// A native absolute path below a fixed root ("C:\src\..." or "/src/...").
std::string at(std::string_view relative) {
    std::string path = std::string(kWindows ? "C:/src/" : "/src/") + std::string(relative);
    return io::normalizePath(path).value_or(path);
}

CMakeProject projectWith(CMakeToolchain toolchain) {
    CMakeProject project;
    project.sourceDirectory = at("");
    project.buildDirectory = at("build");
    project.toolchains.push_back(std::move(toolchain));
    CMakeTarget alpha;
    alpha.name = "alpha";
    CMakeCompileGroup group;
    group.language = "CXX";
    group.defines = {"A", "B=1"};
    group.includeDirectories = {{.path = "/src/inc", .system = false}, {.path = "/dep/include", .system = true}};
    group.flags = {"-O0 -g", "-std=c++20"};
    alpha.compileGroups.push_back(group);
    alpha.sources.push_back(CMakeSource{.path = at("a.cpp"), .compileGroup = 0});
    alpha.sources.push_back(CMakeSource{.path = at("shared.h")});
    project.targets.push_back(alpha);
    CMakeTarget beta;
    beta.name = "beta";
    group.defines = {"BETA"};
    group.includeDirectories.clear();
    beta.compileGroups.push_back(group);
    CMakeCompileGroup rc;
    rc.language = "RC";
    beta.compileGroups.push_back(rc);
    beta.sources.push_back(CMakeSource{.path = at("version.rc"), .compileGroup = 1});
    beta.sources.push_back(CMakeSource{.path = at("generated.cpp"), .generated = true, .compileGroup = 0});
    beta.sources.push_back(CMakeSource{.path = at("b.cpp"), .compileGroup = 0});
    beta.sources.push_back(CMakeSource{.path = at("shared.h")});
    project.targets.push_back(beta);
    return project;
}

constexpr CMakeCommandOptions kPosix{.shell = CMakeShell::Posix, .separator = '/'};
constexpr CMakeCommandOptions kWindowsShell{.shell = CMakeShell::Windows, .separator = '\\'};

TEST(CMakeCompileDatabase, SplitsFragmentsByTheRulesOfTheirShell) {
    EXPECT_EQ(splitCompileCommandFragment(R"(/DWIN32  /D_WINDOWS "-IC:\a b" -DX=\"q\")", CMakeShell::Windows),
              (Args{"/DWIN32", "/D_WINDOWS", R"(-IC:\a b)", R"(-DX="q")"}));
    // 2n backslashes before a quote: n and a toggle; 2n+1: n and a literal quote; "" in quotes.
    EXPECT_EQ(splitCompileCommandFragment(R"(a\\"b c"\d "x""y" e\\\"f)", CMakeShell::Windows),
              (Args{R"(a\b c\d)", R"(x"y)", R"(e\"f)"}));
    EXPECT_EQ(splitCompileCommandFragment(R"(-DX='a b' "-I/p q" \$HOME -Dy=\"z\" "a\"b")", CMakeShell::Posix),
              (Args{"-DX=a b", "-I/p q", "$HOME", R"(-Dy="z")", R"(a"b)"}));
    EXPECT_TRUE(splitCompileCommandFragment("   ", CMakeShell::Posix).empty());
    EXPECT_EQ(splitCompileCommandFragment(R"("")", CMakeShell::Windows), (Args{""})) << "an empty quoted argument";
}

TEST(CMakeCompileDatabase, NamesTheFrontEndFromTheCompiler) {
    EXPECT_EQ(cmakeCompilerFrontend({.compilerPath = "C:/VC/bin/cl.exe", .compilerId = "MSVC"}),
              CMakeCompilerFrontend::Msvc);
    EXPECT_EQ(cmakeCompilerFrontend({.compilerPath = "E:/llvm/bin/clang-cl.exe", .compilerId = "Clang"}),
              CMakeCompilerFrontend::ClangCl);
    EXPECT_EQ(cmakeCompilerFrontend({.compilerPath = "E:/LLVM/BIN/CLANG-CL.EXE", .compilerId = "Clang"}),
              CMakeCompilerFrontend::ClangCl);
    EXPECT_EQ(cmakeCompilerFrontend({.compilerPath = "/usr/bin/clang++", .compilerId = "Clang"}),
              CMakeCompilerFrontend::Gnu);
    EXPECT_EQ(cmakeCompilerFrontend({.compilerPath = "/usr/bin/g++", .compilerId = "GNU"}), CMakeCompilerFrontend::Gnu);
}

TEST(CMakeCompileDatabase, GnuCommandsListDefinesIncludesAndFragmentsThenTheSource) {
    const CMakeProject project = projectWith({.language = "CXX", .compilerPath = "/usr/bin/c++", .compilerId = "GNU"});
    const auto commands = synthesizeCMakeCompileCommands(project, kPosix);
    ASSERT_EQ(commands.size(), 3U) << "a.cpp, generated.cpp and b.cpp; the RC source and headers are skipped";
    EXPECT_EQ(commands[0].file, at("a.cpp"));
    EXPECT_EQ(commands[0].target, 0U);
    EXPECT_EQ(commands[0].arguments, (Args{"/usr/bin/c++", "-DA", "-DB=1", "-I/src/inc", "-isystem", "/dep/include",
                                           "-O0", "-g", "-std=c++20", "-c", at("a.cpp")}));
    EXPECT_EQ(commands[2].arguments, (Args{"/usr/bin/c++", "-DBETA", "-O0", "-g", "-std=c++20", "-c", at("b.cpp")}));
    std::string directory = at("build");
    std::ranges::replace(directory, '\\', '/');
    EXPECT_EQ(commands[0].directory, directory) << "CMake's spelling of the build directory";
}

TEST(CMakeCompileDatabase, MsvcCommandsUseExternalIncludesFrom19_29) {
    const CMakeProject project = projectWith(
        {.language = "CXX", .compilerPath = "C:/VC/cl.exe", .compilerId = "MSVC", .compilerVersion = "19.44.35229.0"});
    const auto commands = synthesizeCMakeCompileCommands(project, kWindowsShell);
    ASSERT_EQ(commands.size(), 3U);
    EXPECT_EQ(commands[0].arguments,
              (Args{"C:/VC/cl.exe", "/nologo", "/TP", "-DA", "-DB=1", R"(-I\src\inc)", R"(-external:I\dep\include)",
                    "-external:W0", "-O0", "-g", "-std=c++20", "-c", at("a.cpp")}));
    const CMakeProject old = projectWith(
        {.language = "CXX", .compilerPath = "C:/VC/cl.exe", .compilerId = "MSVC", .compilerVersion = "19.28.29915"});
    EXPECT_EQ(synthesizeCMakeCompileCommands(old, kWindowsShell)[0].arguments,
              (Args{"C:/VC/cl.exe", "/nologo", "/TP", "-DA", "-DB=1", R"(-I\src\inc)", R"(-I\dep\include)", "-O0", "-g",
                    "-std=c++20", "-c", at("a.cpp")}))
        << "before 19.29.30036.3 a system include is an ordinary -I";
}

TEST(CMakeCompileDatabase, ClangClCommandsUseImsvcAndEndTheOptions) {
    const CMakeProject project =
        projectWith({.language = "CXX", .compilerPath = "E:/llvm/bin/clang-cl.exe", .compilerId = "Clang"});
    EXPECT_EQ(synthesizeCMakeCompileCommands(project, kWindowsShell)[0].arguments,
              (Args{"E:/llvm/bin/clang-cl.exe", "/nologo", "-TP", "-DA", "-DB=1", R"(-I\src\inc)",
                    R"(-imsvc\dep\include)", "-O0", "-g", "-std=c++20", "-c", "--", at("a.cpp")}));
}

TEST(CMakeCompileDatabase, MultiConfigRepliesDefineTheConfigurationAndTheCacheNamesAMissingCompiler) {
    CMakeProject project = projectWith({.language = "C", .compilerPath = "/usr/bin/cc", .compilerId = "GNU"});
    project.toolchains.clear(); // No toolchains object: the cache's CMAKE_CXX_COMPILER is used.
    project.cache.push_back({.name = "CMAKE_CXX_COMPILER", .type = "FILEPATH", .value = "/opt/bin/g++"});
    project.multiConfig = true;
    project.configuration = "Debug";
    const auto commands = synthesizeCMakeCompileCommands(project, kPosix);
    ASSERT_FALSE(commands.empty());
    EXPECT_EQ(commands[2].arguments, (Args{"/opt/bin/g++", "-DBETA", "-DCMAKE_INTDIR=\"Debug\"", "-O0", "-g",
                                           "-std=c++20", "-c", at("b.cpp")}));
    project.cache.clear();
    EXPECT_TRUE(synthesizeCMakeCompileCommands(project, kPosix).empty()) << "no compiler, no invented command";
}

TEST(CMakeCompileDatabase, AHeaderTakesTheContextOfTheTargetChosen) {
    const CMakeProject project = projectWith({.language = "CXX", .compilerPath = "/usr/bin/c++", .compilerId = "GNU"});
    EXPECT_EQ(cmakeTargetsListing(project, at("shared.h")), (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(cmakeTargetsListing(project, at("a.cpp")), (std::vector<std::size_t>{0}));
    EXPECT_TRUE(cmakeTargetsListing(project, at("unknown.h")).empty());
    if constexpr (kWindows)
        EXPECT_EQ(cmakeTargetsListing(project, "c:\\SRC\\Shared.H").size(), 2U) << "case is ignored on Windows";

    const auto inBeta = cmakeCommandInTarget(project, 1, at("shared.h"), kPosix);
    ASSERT_TRUE(inBeta.has_value());
    EXPECT_EQ(inBeta->target, 1U);
    EXPECT_EQ(inBeta->arguments,
              (Args{"/usr/bin/c++", "-DBETA", "-O0", "-g", "-std=c++20", "-x", "c++-header", "-c", at("shared.h")}))
        << "the group of beta's first non-generated C++ source, as a C++ header";
    const auto source = cmakeCommandInTarget(project, 0, at("a.cpp"), kPosix);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->arguments, synthesizeCMakeCompileCommands(project, kPosix)[0].arguments)
        << "a compiled source keeps its own command";
    EXPECT_FALSE(cmakeCommandInTarget(project, 7, at("shared.h"), kPosix).has_value());

    const CMakeProject msvc = projectWith(
        {.language = "CXX", .compilerPath = "C:/VC/cl.exe", .compilerId = "MSVC", .compilerVersion = "19.44"});
    const auto header = cmakeCommandInTarget(msvc, 0, at("shared.h"), kWindowsShell);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->arguments[2], "/TP") << "cl parses the header as C++";
    EXPECT_EQ(header->arguments.back(), at("shared.h"));
}

} // namespace
} // namespace frost::project
