// SPDX-License-Identifier: Apache-2.0
//
// CPP-3 unit tests of the owned compile-commands directory (frost/lang/cmake_compile_commands.hpp)
// on a scratch directory and hand-built CMake models: one stable directory per configuration under
// the cache, nothing before a database exists or for another build directory's model, the
// codemodel's synthesis when CMake wrote no database, a resolve context prepended to CMake's own
// (also an empty one), and the didChangeConfiguration params clangd takes a context from. The real
// cmake and clangd check the same paths in lang_cmake_tests.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/cmake_compile_commands.hpp"
#include "frost/lang/uri.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace frost::lang {
namespace {

std::shared_ptr<project::CMakeProject> modelOf(const std::string& source, const std::string& build) {
    auto project = std::make_shared<project::CMakeProject>();
    project->sourceDirectory = source;
    project->buildDirectory = build;
    project->toolchains.push_back({.language = "CXX", .compilerPath = "/usr/bin/c++", .compilerId = "GNU"});
    for (const char* name : {"alpha", "beta"}) {
        project::CMakeTarget target;
        target.name = name;
        target.compileGroups.push_back({.language = "CXX", .defines = {std::string("IN_") + name}});
        target.sources.push_back({.path = *io::joinPath(source, std::string(name) + ".cpp"), .compileGroup = 0});
        target.sources.push_back({.path = *io::joinPath(source, "shared.h")});
        project->targets.push_back(std::move(target));
    }
    return project;
}

TEST(CMakeOwnedCompileCommands, OneStableDirectoryPerConfigurationUnderTheCache) {
    frost::test::ScratchDir scratch("owned-cdb-names");
    const std::string cache = scratch.join("cache");
    const auto debug = cmakeCompileCommandsDirectory(cache, scratch.join("src/build-debug"), "");
    const auto release = cmakeCompileCommandsDirectory(cache, scratch.join("src/build-release"), "");
    const auto vsDebug = cmakeCompileCommandsDirectory(cache, scratch.join("src/vs"), "Debug");
    const auto vsRelease = cmakeCompileCommandsDirectory(cache, scratch.join("src/vs"), "Release");
    ASSERT_TRUE(debug && release && vsDebug && vsRelease);
    EXPECT_NE(*debug, *release);
    EXPECT_NE(*vsDebug, *vsRelease);
    EXPECT_EQ(*debug, cmakeCompileCommandsDirectory(cache, scratch.join("src/build-debug"), "").value_or(""))
        << "the same configuration finds the same directory (and its shards) again";
    EXPECT_EQ(*vsDebug, cmakeCompileCommandsDirectory(cache, scratch.join("src/vs"), "DEBUG").value_or(""))
        << "configuration names compare without case, as CMake's do";
    EXPECT_EQ(documentKey(io::parentPath(*debug)), documentKey(cache));
    EXPECT_TRUE(io::fileName(*vsDebug).starts_with("cmake-vs-debug-")) << *vsDebug;
    EXPECT_FALSE(cmakeCompileCommandsDirectory("relative", scratch.join("b"), "").has_value());
}

TEST(CMakeOwnedCompileCommands, NothingBeforeADatabaseThenTheCodeModelThenCMakesOwnWithAContextFirst) {
    frost::test::ScratchDir scratch("owned-cdb-provider");
    auto& fs = io::realFileSystem();
    const std::string source = scratch.join("src");
    const std::string build = scratch.join("src/build");
    ASSERT_TRUE(fs.createDirectories(build).has_value());
    std::shared_ptr<const project::CMakeProject> model;
    CMakeCompileCommandsProvider provider(fs, scratch.join("cache"), [&] {
        return CMakeCompileCommandsInput{.buildDirectory = build, .configuration = {}, .model = model};
    });
    // No database and no model: nothing yet.
    auto none = provider.locate();
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());
    // Another build directory's model is not this configuration's.
    model = modelOf(source, scratch.join("src/other"));
    EXPECT_FALSE(provider.locate().value_or(std::nullopt).has_value());

    // CMake wrote no database (a Visual Studio generator): the codemodel's commands.
    model = modelOf(source, build);
    auto synthesized = provider.locate();
    ASSERT_TRUE(synthesized.has_value() && synthesized->has_value());
    EXPECT_EQ((*synthesized)->source, CompileCommandsSource::CMakeCodeModel);
    EXPECT_EQ(documentKey((*synthesized)->origin), documentKey(build));
    auto text = fs.readFile((*synthesized)->file);
    ASSERT_TRUE(text.has_value());
    auto json = parseJson(*text);
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ(json->size(), 2U);

    // CMake's own database (empty here) with beta chosen for the shared header: one entry, first.
    ASSERT_TRUE(fs.writeFileAtomic(*io::joinPath(build, "compile_commands.json"), "[\n]\n").has_value());
    const std::string header = *io::joinPath(source, "shared.h");
    ASSERT_TRUE(provider.setContext(header, "beta").has_value());
    auto copied = provider.locate();
    ASSERT_TRUE(copied.has_value() && copied->has_value());
    EXPECT_EQ((*copied)->source, CompileCommandsSource::CMakeCopy);
    json = parseJson(fs.readFile((*copied)->file).value_or(""));
    ASSERT_TRUE(json.has_value()) << "the merged database is valid JSON";
    ASSERT_EQ(json->size(), 1U);
    const Json* file = json->items()[0].find("file");
    ASSERT_TRUE(file && file->asString());
    EXPECT_EQ(documentKey(*file->asString()), documentKey(header));
    EXPECT_NE(json->items()[0].dump().find("-DIN_beta"), std::string::npos) << json->items()[0].dump();
    // A target the model does not have contributes nothing.
    ASSERT_TRUE(provider.setContext(header, "gamma").has_value());
    ASSERT_TRUE(provider.locate().has_value());
    EXPECT_EQ(fs.readFile((*copied)->file).value_or(""), "[\n]\n");
}

TEST(CMakeOwnedCompileCommands, AContextChangeIsClangdsCompilationDatabaseChange) {
    const CompileCommand command{.directory = "/b", .file = "/s/shared.h", .arguments = {"c++", "-DX", "/s/shared.h"}};
    EXPECT_EQ(compilationDatabaseChange(command).dump(),
              R"({"settings":{"compilationDatabaseChanges":{"/s/shared.h":{"workingDirectory":"/b",)"
              R"("compilationCommand":["c++","-DX","/s/shared.h"]}}}})");
}

} // namespace
} // namespace frost::lang
