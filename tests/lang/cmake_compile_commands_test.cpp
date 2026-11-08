// SPDX-License-Identifier: Apache-2.0
//
// CMK-4 and CPP-3 with the real tools: the pinned cmake configures fixtures/cmake_contexts (two
// libraries listing one header with different defines) in a scratch copy through CMakeService, with
// FrostIDE's own C++ compiler (clang-cl with the captured MSVC environment on Windows, clang++ on
// Linux) and Ninja, and on Windows also with the Visual Studio 2022 generator. The oracle is the
// pinned clangd: `clangd --check` reports the compile command it takes from a database and the cc1
// arguments its driver derives (support/clangd_check.hpp).
//   - The commands synthesized from the Ninja configure's codemodel give clangd exactly the flags of
//     the compile_commands.json CMake wrote (cc1 arguments equal, commands equal but for the output
//     switches).
//   - The owned directory of the configuration holds CMake's database byte for byte; a resolve
//     context puts the chosen target's command first, so clangd parses the header with that
//     target's defines (its #error names the target), and Automatic restores CMake's database.
//   - Windows: a Visual Studio configure writes no database; the codemodel's synthesis gives clangd
//     a database command for every source, with the target's defines and the -std of its fragments.
// Label "integration" with the tags "cmake" and "clangd".

#include "frost/build/cmake_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/cmake_compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_compile_database.hpp"
#include "frost/project/trust_store.hpp"

#include "support/clangd_check.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace frost::lang {
namespace {

using namespace std::chrono_literals;

std::string native(std::string_view path) {
    auto normalized = io::normalizePath(path);
    EXPECT_TRUE(normalized.has_value()) << path;
    return normalized.value_or(std::string());
}

std::string join(const std::string& base, std::string_view relative) {
    auto joined = io::joinPath(base, relative);
    EXPECT_TRUE(joined.has_value()) << base << " + " << relative;
    return native(joined.value_or(std::string()));
}

void copyFixture(const std::string& to) {
    std::error_code error;
    std::filesystem::copy(std::filesystem::path(native(FROST_LANG_FIXTURE_DIR "/cmake_contexts")),
                          std::filesystem::path(to), std::filesystem::copy_options::recursive, error);
    ASSERT_FALSE(error) << error.message();
}

std::string clangdPath() {
    auto found = findClangd(io::realFileSystem(), defaultClangdSearch());
    EXPECT_TRUE(found.has_value()) << "the pinned clangd is required: " << (found ? "" : found.error().describe());
    return found ? found->path : std::string();
}

// A trusted folder's CMakeService configuring `folder` with one profile.
struct Configured {
    std::unique_ptr<project::TrustStore> trust;
    std::unique_ptr<build::CMakeService> service;
};

Configured configure(frost::test::ScratchDir& scratch, const std::string& folder, project::CMakeProfileRecord record) {
    Configured out;
    auto& fs = io::realFileSystem();
    auto store = project::TrustStore::open(fs, scratch.join("trusted-roots.txt"));
    EXPECT_TRUE(store.has_value());
    if (!store)
        return out;
    out.trust = std::move(*store);
    EXPECT_TRUE(out.trust->trust(folder).has_value());
    build::CMakeServiceOptions options;
    options.cmakePath = native(FROST_TEST_CMAKE);
    options.ninjaPath = native(FROST_TEST_NINJA);
    options.msvcCacheDirectory = scratch.join("msvc");
    options.profileRecords = {std::move(record)};
    build::CMakeServiceCallbacks callbacks;
    callbacks.onConfigureOutput = [](build::BuildOutputKind, std::string_view line) {
        std::cout << "[configure] " << line << '\n';
    };
    out.service = build::CMakeService::create(fs, io::realProcessLauncher(), *out.trust, options, callbacks);
    EXPECT_TRUE(out.service->openFolder(folder).has_value());
    EXPECT_TRUE(out.service->waitForIdle(300s));
    const auto outcome = out.service->lastConfigure();
    EXPECT_TRUE(outcome && outcome->succeeded())
        << (outcome && outcome->error ? outcome->error->describe() : std::string("cmake failed"));
    return out;
}

// The files of a compile_commands.json, in order.
std::vector<std::string> databaseFiles(const std::string& file) {
    std::vector<std::string> files;
    auto text = io::realFileSystem().readFile(file);
    EXPECT_TRUE(text.has_value()) << file;
    if (!text)
        return files;
    auto json = parseJson(*text);
    EXPECT_TRUE(json.has_value()) << file;
    if (!json)
        return files;
    for (const Json& entry : json->items())
        if (const Json* path = entry.find("file"); path && path->asString())
            files.push_back(*path->asString());
    return files;
}

project::CMakeProfileRecord ninjaRecord() {
    return project::CMakeProfileRecord{
        .name = "Debug",
        .generator = "Ninja",
        .cacheVariables = {std::string("CMAKE_CXX_COMPILER:FILEPATH=") + FROST_TEST_CXX_COMPILER}};
}

bool hasError(const frost::test::ClangdCheck& check, std::string_view message) {
    return std::ranges::any_of(check.errors,
                               [&](const std::string& e) { return e.find(message) != std::string::npos; });
}

TEST(CMakeCompileCommands, TheCodeModelGivesClangdTheFlagsOfCMakesDatabaseAndAResolveContextLeadsTheOwnedCopy) {
    const std::string clangd = clangdPath();
    ASSERT_FALSE(clangd.empty());
    frost::test::ScratchDir scratch("cmake-contexts-ninja");
    const std::string folder = scratch.join("contexts");
    copyFixture(folder);
    auto configured = configure(scratch, folder, ninjaRecord());
    ASSERT_TRUE(configured.service);
    const auto model = configured.service->model();
    ASSERT_NE(model, nullptr);
    const std::string buildDir = configured.service->activeBuildDirectory();
    const std::string cmakeDatabase = join(buildDir, "compile_commands.json");
    const std::vector<std::string> cmakeFiles = databaseFiles(cmakeDatabase);
    ASSERT_GE(cmakeFiles.size(), 3U) << "alpha.cpp, beta.cpp and alpha's precompiled-header source";

    // CMK-4: the codemodel's commands, written as a database of their own.
    auto& fs = io::realFileSystem();
    const std::string synthesized = scratch.join("synthesized");
    const auto commands = cmakeCodeModelCompileCommands(*model);
    ASSERT_TRUE(writeCompileCommands(fs, synthesized, commands).has_value());
    EXPECT_EQ(commands.size(), cmakeFiles.size()) << "one command per source CMake's database lists";
    for (const std::string& file : cmakeFiles) {
        SCOPED_TRACE(file);
        auto fromCMake = frost::test::runClangdCheck(clangd, file, buildDir);
        auto fromModel = frost::test::runClangdCheck(clangd, file, synthesized);
        ASSERT_TRUE(fromCMake.has_value()) << fromCMake.error().describe();
        ASSERT_TRUE(fromModel.has_value()) << fromModel.error().describe();
        ASSERT_EQ(fromCMake->origin, "from CDB") << fromCMake->log;
        ASSERT_EQ(fromModel->origin, "from CDB") << fromModel->log;
        ASSERT_FALSE(fromCMake->cc1.empty()) << fromCMake->log;
        EXPECT_EQ(fromModel->cc1, fromCMake->cc1) << "the parse flags differ";
        EXPECT_EQ(frost::test::withoutOutputSwitches(fromModel->command),
                  frost::test::withoutOutputSwitches(fromCMake->command));
        EXPECT_EQ(fromModel->errors, fromCMake->errors);
    }

    // CPP-3: the owned directory of the configuration, under the cache, holds CMake's database.
    const std::string cacheRoot = scratch.join("cache/contexts-workspace");
    CMakeCompileCommandsProvider provider(fs, cacheRoot, [&] {
        return CMakeCompileCommandsInput{.buildDirectory = buildDir, .configuration = {}, .model = model};
    });
    auto located = provider.locate();
    ASSERT_TRUE(located.has_value()) << located.error().describe();
    ASSERT_TRUE(located->has_value());
    const CompileCommandsLocation owned = **located;
    EXPECT_EQ(owned.source, CompileCommandsSource::CMakeCopy);
    EXPECT_EQ(documentKey(owned.origin), documentKey(buildDir));
    EXPECT_EQ(documentKey(io::parentPath(owned.directory)), documentKey(cacheRoot)) << owned.directory;
    EXPECT_EQ(fs.readFile(owned.file).value_or("owned"), fs.readFile(cmakeDatabase).value_or("cmake"));
    EXPECT_FALSE(fs.exists(join(buildDir, ".cache/clangd")).value_or(true)) << "nothing is written to the build tree";

    // The header is listed by both targets; each context gives clangd that target's defines.
    const std::string header = join(folder, "shared.h");
    const auto owners = project::cmakeTargetsListing(*model, header);
    ASSERT_EQ(owners.size(), 2U);
    EXPECT_EQ(model->targets[owners[0]].name, "alpha");
    EXPECT_EQ(model->targets[owners[1]].name, "beta");
    for (const std::string target : {"beta", "alpha"}) {
        SCOPED_TRACE(target);
        ASSERT_TRUE(provider.setContext(header, target).has_value());
        EXPECT_EQ(provider.context(header), target);
        ASSERT_TRUE(provider.locate().has_value());
        const std::vector<std::string> files = databaseFiles(owned.file);
        ASSERT_FALSE(files.empty());
        EXPECT_EQ(documentKey(files.front()), documentKey(header)) << "the chosen context comes first";
        EXPECT_EQ(files.size(), cmakeFiles.size() + 1);
        auto check = frost::test::runClangdCheck(clangd, header, owned.directory);
        ASSERT_TRUE(check.has_value()) << check.error().describe();
        EXPECT_EQ(check->origin, "from CDB") << check->log;
        EXPECT_TRUE(hasError(*check, "\"compiled for " + target + "\"")) << check->log;
        EXPECT_FALSE(hasError(*check, target == "alpha" ? "compiled for beta" : "compiled for alpha"));
    }
    // A provider started later (a clangd restart, the next session) reads the saved choice.
    CMakeCompileCommandsProvider reopened(fs, cacheRoot, [&] {
        return CMakeCompileCommandsInput{.buildDirectory = buildDir, .configuration = {}, .model = model};
    });
    EXPECT_EQ(reopened.context(header), "alpha");
    ASSERT_TRUE(reopened.setContext(header, "").has_value());
    ASSERT_TRUE(reopened.locate().has_value());
    EXPECT_EQ(fs.readFile(owned.file).value_or("owned"), fs.readFile(cmakeDatabase).value_or("cmake"))
        << "Automatic leaves CMake's database as it is";
}

#if defined(_WIN32)
TEST(CMakeCompileCommands, AVisualStudioConfigureGetsADatabaseFromItsCodeModel) {
    const std::string clangd = clangdPath();
    ASSERT_FALSE(clangd.empty());
    frost::test::ScratchDir scratch("cmake-contexts-vs");
    const std::string folder = scratch.join("contexts");
    copyFixture(folder);
    auto configured =
        configure(scratch, folder, project::CMakeProfileRecord{.name = "VS", .generator = "Visual Studio 17 2022"});
    ASSERT_TRUE(configured.service);
    const auto model = configured.service->model();
    ASSERT_NE(model, nullptr);
    EXPECT_TRUE(model->multiConfig);
    const std::string buildDir = configured.service->activeBuildDirectory();
    auto& fs = io::realFileSystem();
    ASSERT_FALSE(fs.exists(join(buildDir, "compile_commands.json")).value_or(true))
        << "the premise: Visual Studio generators write no compile_commands.json";

    const auto profile = configured.service->activeProfile();
    ASSERT_TRUE(profile.has_value());
    const std::string configuration = profile->effectiveConfiguration();
    CMakeCompileCommandsProvider provider(fs, scratch.join("cache/vs-workspace"), [&] {
        return CMakeCompileCommandsInput{.buildDirectory = buildDir, .configuration = configuration, .model = model};
    });
    auto located = provider.locate();
    ASSERT_TRUE(located.has_value()) << located.error().describe();
    ASSERT_TRUE(located->has_value()) << "no database from the codemodel";
    const CompileCommandsLocation owned = **located;
    EXPECT_EQ(owned.source, CompileCommandsSource::CMakeCodeModel);

    // Every source gets clangd's database command, with its target's flags, and parses cleanly.
    for (const auto& [source, define, standard] : {std::tuple{"alpha.cpp", "-D CONTEXT_ALPHA=1", ""},
                                                   std::tuple{"beta.cpp", "-D CONTEXT_BETA=1", "-std=c++20"}}) {
        SCOPED_TRACE(source);
        auto check = frost::test::runClangdCheck(clangd, join(folder, source), owned.directory);
        ASSERT_TRUE(check.has_value()) << check.error().describe();
        EXPECT_EQ(check->origin, "from CDB") << check->log;
        EXPECT_NE(check->cc1.find(define), std::string::npos) << check->cc1;
        EXPECT_NE(check->cc1.find("CMAKE_INTDIR="), std::string::npos) << "multi-config generators define it";
        if (*standard)
            EXPECT_NE(check->cc1.find(standard), std::string::npos) << check->cc1;
        EXPECT_TRUE(check->errors.empty()) << check->log;
    }
    const std::string header = join(folder, "shared.h");
    ASSERT_TRUE(provider.setContext(header, "beta").has_value());
    ASSERT_TRUE(provider.locate().has_value());
    auto check = frost::test::runClangdCheck(clangd, header, owned.directory);
    ASSERT_TRUE(check.has_value()) << check.error().describe();
    EXPECT_EQ(check->origin, "from CDB") << check->log;
    EXPECT_TRUE(hasError(*check, "\"compiled for beta\"")) << check->log;
}
#endif

} // namespace
} // namespace frost::lang
