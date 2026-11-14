// SPDX-License-Identifier: Apache-2.0
// Authored semantic-operation wire tests. The transport must preserve scope and refuse fake modes.
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/cpp_refactoring_plan.hpp"
#include "frost/sched/sim_executor.hpp"

#include <gtest/gtest.h>

#include <chrono>
namespace {
std::string path(std::string_view name) {
    return frost::io::joinPath(frost::io::kNativePathStyle == frost::io::PathStyle::Windows ? "C:/project" : "/project",
                               name)
        .value();
}
TEST(CppRefactoringPlan, SerializesExactByteSelectionAndParameterPermutation) {
    frost::lang::CppRefactoringRequest request;
    request.context.path = path("a.cpp");
    request.context.offset = 12;
    request.endOffset = 19;
    request.context.newName = "temporary";
    request.operation = "change-signature";
    request.parameterOrder = {2, 0, 1};
    const auto json = frost::lang::cppRefactoringRequestJson(request);
    EXPECT_EQ(json.stringAt("operation"), "change-signature");
    EXPECT_EQ(json.find("offset")->asUnsigned(), 12u);
    EXPECT_EQ(json.find("endOffset")->asUnsigned(), 19u);
    ASSERT_EQ(json.find("parameterOrder")->items().size(), 3u);
    EXPECT_EQ(json.find("parameterOrder")->items()[0].asUnsigned(), 2u);
}
TEST(CppRefactoringPlan, ExplicitSignatureRowsKeepUserTypesNamesAndNewCallExpressions) {
    frost::lang::CppRefactoringRequest request;
    request.operation = "edit-signature";
    request.editParameters = true;
    request.parameters = {{1, "long", "amount", {}}, {std::nullopt, "int", "bias", "4"}};
    const auto json = frost::lang::cppRefactoringRequestJson(request);
    EXPECT_EQ(json.stringAt("operation"), "edit-signature");
    ASSERT_EQ(json.find("parameters")->items().size(), 2u);
    const auto& old = json.find("parameters")->items()[0];
    EXPECT_EQ(old.find("oldIndex")->asUnsigned(), 1u);
    EXPECT_EQ(old.stringAt("type"), "long");
    const auto& added = json.find("parameters")->items()[1];
    EXPECT_EQ(added.find("oldIndex"), nullptr);
    EXPECT_EQ(added.stringAt("argument"), "4");
}
TEST(CppRefactoringPlan, UnknownOperationsCannotStartArbitraryHelperModes) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::SimExecutor executor;
    frost::io::ScriptedProcessLauncher launcher;
    frost::lang::CppRefactoringRequest request;
    request.context.projectRoot = path("");
    request.context.path = path("a.cpp");
    request.context.seedTranslationUnit = request.context.path;
    request.operation = "--execute-arbitrary";
    auto result =
        executor
            .spawn(frost::sched::Lane::Background,
                   frost::lang::requestCppRefactoringPlan(launcher, {**reactor, executor}, path("frost-clang"), request,
                                                          std::chrono::seconds(10)))
            .join();
    EXPECT_FALSE(result);
    EXPECT_TRUE(launcher.launches().empty());
}
TEST(CppRefactoringPlan, RealRefusalUsesDedicatedModeAndCannotCarryPartialEdits) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::SimExecutor executor;
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect(
        {.exitCode = 1,
         .stdoutBytes =
             R"({"kind":"cpp-refactoring-plan","status":"unsupported","files":[],"diagnostics":["macro range"]})"});
    frost::lang::CppRefactoringRequest request;
    request.context.projectRoot = path("");
    request.context.path = path("a.cpp");
    request.context.seedTranslationUnit = request.context.path;
    request.context.newName = "unused";
    request.context.compilationDatabase = "[]";
    request.operation = "inline-variable";
    auto result =
        executor
            .spawn(frost::sched::Lane::Background,
                   frost::lang::requestCppRefactoringPlan(launcher, {**reactor, executor}, path("frost-clang"), request,
                                                          std::chrono::seconds(10)))
            .join();
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->ready());
    EXPECT_TRUE(result->files.empty());
    ASSERT_EQ(launcher.launches().size(), 1u);
    EXPECT_EQ(launcher.launches()[0].spec.arguments, (std::vector<std::string>{"--plan-ast-refactoring"}));
    EXPECT_TRUE(launcher.launches()[0].spec.killOnClose);
}

TEST(CppRefactoringPlan, CreationPreservesExplicitImplementationAndUnknownReturnInferenceOnWire) {
    frost::lang::CppRefactoringRequest request;
    request.context.path = path("a.cpp"); request.context.seedTranslationUnit = request.context.path;
    request.context.offset = 31; request.endOffset = 38; request.operation = "create-from-usage";
    request.usageBody = "return left + right;";
    const auto json = frost::lang::cppRefactoringRequestJson(request);
    EXPECT_EQ(json.stringAt("operation"), "create-from-usage");
    EXPECT_EQ(json.stringAt("usageReturnType"), "");
    EXPECT_EQ(json.stringAt("usageBody"), "return left + right;");
}
TEST(CppRefactoringPlan, CreationUsesContainedAstModeAndRetainsRealRecoveryRefusal) {
    auto reactor = frost::io::Reactor::create(); ASSERT_TRUE(reactor);
    frost::sched::SimExecutor executor; frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.exitCode = 1, .stdoutBytes = R"({"kind":"cpp-refactoring-plan","status":"parse-error","files":[],"diagnostics":["unrelated baseline error"]})"});
    frost::lang::CppRefactoringRequest request;
    request.context.projectRoot = path(""); request.context.path = path("a.cpp");
    request.context.seedTranslationUnit = request.context.path; request.context.newName = "unused";
    request.context.compilationDatabase = "[]"; request.operation = "create-from-usage";
    request.usageReturnType = "int"; request.usageBody = "return 42;";
    auto result = executor.spawn(frost::sched::Lane::Background,
        frost::lang::requestCppRefactoringPlan(launcher, {**reactor, executor}, path("frost-clang"), request,
                                               std::chrono::seconds(10))).join();
    ASSERT_TRUE(result); EXPECT_FALSE(result->ready()); EXPECT_TRUE(result->files.empty());
    ASSERT_EQ(launcher.launches().size(), 1U);
    EXPECT_EQ(launcher.launches()[0].spec.arguments, (std::vector<std::string>{"--plan-ast-refactoring"}));
}
} // namespace
