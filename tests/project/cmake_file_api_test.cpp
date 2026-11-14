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
#include "frost/project/cmake_membership.hpp"
#include "frost/project/cmake_syntax.hpp"
#include "frost/project/cmake_view_model.hpp"

#include "cmake_reply_fixture.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

TEST(CMakeSyntax, TracksLiteralSpansAcrossOpaqueSyntax) {
    const std::string text = "# [=[ comment ( ) ]=]\nif((A OR B))\n"
                             "add_library(core STATIC \"src/a b.cpp\" ${SOURCES}) # comment\n"
                             "target_sources(core PRIVATE [=[foo ) bar]=] src/\xC3\xA4.cpp)\nendif()\n";
    auto commands = scanCMakeSyntax(text);
    ASSERT_TRUE(commands.has_value());
    ASSERT_EQ(commands->size(), 4U);
    const auto& declaration = (*commands)[1];
    EXPECT_EQ(declaration.name, "add_library");
    EXPECT_EQ(declaration.line, 3U);
    EXPECT_EQ(declaration.scopeDepth, 1U);
    EXPECT_TRUE((*commands)[0].nestedArguments);
    ASSERT_EQ(declaration.arguments.size(), 4U);
    EXPECT_EQ(declaration.arguments[2].literal, "src/a b.cpp");
    EXPECT_EQ(
        text.substr(declaration.arguments[2].begin, declaration.arguments[2].end - declaration.arguments[2].begin),
        "\"src/a b.cpp\"");
    EXPECT_FALSE(declaration.arguments[3].isLiteral);
    EXPECT_TRUE((*commands)[2].arguments[2].isLiteral);
    EXPECT_EQ((*commands)[2].arguments[2].literal, "foo ) bar");
    EXPECT_EQ((*commands)[2].arguments[3].literal, "src/\xC3\xA4.cpp");
    EXPECT_FALSE(scanCMakeSyntax("message([=[bad)").has_value());
    auto bom = scanCMakeSyntax("\xEF\xBB\xBF"
                               "add_library(core [=[dir/a b.cpp]=])\n");
    ASSERT_TRUE(bom.has_value());
    ASSERT_EQ((*bom)[0].arguments.size(), 2U);
    EXPECT_TRUE((*bom)[0].arguments[1].isLiteral);
    EXPECT_EQ((*bom)[0].arguments[1].literal, "dir/a b.cpp");
}

Result<CMakeMembershipPlan> planCMakeMembership(const CMakeProject& model, std::size_t target,
                                                CMakeMembershipOperation operation, std::string_view sourcePath,
                                                std::string_view definitionPath, std::string_view contents) {
    const std::array inputs{
        CMakeInputSnapshot{.path = source("CMakeLists.txt"),
                           .contents = "cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"},
        CMakeInputSnapshot{.path = std::string(definitionPath), .contents = std::string(contents)}};
    return frost::project::planCMakeMembership(model, target, operation, sourcePath, definitionPath, inputs);
}

CMakeProject editableProject() {
    CMakeProject model;
    model.sourceDirectory = source();
    model.buildDirectory = build();
    model.configurations = {"Debug"};
    model.inputsAvailable = true;
    model.cacheAvailable = true;
    model.inputs.push_back({.path = source("CMakeLists.txt")});
    model.inputs.push_back({.path = source("sub/CMakeLists.txt")});
    CMakeTarget target;
    target.name = "core";
    target.type = CMakeTargetType::StaticLibrary;
    target.sourceDirectory = source("sub");
    target.backtrace = {{.file = source("sub/CMakeLists.txt"), .line = 2, .command = "add_library"}};
    CMakeSource member;
    member.path = source("sub/dir/a b.cpp");
    member.backtraces = {{{.file = source("sub/CMakeLists.txt"), .line = 3, .command = "target_sources"}}};
    target.sources.push_back(std::move(member));
    model.targets.push_back(std::move(target));
    return model;
}

TEST(CMakeMembership, PlansTracedSubdirectoryAdditionAndTargetSourcesRemoval) {
    const auto model = editableProject();
    const std::string text = "# unchanged\r\nadd_library(core STATIC base.cpp) # keep\r\n"
                             "target_sources(core PRIVATE \"dir/a b.cpp\") # keep too\r\n";
    const auto path = source("sub/CMakeLists.txt");
    const auto extra = source("sub/dir/\xC3\xA4 new.cpp");
    auto add = planCMakeMembership(model, 0, CMakeMembershipOperation::Add, extra, path, text);
    ASSERT_TRUE(add.has_value()) << add.error().describe();
    EXPECT_EQ(add->original, text);
    EXPECT_EQ(add->replacement, "# unchanged\r\nadd_library(core STATIC base.cpp \"dir/\xC3\xA4 new.cpp\") # keep\r\n"
                                "target_sources(core PRIVATE \"dir/a b.cpp\") # keep too\r\n");
    auto remove =
        planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, source("sub/dir/a b.cpp"), path, text);
    ASSERT_TRUE(remove.has_value()) << remove.error().describe();
    EXPECT_EQ(remove->replacement, "# unchanged\r\nadd_library(core STATIC base.cpp) # keep\r\n"
                                   "target_sources(core PRIVATE ) # keep too\r\n");
}

TEST(CMakeMembership, RefusesUnsafeAndAmbiguousOrigins) {
    auto model = editableProject();
    const auto path = source("sub/CMakeLists.txt");
    const auto member = source("sub/dir/a b.cpp");
    const std::string text = "# unchanged\nadd_library(core STATIC base.cpp)\n"
                             "target_sources(core PRIVATE \"dir/a b.cpp\")\n";
    const auto plan = [&](std::string_view input) {
        return planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, path, input);
    };
    EXPECT_EQ(
        plan("# unchanged\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE ${FILES})\n").error().code(),
        ErrorCode::Unsupported);
    EXPECT_EQ(plan("if(ON)\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE \"dir/a b.cpp\")\nendif()\n")
                  .error()
                  .code(),
              ErrorCode::Unsupported);
    EXPECT_EQ(plan("# unchanged\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE \"dir/a b.cpp\" "
                   "\"dir/a b.cpp\")\n")
                  .error()
                  .code(),
              ErrorCode::Unsupported);
    model.targets[0].sources[0].backtraces.push_back(model.targets[0].sources[0].backtraces[0]);
    EXPECT_EQ(plan(text).error().code(), ErrorCode::Unsupported);
    model.targets[0].sources[0].backtraces.pop_back();
    model.targets[0].sources[0].generated = true;
    EXPECT_EQ(plan(text).error().code(), ErrorCode::Unsupported);
    model.targets[0].sources[0].generated = false;
    model.multiConfig = true;
    EXPECT_EQ(plan(text).error().code(), ErrorCode::Unsupported);
}

TEST(CMakeMembership, RequiresUniqueCommandAndOwnedInput) {
    auto model = editableProject();
    const auto path = source("sub/CMakeLists.txt");
    const auto member = source("sub/dir/a b.cpp");
    const auto plan = [&](std::string_view text) {
        return planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, path, text);
    };
    const std::string text = "# comment\nadd_library(core STATIC base.cpp)\n"
                             "target_sources(core PRIVATE \"dir/a b.cpp\")\n";
    EXPECT_EQ(plan("# comment\nadd_library(core STATIC base.cpp)\n"
                   "target_sources(other PRIVATE x.cpp) target_sources(core PRIVATE \"dir/a b.cpp\")\n")
                  .error()
                  .code(),
              ErrorCode::Unsupported);
    model.inputs[0].external = true;
    EXPECT_EQ(plan(text).error().code(), ErrorCode::Unsupported);
    model.inputs[0].external = false;
    model.cacheAvailable = false;
    EXPECT_EQ(plan(text).error().code(), ErrorCode::Unsupported);
    model.cacheAvailable = true;
    model.cache.push_back({.name = "CMAKE_PROJECT_Test_INCLUDE", .value = source("sub/CMakeLists.txt")});
    EXPECT_EQ(plan(text).error().code(), ErrorCode::Unsupported);
    model.cache.clear();
    model.targets[0].sources[0].backtraces[0].push_back(
        {.file = source("CMakeLists.txt"), .line = 5, .command = "include"});
    EXPECT_EQ(plan(text).error().code(), ErrorCode::Unsupported);
}

TEST(CMakeMembership, InspectsEveryConfiguredInputForOtherDeclarations) {
    auto model = editableProject();
    const auto path = source("sub/CMakeLists.txt");
    const auto other = source("sub/child/CMakeLists.txt");
    const auto member = source("sub/dir/a b.cpp");
    model.inputs.push_back({.path = other});
    std::string absolute = member;
    std::replace(absolute.begin(), absolute.end(), '\\', '/');
    std::array inputs{
        CMakeInputSnapshot{.path = source("CMakeLists.txt"),
                           .contents = "cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"},
        CMakeInputSnapshot{.path = path,
                           .contents = "# comment\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE "
                                       "\"dir/a b.cpp\")\nadd_subdirectory(child)\n"},
        CMakeInputSnapshot{.path = other, .contents = "target_sources(core PRIVATE \"" + absolute + "\")\n"}};
    auto plan = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, path, inputs);
    EXPECT_EQ(plan.error().code(), ErrorCode::Unsupported);
    inputs[2].contents = "target_sources(core PRIVATE ${FILES})\n";
    plan = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, path, inputs);
    EXPECT_EQ(plan.error().code(), ErrorCode::Unsupported);
    inputs[2].contents = "set_property(TARGET core APPEND PROPERTY SOURCES other.cpp)\n";
    plan = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, path, inputs);
    EXPECT_EQ(plan.error().code(), ErrorCode::Unsupported);
    inputs[2].contents = "# no other declarations\n";
    plan = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, path, inputs);
    EXPECT_TRUE(plan.has_value());
    plan = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, path,
                                               std::span(inputs).first(2));
    EXPECT_EQ(plan.error().code(), ErrorCode::Unsupported);
}

TEST(CMakeMembership, VerifiesGeneratedCompilerInputs) {
    auto model = editableProject();
    model.cmakeVersion = "4.4.5";
    const auto definition = source("sub/CMakeLists.txt");
    const auto generated = build("CMakeFiles/4.4.5/CMakeSystem.cmake");
    model.inputs.push_back({.path = generated, .generated = true});
    model.inputs.push_back({.path = generated, .generated = true});
    std::array inputs{
        CMakeInputSnapshot{.path = source("CMakeLists.txt"),
                           .contents = "cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"},
        CMakeInputSnapshot{.path = definition,
                           .contents = "# comment\nadd_library(core STATIC base.cpp)\n"
                                       "target_sources(core PUBLIC [=[dir/a b.cpp]=])\n"},
        CMakeInputSnapshot{.path = generated, .contents = "set(CMAKE_SYSTEM_NAME Linux)\n"}};
    auto result = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove,
                                                      source("sub/dir/a b.cpp"), definition, inputs);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->replacement, "# comment\nadd_library(core STATIC base.cpp)\ntarget_sources(core PUBLIC )\n");
    const auto rc = build("CMakeFiles/4.4.5/CMakeRCCompiler.cmake");
    model.inputs[2].path = rc;
    model.inputs[3].path = rc;
    inputs[2].path = rc;
    inputs[2].contents = "set(CMAKE_RC_COMPILER llvm-rc)\nset(CMAKE_RC_COMPILER_ARG1 \"\")\n"
                         "set(CMAKE_RC_COMPILER_LOADED 1)\nset(CMAKE_RC_SOURCE_FILE_EXTENSIONS rc;RC)\n"
                         "set(CMAKE_RC_OUTPUT_EXTENSION .res)\nset(CMAKE_RC_COMPILER_ENV_VAR RC)\n";
    result = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, source("sub/dir/a b.cpp"),
                                                 definition, inputs);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    inputs[2].contents += "set(MSVC_C_ARCHITECTURE_ID x64)\n";
    result = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, source("sub/dir/a b.cpp"),
                                                 definition, inputs);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    inputs[2].contents = "set(CMAKE_PROJECT_INCLUDE /tmp/unknown.cmake)\n";
    result = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, source("sub/dir/a b.cpp"),
                                                 definition, inputs);
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    inputs[2].contents = "target_sources(core PRIVATE malicious.cpp)\n";
    result = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, source("sub/dir/a b.cpp"),
                                                 definition, inputs);
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
}

TEST(CMakeMembership, RefusesRoleChangesAndQuotesOptionNamedNewSources) {
    auto model = editableProject();
    const auto definition = source("sub/CMakeLists.txt");
    const auto member = source("sub/first.cpp");
    model.targets[0].sources[0].path = member;
    model.targets[0].sources[0].backtraces[0][0] = {.file = definition, .line = 2, .command = "add_library"};
    auto result = planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, definition,
                                      "# comment\nadd_library(core STATIC first.cpp SHARED)\n");
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    for (const std::string_view role : {"UNKNOWN", "SYMBOLIC", "GLOBAL"}) {
        const std::string contents =
            "# comment\nadd_library(core STATIC first.cpp " + std::string(role) + " tail.cpp)\n";
        result = planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, definition, contents);
        EXPECT_EQ(result.error().code(), ErrorCode::Unsupported) << role;
    }
    model.targets[0].type = CMakeTargetType::Executable;
    model.targets[0].sources[0].backtraces[0][0].command = "add_executable";
    result = planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, definition,
                                 "# comment\nadd_executable(core first.cpp WIN32 tail.cpp)\n");
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    model.targets[0].type = CMakeTargetType::StaticLibrary;
    model.targets[0].backtrace[0].command = "add_library";
    model.targets[0].sources.clear();
    result = planCMakeMembership(model, 0, CMakeMembershipOperation::Add, source("sub/STATIC"), definition,
                                 "# comment\nadd_library(core STATIC)\n");
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->replacement, "# comment\nadd_library(core STATIC \"./STATIC\")\n");
    for (const std::string_view role : {"UNKNOWN", "SYMBOLIC", "GLOBAL"}) {
        result = planCMakeMembership(model, 0, CMakeMembershipOperation::Add,
                                     source(std::string("sub/") + std::string(role)), definition,
                                     "# comment\nadd_library(core STATIC)\n");
        ASSERT_TRUE(result.has_value()) << result.error().describe();
        EXPECT_EQ(result->replacement, "# comment\nadd_library(core STATIC \"./" + std::string(role) + "\")\n");
    }
}

TEST(CMakeMembership, RefusesFileSetRoles) {
    auto model = editableProject();
    const auto definition = source("sub/CMakeLists.txt");
    auto result =
        planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, model.targets[0].sources[0].path, definition,
                            "# comment\nadd_library(core STATIC base.cpp)\n"
                            "target_sources(core PUBLIC FILE_SET api TYPE HEADERS FILES \"dir/a b.cpp\")\n");
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
}

TEST(CMakeMembership, ResolvesNestedTargetSourcesOnlyWithProvenNewPolicy) {
    auto model = editableProject();
    const auto root = source("CMakeLists.txt");
    const auto targetFile = source("sub/CMakeLists.txt");
    const auto childFile = source("sub/child/CMakeLists.txt");
    const auto member = source("sub/child/thing.cpp");
    model.inputs = {{.path = root}, {.path = targetFile}, {.path = childFile}};
    model.targets[0].sources[0].path = member;
    model.targets[0].sources[0].backtraces = {{{.file = childFile, .line = 1, .command = "target_sources"},
                                               {.file = targetFile, .line = 2, .command = "add_subdirectory"}}};
    std::array inputs{
        CMakeInputSnapshot{.path = root,
                           .contents = "cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"},
        CMakeInputSnapshot{.path = targetFile,
                           .contents = "add_library(core STATIC base.cpp)\nadd_subdirectory(child)\n"},
        CMakeInputSnapshot{
            .path = childFile,
            .contents =
                "target_sources(core PRIVATE [=[thing.cpp]=]) # keep\ntarget_sources(core PRIVATE other.cpp)\n"}};
    auto result =
        frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, childFile, inputs);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->replacement, "target_sources(core PRIVATE ) # keep\ntarget_sources(core PRIVATE other.cpp)\n");
    inputs[0].contents = "add_subdirectory(sub)\ncmake_minimum_required(VERSION 3.20)\nproject(Test)\n";
    result = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, childFile, inputs);
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    inputs[0].contents = "cmake_minimum_required(VERSION 3.12)\nproject(Test)\nadd_subdirectory(sub)\n";
    result = frost::project::planCMakeMembership(model, 0, CMakeMembershipOperation::Remove, member, childFile, inputs);
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
}

TEST(CMakeMembership, DoesNotTreatSiblingPrefixAsRelative) {
    const auto model = editableProject();
    const auto path = source("sub/CMakeLists.txt");
    const auto sibling = source("submarine/file.cpp");
    auto result = planCMakeMembership(model, 0, CMakeMembershipOperation::Add, sibling, path,
                                      "# comment\nadd_library(core STATIC base.cpp)\n");
    ASSERT_TRUE(result.has_value());
    std::string expected = sibling;
    std::replace(expected.begin(), expected.end(), '\\', '/');
    EXPECT_EQ(result->replacement, "# comment\nadd_library(core STATIC base.cpp \"" + expected + "\")\n");
}

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
    ASSERT_EQ(core->backtrace.size(), 2U);
    EXPECT_EQ(core->backtrace[0].file, source("sub/part.cmake"));
    EXPECT_EQ(core->backtrace[0].line, 9U);
    EXPECT_EQ(core->backtrace[0].command, "add_library");
    EXPECT_EQ(core->backtrace[1].file, source("CMakeLists.txt"));
    EXPECT_EQ(core->backtrace[1].command, "include");
    EXPECT_EQ(core->artifacts, std::vector<std::string>{build("libcore.a")});
    EXPECT_EQ(core->sourceDirectory, source());
    ASSERT_EQ(core->sources.size(), 4U);
    EXPECT_EQ(core->sources[0].path, source("src/core.c"));
    ASSERT_EQ(core->sources[0].backtraces.size(), 1U);
    EXPECT_EQ(core->sources[0].backtraces[0], core->backtrace);
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

TEST(CMakeFileApi, RetainsEverySourceOrigin) {
    ReplyTree tree;
    const std::string path = build(".cmake/api/v1/reply/target-core-Debug-44dd.json");
    std::string text = tree.fs.readFile(path).value();
    frost::test::replaceAll(
        text, "\"backtrace\": 1, \"compileGroupIndex\": 0, \"path\": \"src/core.c\"",
        "\"backtrace\": 1, \"backtraces\": [1, 0], \"compileGroupIndex\": 0, \"path\": \"src/core.c\"");
    ASSERT_TRUE(tree.fs.writeFileAtomic(path, text).has_value());
    auto read = readCMakeFileApiReply(tree.fs, build());
    ASSERT_TRUE(read.has_value() && read->has_value());
    const auto* core = (*read)->findTarget("core");
    ASSERT_NE(core, nullptr);
    ASSERT_EQ(core->sources[0].backtraces.size(), 2U);
    EXPECT_EQ(core->sources[0].backtraces[0].size(), 2U);
    EXPECT_EQ(core->sources[0].backtraces[1].size(), 1U);
}

TEST(CMakeFileApi, RejectsMalformedNewOriginArrayInsteadOfFallingBack) {
    ReplyTree tree;
    const std::string path = build(".cmake/api/v1/reply/target-core-Debug-44dd.json");
    std::string text = tree.fs.readFile(path).value();
    frost::test::replaceAll(
        text, "\"backtrace\": 1, \"compileGroupIndex\": 0, \"path\": \"src/core.c\"",
        "\"backtrace\": 1, \"backtraces\": [1, 999], \"compileGroupIndex\": 0, \"path\": \"src/core.c\"");
    ASSERT_TRUE(tree.fs.writeFileAtomic(path, text).has_value());
    auto read = readCMakeFileApiReply(tree.fs, build());
    ASSERT_TRUE(read.has_value() && read->has_value());
    const auto* core = (*read)->findTarget("core");
    ASSERT_NE(core, nullptr);
    EXPECT_TRUE(core->sources[0].backtraces.empty());
}

TEST(CMakeFileApi, RejectsCyclicBacktraceWithoutInventingOrigin) {
    ReplyTree tree;
    const std::string path = build(".cmake/api/v1/reply/target-core-Debug-44dd.json");
    std::string text = tree.fs.readFile(path).value();
    frost::test::replaceAll(text, "\"parent\": 0", "\"parent\": 1");
    ASSERT_TRUE(tree.fs.writeFileAtomic(path, text).has_value());
    auto read = readCMakeFileApiReply(tree.fs, build());
    ASSERT_TRUE(read.has_value() && read->has_value());
    const auto* core = (*read)->findTarget("core");
    ASSERT_NE(core, nullptr);
    EXPECT_TRUE(core->backtrace.empty());
    ASSERT_FALSE(core->sources.empty());
    EXPECT_TRUE(core->sources[0].backtraces.empty());
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

TEST(CMakeMembershipRename, PreservesTokensCommentsVisibilityAndUtf8CrLf) {
    auto model = editableProject();
    const auto definition = source("sub/CMakeLists.txt");
    const std::string text = "# unchanged\r\nadd_library(core STATIC base.cpp) # retain\r\n"
                             "target_sources(core PRIVATE \"dir/a b.cpp\") # unchanged\r\n";
    const std::array inputs{
        CMakeInputSnapshot{source("CMakeLists.txt"),
                           "cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"},
        CMakeInputSnapshot{definition, text}};
    auto plan = planCMakeSourceRename(model, 0, source("sub/dir/a b.cpp"), source("sub/dir/\xC3\xA4 new.cpp"),
                                      definition, inputs);
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_EQ(plan->original, text);
    EXPECT_EQ(plan->replacement, "# unchanged\r\nadd_library(core STATIC base.cpp) # retain\r\n"
                                 "target_sources(core PRIVATE \"dir/\xC3\xA4 new.cpp\") # unchanged\r\n");
}

TEST(CMakeMembershipRename, KeepsOptionLookingLeafInSourceRole) {
    auto model = editableProject();
    model.targets[0].sources[0].path = source("sub/old.cpp");
    const auto definition = source("sub/CMakeLists.txt");
    const std::array inputs{
        CMakeInputSnapshot{source("CMakeLists.txt"),
                           "cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"},
        CMakeInputSnapshot{definition,
                           "# keep\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE old.cpp)\n"}};
    auto plan = planCMakeSourceRename(model, 0, source("sub/old.cpp"), source("sub/PRIVATE"), definition, inputs);
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_EQ(plan->replacement,
              "# keep\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE \"./PRIVATE\")\n");
}

TEST(CMakeMembershipRename, RefusesOtherTargetDestinationDuplicateAndIndirectMembership) {
    auto model = editableProject();
    const auto definition = source("sub/CMakeLists.txt");
    std::array inputs{
        CMakeInputSnapshot{source("CMakeLists.txt"),
                           "cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"},
        CMakeInputSnapshot{
            definition, "# keep\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE \"dir/a b.cpp\")\n"}};
    auto rename = [&] {
        return planCMakeSourceRename(model, 0, source("sub/dir/a b.cpp"), source("sub/dir/new.cpp"), definition,
                                     inputs);
    };
    model.targets.push_back(model.targets[0]);
    EXPECT_FALSE(rename());
    model.targets.pop_back();
    auto destination = model.targets[0].sources[0];
    destination.path = source("sub/dir/new.cpp");
    model.targets[0].sources.push_back(destination);
    EXPECT_EQ(rename().error().code(), ErrorCode::AlreadyExists);
    model.targets[0].sources.pop_back();
    inputs[1].contents += "target_sources(core PRIVATE \"dir/a b.cpp\")\n";
    EXPECT_FALSE(rename());
    inputs[1].contents = "# keep\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE ${SOURCES})\n";
    EXPECT_FALSE(rename());
    EXPECT_FALSE(
        planCMakeSourceRename(model, 0, source("sub/dir/a b.cpp"), source("else/new.cpp"), definition, inputs));
}

TEST(CMakeMembershipRename, RefusesDotTrailingSlashAndNonNormalizedDestinations) {
    const auto model = editableProject();
    const auto definition = source("sub/CMakeLists.txt");
    const std::array inputs{
        CMakeInputSnapshot{source("CMakeLists.txt"),
                           "cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"},
        CMakeInputSnapshot{
            definition, "# keep\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE \"dir/a b.cpp\")\n"}};
    const auto parent = source("sub/dir");
    for (const auto& suffix : {"/.", "/", "/..", "/./new.cpp", "//new.cpp"}) {
        auto plan = planCMakeSourceRename(model, 0, source("sub/dir/a b.cpp"), parent + suffix, definition, inputs);
        ASSERT_FALSE(plan) << suffix;
        EXPECT_EQ(plan.error().code(), ErrorCode::InvalidArgument) << suffix;
    }
}


TEST(CMakeMembershipRename, AggregatesSharedDefinitionFromOriginalBytesAcrossTargets) {
    auto model = editableProject();
    auto other = model.targets[0];
    other.name = "other";
    other.backtrace[0].line = 4;
    other.sources[0].backtraces[0][0].line = 4;
    other.sources[0].backtraces[0][0].command = "add_library";
    model.targets.push_back(other);
    const auto definition = source("sub/CMakeLists.txt");
    const std::string text = "# keep\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE \"dir/a b.cpp\")\nadd_library(other STATIC \"dir/a b.cpp\")\n";
    const std::array inputs{CMakeInputSnapshot{source("CMakeLists.txt"), "cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"}, CMakeInputSnapshot{definition, text}};
    auto plans = planCMakeSourceRenameAllTargets(model, source("sub/dir/a b.cpp"), source("sub/dir/new.cpp"), inputs);
    ASSERT_TRUE(plans) << plans.error().describe();
    ASSERT_EQ(plans->size(), 1U);
    EXPECT_EQ((*plans)[0].original, text);
    EXPECT_EQ((*plans)[0].replacement, "# keep\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE \"dir/new.cpp\")\nadd_library(other STATIC \"dir/new.cpp\")\n");
    model.targets.back().sources[0].generated = true;
    EXPECT_FALSE(planCMakeSourceRenameAllTargets(model, source("sub/dir/a b.cpp"), source("sub/dir/new.cpp"), inputs));
}


TEST(CMakeMembershipRename, RefusesCurrentTargetMembershipMissingFromPublishedModel) {
    const auto model=editableProject();
    const auto definition=source("sub/CMakeLists.txt");
    const std::array inputs{CMakeInputSnapshot{source("CMakeLists.txt"),"cmake_minimum_required(VERSION 3.20)\nproject(Test)\nadd_subdirectory(sub)\n"},CMakeInputSnapshot{definition,"# keep\nadd_library(core STATIC base.cpp)\ntarget_sources(core PRIVATE \"dir/a b.cpp\")\nadd_library(new_target STATIC \"dir/a b.cpp\")\n"}};
    auto result=planCMakeSourceRenameAllTargets(model,source("sub/dir/a b.cpp"),source("sub/dir/new.cpp"),inputs);
    ASSERT_FALSE(result);EXPECT_EQ(result.error().code(),ErrorCode::Unsupported);
}

} // namespace
} // namespace frost::project
