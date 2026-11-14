// SPDX-License-Identifier: Apache-2.0
// Authored protocol/cancellation tests for read-only project rename. Not executed under user override.
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/cpp_rename_plan.hpp"
#include "frost/sched/sim_executor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
namespace {
using namespace std::chrono_literals;
using frost::lang::Json;
std::string path(std::string_view name) {
#ifdef _WIN32
    return "C:\\project\\" + std::string(name);
#else
    return "/project/" + std::string(name);
#endif
}
Json ready() {
    const std::string before = "int old;", after = "int renamed;";
    return Json::object()
        .set("kind", "cpp-rename-plan")
        .set("schemaVersion", 1)
        .set("status", "ready")
        .set("atomic", true)
        .set("translationUnitConfigurations", 1)
        .set("compilationDatabaseSha256", frost::io::sha256Hex("[]"))
        .set("files", Json::array({Json::object()
                                       .set("path", path("a.cpp"))
                                       .set("before", before)
                                       .set("after", after)
                                       .set("beforeSha256", frost::io::sha256Hex(before))
                                       .set("afterSha256", frost::io::sha256Hex(after))
                                       .set("overlay", true)}))
        .set("dependencies", Json::array({Json::object()
                                              .set("path", path("a.cpp"))
                                              .set("sha256", frost::io::sha256Hex(before))
                                              .set("bytes", before.size())
                                              .set("overlay", true)}))
        .set("missingDependencies", Json::array({Json(path("optional.h"))}))
        .set("directoryDependencies", Json::array({Json(path("include"))}));
}
TEST(CppRenamePlan, PreservesWholeImagesAndDirtyOverlayReadReceipts) {
    auto parsed = frost::lang::parseCppRenameProposal(ready().dump());
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(parsed->ready());
    ASSERT_EQ(parsed->files.size(), 1U);
    EXPECT_EQ(parsed->files[0].before, "int old;");
    EXPECT_TRUE(parsed->files[0].overlay);
    ASSERT_EQ(parsed->missingDependencies.size(), 1U);
    EXPECT_EQ(parsed->missingDependencies[0], path("optional.h"));
}
TEST(CppRenamePlan, RefusesTamperedImagesMissingReceiptsAndNonAtomicStatus) {
    auto value = ready();
    value.set("atomic", false);
    EXPECT_FALSE(frost::lang::parseCppRenameProposal(value.dump()));
    value = ready();
    value.set("dependencies", Json::array());
    EXPECT_FALSE(frost::lang::parseCppRenameProposal(value.dump()));
    value = ready();
    value.set("status", "conflict");
    EXPECT_FALSE(frost::lang::parseCppRenameProposal(value.dump()));
    value = ready();
    value.set("translationUnitConfigurations", 0);
    EXPECT_FALSE(frost::lang::parseCppRenameProposal(value.dump()));
    value = ready();
    value.set("missingDependencies", Json::array({Json(path("a.cpp"))}));
    EXPECT_FALSE(frost::lang::parseCppRenameProposal(value.dump()));
}
TEST(CppRenamePlan, RequestContainsCapturedCompileCommandsAndHeaderAutoSelection) {
    frost::lang::CppRenameRequest request;
    request.projectRoot = path("");
    request.path = path("a.h");
    request.newName = "Next";
    request.compilationDatabase = "[]";
    request.overlays = {{path("a.h"), "// utf8: \xC3\xA4\r\nint old;"}};
    auto value = frost::lang::cppRenameRequestJson(request);
    EXPECT_EQ(value.stringAt("seedTranslationUnit"), "");
    EXPECT_EQ(value.stringAt("compilationDatabase"), "[]");
    ASSERT_NE(value.find("overlays"), nullptr);
    EXPECT_EQ(value.find("overlays")->items()[0].stringAt("contents"), request.overlays[0].contents);
}
TEST(CppRenamePlan, AsyncServiceUsesContainedReadOnlyStdinProtocolAndChecksNativeExit) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::SimExecutor executor;
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect(frost::io::ScriptedProcess{.stdoutBytes = ready().dump()});
    frost::lang::CppRenameRequest request;
    request.projectRoot = path("");
    request.path = path("a.cpp");
    request.newName = "renamed";
    request.compilationDatabase = "[]";
    request.overlays = {{path("a.cpp"), "int old;"}};
    auto result = executor
                      .spawn(frost::sched::Lane::UserVisible,
                             frost::lang::requestCppRenamePlan(launcher, {**reactor, executor}, path("frost-clang.exe"),
                                                               request, 1s))
                      .join();
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->ready());
    ASSERT_EQ(launcher.launches().size(), 1U);
    const auto record = launcher.launches()[0];
    EXPECT_EQ(record.spec.arguments, std::vector<std::string>{"--plan-project-rename"});
    EXPECT_TRUE(record.spec.killOnClose);
    EXPECT_EQ(record.spec.stdinMode, frost::io::Stdio::Pipe);
    auto stdinJson = frost::lang::parseJson(record.stdinBytes);
    ASSERT_TRUE(stdinJson);
    EXPECT_EQ(stdinJson->stringAt("path"), request.path);
    EXPECT_TRUE(record.stdinClosed);
    launcher.expect(frost::io::ScriptedProcess{.exitCode = 8, .stdoutBytes = ready().dump()});
    EXPECT_FALSE(executor
                     .spawn(frost::sched::Lane::UserVisible,
                            frost::lang::requestCppRenamePlan(launcher, {**reactor, executor}, path("frost-clang.exe"),
                                                              request, 1s))
                     .join());
}
TEST(CppRenamePlan, AsyncTimeoutPropagatesAndTerminatesHungHelper) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::SimExecutor executor;
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect(frost::io::ScriptedProcess{.neverExits = true});
    frost::lang::CppRenameRequest request;
    request.projectRoot = path("");
    request.path = path("a.cpp");
    request.newName = "renamed";
    request.compilationDatabase = "[]";
    request.overlays = {{path("a.cpp"), "int old;"}};
    auto result = executor
                      .spawn(frost::sched::Lane::UserVisible,
                             frost::lang::requestCppRenamePlan(launcher, {**reactor, executor}, path("frost-clang.exe"),
                                                               request, 1ms))
                      .join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Timeout);
    EXPECT_TRUE(launcher.launches()[0].terminated);
}
} // namespace
