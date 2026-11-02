// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the CMake File API (cmake_file_api.hpp) and the CMake view model
// (cmake_view_model.hpp): the query FrostIDE writes, the synthetic recorded reply under
// fixtures/cmake/reply read into the project model (the newest index, targets, artifacts, sources
// with the generated one flagged, includes, defines, flags, inputs, cache, toolchains), the
// fallback to the index's objects, and the Solution Explorer tree built from the model.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/project/cmake_view_model.hpp"

#include "cmake_reply_fixture.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace frost::project {
namespace {

using test::MemTree;

std::string source(std::string_view relative = {}) {
    return relative.empty() ? MemTree::path("tiny") : MemTree::path(std::string("tiny/") + std::string(relative));
}
std::string build(std::string_view relative = {}) {
    return relative.empty() ? source("cmake-build-debug")
                            : source(std::string("cmake-build-debug/") + std::string(relative));
}

// A MemTree holding the fixture reply for source() and build().
struct ReplyTree : MemTree {
    ReplyTree() {
        frost::test::installCMakeReply(fs, test::fixture("cmake/reply"), source(), build(), MemTree::path("other"));
    }
};

TEST(CMakeFileApi, WritesTheQueryOnce) {
    MemTree tree;
    const std::string path = cmakeFileApiQueryPath(build()).value();
    EXPECT_EQ(path, build(".cmake/api/v1/query/client-frostide/query.json"));
    ASSERT_TRUE(writeCMakeFileApiQuery(tree.fs, build()).has_value());
    const std::string text = tree.fs.readFile(path).value();
    for (std::string_view kind : {"codemodel", "cache", "cmakeFiles", "toolchains"}) {
        EXPECT_NE(text.find(std::string("\"") + std::string(kind) + "\""), std::string::npos) << kind;
    }
    const auto before = tree.fs.status(path).value();
    tree.clock.advance(std::chrono::seconds(5));
    ASSERT_TRUE(writeCMakeFileApiQuery(tree.fs, build()).has_value());
    EXPECT_EQ(tree.fs.status(path).value().mtime, before.mtime); // Unchanged text is not rewritten.
    EXPECT_FALSE(writeCMakeFileApiQuery(tree.fs, "relative").has_value());
}

TEST(CMakeFileApi, NoReplyIsNotAnError) {
    MemTree tree;
    auto none = readCMakeFileApiReply(tree.fs, build());
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());
}

TEST(CMakeFileApi, ReadsTheRecordedReply) {
    ReplyTree tree;
    auto read = readCMakeFileApiReply(tree.fs, build());
    ASSERT_TRUE(read.has_value()) << read.error().describe();
    ASSERT_TRUE(read->has_value());
    const CMakeProject& project = **read;
    EXPECT_EQ(project.cmakeVersion, "4.4.5"); // The newest index, not the stale one.
    EXPECT_EQ(project.generator, "Ninja");
    EXPECT_FALSE(project.multiConfig);
    EXPECT_EQ(project.name, "Tiny");
    EXPECT_EQ(project.sourceDirectory, source());
    EXPECT_EQ(project.buildDirectory, build());
    EXPECT_EQ(project.configuration, "Debug");
    EXPECT_EQ(project.configurations, std::vector<std::string>{"Debug"});
    ASSERT_EQ(project.targets.size(), 4U);

    const CMakeTarget* core = project.findTarget("core");
    ASSERT_NE(core, nullptr);
    EXPECT_EQ(core->type, CMakeTargetType::StaticLibrary);
    EXPECT_EQ(core->artifacts, std::vector<std::string>{build("libcore.a")});
    EXPECT_EQ(core->sourceDirectory, source());
    ASSERT_EQ(core->sources.size(), 4U);
    EXPECT_EQ(core->sources[0].path, source("src/core.c"));
    EXPECT_FALSE(core->sources[0].generated);
    EXPECT_EQ(core->sources[0].sourceGroup, "Source Files");
    EXPECT_EQ(core->sources[0].compileGroup, std::optional<std::size_t>(0));
    EXPECT_EQ(core->sources[1].path, source("include/core.h"));
    EXPECT_EQ(core->sources[1].compileGroup, std::nullopt);
    EXPECT_EQ(core->sources[2].path, build("generated/version.c"));
    EXPECT_TRUE(core->sources[2].generated);
    EXPECT_EQ(core->sources[3].path, MemTree::path("other/shared/extra.c"));
    ASSERT_EQ(core->compileGroups.size(), 1U);
    const CMakeCompileGroup& group = core->compileGroups[0];
    EXPECT_EQ(group.language, "C");
    EXPECT_EQ(group.includes, (std::vector<std::string>{source("include"), source("third_party")}));
    EXPECT_EQ(group.systemIncludes, std::vector<std::string>{source("third_party")});
    EXPECT_EQ(group.defines, (std::vector<std::string>{"TINY_CORE=1", "TINY_DEBUG"}));
    EXPECT_EQ(group.flags, (std::vector<std::string>{"-g", "-Wall"}));
    EXPECT_EQ(group.languageStandard, "11");

    const CMakeTarget* app = project.findTarget("app");
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->type, CMakeTargetType::Executable);
    EXPECT_EQ(app->folder, "Apps");
    EXPECT_EQ(app->sourceDirectory, source("app"));
    EXPECT_EQ(app->artifacts, std::vector<std::string>{build("app/app")});
    EXPECT_EQ(app->dependencies, std::vector<std::string>{"core::@6890427a1f51a3e7e1df"});
    EXPECT_TRUE(project.findTarget("ZERO_CHECK")->generatorProvided);
    EXPECT_EQ(project.findTarget("docs")->type, CMakeTargetType::Utility);

    ASSERT_EQ(project.inputs.size(), 5U);
    EXPECT_EQ(project.projectInputs(),
              (std::vector<std::string>{source("CMakeLists.txt"), source("cmake/warnings.cmake"),
                                        source("app/CMakeLists.txt")}));
    EXPECT_EQ(project.cacheValue("CMAKE_EXPORT_COMPILE_COMMANDS"), std::optional<std::string>("ON"));
    EXPECT_EQ(project.cacheValue("NOPE"), std::nullopt);
    ASSERT_EQ(project.toolchains.size(), 1U);
    EXPECT_EQ(project.toolchains[0].compilerId, "Clang");
    EXPECT_TRUE(io::isAbsolutePath(project.toolchains[0].compilerPath));

    EXPECT_EQ(readCMakeFileApiReply(tree.fs, build(), "release").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(readCMakeFileApiReply(tree.fs, build(), "DEBUG").value()->configuration, "Debug");
}

TEST(CMakeFileApi, FallsBackToTheIndexObjects) {
    ReplyTree tree;
    // Another client's build directory: the index has no client-frostide reply.
    const std::string index = build(".cmake/api/v1/reply/index-2026-11-02T08-30-00-0000.json");
    std::string text = tree.fs.readFile(index).value();
    frost::test::replaceAll(text, "client-frostide", "client-other");
    ASSERT_TRUE(tree.fs.writeFileAtomic(index, text).has_value());
    auto read = readCMakeFileApiReply(tree.fs, build());
    ASSERT_TRUE(read.has_value() && read->has_value()) << (read ? "" : read.error().describe());
    EXPECT_EQ((*read)->targets.size(), 4U);
    EXPECT_EQ((*read)->cache.size(), 2U);

    // A broken codemodel is an error.
    ASSERT_TRUE(tree.fs.writeFileAtomic(build(".cmake/api/v1/reply/codemodel-v2-0a1b.json"), "{").has_value());
    EXPECT_FALSE(readCMakeFileApiReply(tree.fs, build()).has_value());
}

TEST(CMakeViewModel, GroupsTargetSourcesByFolder) {
    ReplyTree tree;
    const CMakeProject project = readCMakeFileApiReply(tree.fs, build()).value().value();
    const std::vector<std::string> presets = {source("CMakePresets.json"), source("CMakeLists.txt")};
    const CMakeViewModel model = CMakeViewModel::build(project, presets);
    const auto labels = [&](std::size_t id) {
        std::vector<std::string> out;
        for (const std::size_t child : model.node(id).children) {
            out.push_back(model.node(child).label);
        }
        return out;
    };
    const CMakeViewNode& root = model.node(CMakeViewModel::root());
    EXPECT_EQ(root.kind, CMakeViewNodeKind::Project);
    EXPECT_EQ(root.label, "Tiny");
    EXPECT_EQ(root.path, source());
    // Targets by name (ZERO_CHECK is generator-provided), then the CMake group.
    EXPECT_EQ(labels(0), (std::vector<std::string>{"app", "core", "docs", "CMake"}));

    const std::size_t core = model.findKey("target:core").value();
    EXPECT_EQ(model.node(core).kind, CMakeViewNodeKind::Target);
    EXPECT_EQ(model.node(core).targetType, CMakeTargetType::StaticLibrary);
    EXPECT_EQ(labels(core), (std::vector<std::string>{"include", "src", "External", "Generated"}));
    const std::size_t generated = model.findKey("target:core/generated").value();
    ASSERT_EQ(model.node(generated).children.size(), 1U);
    const CMakeViewNode& version = model.node(model.node(generated).children.front());
    EXPECT_EQ(version.label, "version.c");
    EXPECT_TRUE(version.generated);
    EXPECT_EQ(version.path, build("generated/version.c"));
    EXPECT_EQ(version.target, std::optional<std::size_t>(1)); // core is the second target.

    const std::size_t app = model.findKey("target:app").value();
    EXPECT_EQ(labels(app), std::vector<std::string>{"app"});
    const std::size_t appFolder = model.node(app).children.front();
    EXPECT_EQ(model.node(appFolder).kind, CMakeViewNodeKind::Folder);
    EXPECT_EQ(labels(appFolder), std::vector<std::string>{"main.c"});

    // The CMake group: the project's inputs and the presets file, each once, folders first.
    const std::size_t cmake = model.findKey("cmake").value();
    EXPECT_EQ(model.node(cmake).kind, CMakeViewNodeKind::CMakeFiles);
    std::vector<std::string> files;
    for (const std::size_t child : model.node(cmake).children) {
        files.push_back(model.node(child).label);
        EXPECT_EQ(model.node(child).kind, CMakeViewNodeKind::CMakeFile);
    }
    EXPECT_EQ(files.size(), 4U);
    EXPECT_NE(std::ranges::find(files, "CMakePresets.json"), files.end());
    EXPECT_EQ(model.findPath(source("CMakeLists.txt")).size(), 1U);
    EXPECT_EQ(model.findPath(source("src/core.c")).size(), 1U);

    // Keys survive a rebuild of the same project.
    const CMakeViewModel again = CMakeViewModel::build(project, presets);
    EXPECT_EQ(again.findKey("target:core/generated"), model.findKey("target:core/generated"));
    const CMakeViewModel empty("Nothing");
    EXPECT_EQ(empty.nodes().size(), 1U);
    EXPECT_EQ(empty.node(0).label, "Nothing");
}

} // namespace
} // namespace frost::project
