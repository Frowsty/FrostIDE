// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for compile commands (compile_commands.hpp): finding an existing
// compile_commands.json (root, build/, out/build/*/, cmake-build-*/), the JSON written and its
// write-only-on-change rule, the vcxproj ClCompile metadata fold, synthesis for the tiny fixture
// solution in both configurations, and the provider chain with the generator hook.

#include "frost/core/clock.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/vcxproj.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::io::MemFs;
using frost::lang::CompileCommand;
using frost::lang::CompileCommandsSource;
using frost::lang::findCompileCommands;
using frost::lang::SynthesisOptions;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;

// Synthesis options for `configuration` (and system include directories).
frost::lang::SynthesisOptions forConfiguration(std::string configuration, std::vector<std::string> system = {}) {
    frost::lang::SynthesisOptions options;
    options.configuration = std::move(configuration);
    options.systemIncludeDirectories = std::move(system);
    return options;
}

std::string root() { return kWindows ? "C:\\w" : "/w"; }

// root()/<relative> with '/' turned into the native separator.
std::string under(std::string relative) {
    if (kWindows) {
        std::ranges::replace(relative, '/', '\\');
    }
    return frost::io::joinPath(root(), relative).value();
}

void touch(MemFs& fs, const std::string& file) {
    ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(file)).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(file, "[]").has_value());
}

std::string fixture(std::string relative) {
    if (kWindows) {
        std::ranges::replace(relative, '/', '\\');
    }
    const std::string base = frost::io::normalizePath(FROST_LANG_FIXTURE_DIR).value();
    return frost::io::normalizePath(frost::io::joinPath(base, relative).value()).value();
}

TEST(FindCompileCommands, SearchesTheKnownPlacesInOrder) {
    MemFs fs(frost::systemClock());
    ASSERT_TRUE(fs.createDirectories(root()).has_value());
    auto none = findCompileCommands(fs, root());
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());

    touch(fs, under("cmake-build-release/compile_commands.json"));
    touch(fs, under("cmake-build-debug/compile_commands.json"));
    auto clion = findCompileCommands(fs, root());
    ASSERT_TRUE(clion.has_value() && clion->has_value());
    EXPECT_EQ((*clion)->directory, under("cmake-build-debug")); // Name order.
    EXPECT_EQ((*clion)->source, CompileCommandsSource::Existing);

    touch(fs, under("out/build/x64-debug/compile_commands.json"));
    EXPECT_EQ(findCompileCommands(fs, root()).value()->directory, under("out/build/x64-debug"));
    touch(fs, under("build/compile_commands.json"));
    EXPECT_EQ(findCompileCommands(fs, root()).value()->directory, under("build"));
    touch(fs, under("compile_commands.json"));
    auto top = findCompileCommands(fs, root());
    EXPECT_EQ(top.value()->directory, root());
    EXPECT_EQ(top.value()->file, under("compile_commands.json"));

    EXPECT_FALSE(findCompileCommands(fs, "relative").has_value());
}

TEST(CompileCommandsJson, WritesArgumentsFormOnlyWhenChanged) {
    MemFs fs(frost::systemClock());
    const std::vector<CompileCommand> commands = {
        CompileCommand{.directory = root(),
                       .file = under("a b.cpp"),
                       .arguments = {"clang-cl", "/DX=\"1\"", "/c", under("a b.cpp")}},
    };
    auto parsed = frost::lang::parseJson(frost::lang::compileCommandsJson(commands));
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->size(), 1U);
    const auto& entry = parsed->items()[0];
    EXPECT_EQ(entry.stringAt("directory"), root());
    EXPECT_EQ(entry.stringAt("file"), under("a b.cpp"));
    ASSERT_EQ(entry.find("arguments")->size(), 4U);
    EXPECT_EQ(*entry.find("arguments")->items()[1].asString(), "/DX=\"1\"");

    const std::string cache = under("cache/db");
    EXPECT_TRUE(frost::lang::writeCompileCommands(fs, cache, commands).value());
    EXPECT_FALSE(frost::lang::writeCompileCommands(fs, cache, commands).value()); // Unchanged.
    EXPECT_TRUE(frost::lang::writeCompileCommands(fs, cache, {}).value());
}

TEST(VcxprojMetadata, FoldsItemDefinitionGroupsPerConfiguration) {
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(), fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    ASSERT_EQ(model->projects().size(), 1U);
    const auto& vcx = *model->projects()[0].vcxproj;
    const auto debug = frost::project::clCompileMetadata(vcx, "Debug|x64");
    EXPECT_EQ(debug.at("preprocessordefinitions").find("TINY_DEBUG=1;TINY_ALL"), 0U)
        << debug.at("preprocessordefinitions");
    EXPECT_EQ(debug.at("languagestandard"), "stdcpp20");
    const auto release = frost::project::clCompileMetadata(vcx, "release|X64"); // Case ignored.
    EXPECT_EQ(release.at("languagestandard"), "stdcpp17");
    EXPECT_EQ(release.count("additionalincludedirectories"), 0U);
}

bool contains(const std::vector<std::string>& arguments, const std::string& value) {
    return std::ranges::find(arguments, value) != arguments.end();
}

TEST(SynthesizeCompileCommands, UsesTheActiveConfiguration) {
    auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(), fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    const std::string system = kWindows ? "C:\\sdk\\include" : "/sdk/include";
    auto debug = frost::lang::synthesizeCompileCommands(*model, forConfiguration("Debug|x64", {system}));
    ASSERT_TRUE(debug.has_value()) << debug.error().describe();
    ASSERT_EQ(debug->size(), 1U);
    const CompileCommand& command = debug->front();
    EXPECT_EQ(command.file, fixture("msvc_solution/Tiny/src/main.cpp"));
    EXPECT_EQ(command.directory, fixture("msvc_solution/Tiny"));
    const auto& args = command.arguments;
    EXPECT_EQ(args.front(), "clang-cl");
    EXPECT_TRUE(contains(args, "--driver-mode=cl"));
    EXPECT_TRUE(contains(args, "--target=x86_64-pc-windows-msvc"));
    EXPECT_TRUE(contains(args, "/MDd"));
    EXPECT_TRUE(contains(args, "/EHsc"));
    EXPECT_TRUE(contains(args, "/permissive-"));
    EXPECT_TRUE(contains(args, "/DUNICODE"));
    EXPECT_TRUE(contains(args, "/DTINY_DEBUG=1"));
    EXPECT_TRUE(contains(args, "/DTINY_ALL"));
    EXPECT_TRUE(contains(args, "/I" + fixture("msvc_solution/Tiny/include")));
    EXPECT_TRUE(contains(args, "/imsvc" + system));
    EXPECT_TRUE(contains(args, "/std:c++20"));
    EXPECT_EQ(std::ranges::count_if(args, [](const std::string& a) { return a.find("$(") != std::string::npos; }), 0);
    EXPECT_EQ(args.back(), command.file);

    auto release = frost::lang::synthesizeCompileCommands(*model, forConfiguration("Release|x64"));
    ASSERT_TRUE(release.has_value());
    const auto& rargs = release->front().arguments;
    EXPECT_TRUE(contains(rargs, "/MT"));
    EXPECT_FALSE(contains(rargs, "/EHsc"));
    EXPECT_TRUE(contains(rargs, "/D_MBCS"));
    EXPECT_TRUE(contains(rargs, "/DNDEBUG"));
    EXPECT_FALSE(contains(rargs, "/DTINY_DEBUG=1"));
    EXPECT_TRUE(contains(rargs, "/std:c++17"));

    auto missing = frost::lang::synthesizeCompileCommands(*model, forConfiguration("Profile|x64"));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST(CompileCommandsProviders, SolutionGeneratorAndChain) {
    frost::test::ScratchDir scratch("lang-ccdb");
    auto& fs = frost::io::realFileSystem();
    auto model = frost::project::SolutionModel::load(fs, fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model.has_value());
    int msvcLookups = 0;
    auto solution = std::make_shared<frost::lang::SolutionCompileCommandsProvider>(
        fs, std::make_shared<const frost::project::SolutionModel>(std::move(*model)), scratch.join("synth"),
        forConfiguration("Debug|x64"), [&]() -> frost::Result<frost::lang::MsvcEnvironment> {
            ++msvcLookups;
            return frost::fail(ErrorCode::Unsupported, "no MSVC in this test");
        });
    auto first = solution->locate();
    ASSERT_TRUE(first.has_value() && first->has_value()) << (first ? "" : first.error().describe());
    EXPECT_EQ((*first)->source, CompileCommandsSource::Synthesized);
    EXPECT_EQ((*first)->directory, scratch.join("synth"));
    auto text = fs.readFile((*first)->file);
    ASSERT_TRUE(text.has_value());
    EXPECT_NE(text->find("TINY_DEBUG=1"), std::string::npos);
    ASSERT_TRUE(solution->locate().has_value());
    EXPECT_EQ(msvcLookups, 1); // Looked up once.

    auto empty = std::make_shared<frost::lang::GeneratorCompileCommandsProvider>(
        fs, scratch.join("gen"),
        []() -> frost::Result<std::vector<CompileCommand>> { return std::vector<CompileCommand>{}; });
    EXPECT_FALSE(empty->locate().value().has_value());
    auto failing = std::make_shared<frost::lang::GeneratorCompileCommandsProvider>(
        fs, scratch.join("gen"), []() -> frost::Result<std::vector<CompileCommand>> {
            return frost::fail(ErrorCode::IoError, "generator broke");
        });
    EXPECT_FALSE(failing->locate().has_value());

    frost::lang::FirstAvailableCompileCommandsProvider chain({failing, empty, solution});
    auto chosen = chain.locate();
    ASSERT_TRUE(chosen.has_value() && chosen->has_value());
    EXPECT_EQ((*chosen)->source, CompileCommandsSource::Synthesized);
    frost::lang::FirstAvailableCompileCommandsProvider broken({empty, failing});
    EXPECT_FALSE(broken.locate().has_value()); // The error surfaces when nothing was found.
}

} // namespace
