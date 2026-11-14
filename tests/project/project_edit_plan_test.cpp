// SPDX-License-Identifier: Apache-2.0
// Versioned plan-only protocol and cancellation/dirty-overlay admission; no helper needed.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/vcxproj_rename.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>

namespace frost::project {
namespace {

using test::MemTree;
std::string quoted(std::string_view value) {
    std::string result = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"')
            result += '\\';
        result += c;
    }
    return result + '"';
}
std::string answer(std::string_view project = MemTree::path("App/App.vcxproj")) {
    return "{\"Error\":null,\"Edit\":{\"PlanVersion\":1,\"Mode\":\"BeforeFileMoveRename\",\"ProjectPath\":" +
           quoted(project) +
           R"(,"ProjectBefore":"PFByb2plY3QvPg==","Project":"PFByb2plY3QvPg==",)"
           R"("FiltersBefore":null,"Filters":null,"Configurations":["Debug|x64"],"Environment":"captured",)"
           R"("InputBytes":[]}})";
}
ProjectEdit edit() {
    ProjectEdit result;
    result.kind = ProjectEditKind::RenameFiles;
    result.root = MemTree::root();
    result.definition = MemTree::path("App/App.vcxproj");
    result.files = {MemTree::path("App/old.cpp")};
    result.renamedTo = {MemTree::path("App/new.cpp")};
    result.configuration = "Debug";
    result.platform = "x64";
    result.transactionId = "9cdb031c-2eb4-4765-a6f9-62bbeb5f3a15";
    result.commit = false;
    result.beforeFileMove = true;
    return result;
}
ProjectEditLaunch launch() {
    return {.hostPath = MemTree::path("tools/ProjectHost.exe"), .msbuildPath = MemTree::path("tools/MSBuild.exe")};
}

TEST(ProjectEditPlan, DecodesExactBeforeAfterAndObservedFilterAbsence) {
    auto plan = parseProjectEditPlan(answer());
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_EQ(plan->projectBefore, "<Project/>");
    EXPECT_EQ(plan->projectAfter, "<Project/>");
    EXPECT_FALSE(plan->filtersBefore);
    EXPECT_FALSE(plan->filtersAfter);
    EXPECT_EQ(plan->configurations, (std::vector<std::string>{"Debug|x64"}));
}

TEST(ProjectEditPlan, MalformedVersionDuplicateKeysAndNoncanonicalBase64AreRejected) {
    for (auto [from, to] : {std::pair{"\"PlanVersion\":1", "\"PlanVersion\":2"},
                            std::pair{"\"PlanVersion\":1", "\"PlanVersion\":1,\"PlanVersion\":1"},
                            std::pair{"PFByb2plY3QvPg==", "PFByb2plY3QvPh=="},
                            std::pair{"\"Filters\":null", "\"Filters\":\"PFByb2plY3QvPg==\""},
                            std::pair{"\"Debug|x64\"", "\"Debug|x64\",\"Debug|x64\""}}) {
        auto response = answer();
        auto at = response.find(from);
        ASSERT_NE(at, std::string::npos);
        response.replace(at, std::string_view(from).size(), to);
        EXPECT_FALSE(parseProjectEditPlan(response)) << response;
    }
}

TEST(ProjectEditPlan, BeforeMoveCannotBeUsedByTheIndependentCommittingPath) {
    auto request = edit();
    auto line = projectEditRequest(request);
    ASSERT_TRUE(line) << line.error().describe();
    EXPECT_NE(line->find("\"BeforeFileMove\":\"true\""), std::string::npos);
    EXPECT_NE(line->find("\"Commit\":\"false\""), std::string::npos);
    request.commit = true;
    EXPECT_FALSE(projectEditRequest(request));
    request.commit = false;
    request.kind = ProjectEditKind::RemoveFiles;
    EXPECT_FALSE(projectEditRequest(request));
}

TEST(ProjectEditPlan, OwnedWorkerReturnsPlanAndNeverSendsAnApproval) {
    io::ScriptedProcessLauncher processes;
    processes.expect({.stdoutBytes = answer() + "\n", .outputHeld = true});
    auto plan = runProjectEditPlan(processes, launch(), edit());
    ASSERT_TRUE(plan) << plan.error().describe();
    ASSERT_EQ(processes.launches().size(), 1);
    EXPECT_TRUE(processes.launches()[0].stdinClosed);
    EXPECT_NE(processes.launches()[0].stdinBytes.find("BeforeFileMove"), std::string::npos);
    EXPECT_EQ(processes.launches()[0].stdinBytes.find("Decision"), std::string::npos);
    EXPECT_TRUE(processes.launches()[0].terminated);
    EXPECT_TRUE(processes.launches()[0].outputAbandoned);
}

TEST(ProjectEditPlan, CancellationRacingLaunchEndsTheOwnedHelperBeforeBlockingReads) {
    class CancellingLauncher final : public io::IProcessLauncher {
    public:
        io::ScriptedProcessLauncher scripted;
        std::stop_source stop;
        Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
            auto process = scripted.launch(spec);
            stop.request_stop(); // Races between successful launch and callback registration.
            return process;
        }
        Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                               const io::AsyncContext& context) override {
            return scripted.launchAsync(spec, context);
        }
    } processes;
    processes.scripted.expect({.neverExits = true, .outputHeld = true});
    auto result = runProjectEditPlan(processes, launch(), edit(), processes.stop.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    ASSERT_EQ(processes.scripted.launches().size(), 1);
    EXPECT_TRUE(processes.scripted.launches()[0].terminated);
    EXPECT_TRUE(processes.scripted.launches()[0].stdinClosed);
}

TEST(ProjectEditPlan, UnexpectedReadyAndDifferentProjectAreNotAcceptedAsPlans) {
    io::ScriptedProcessLauncher processes;
    processes.expect(
        {.stdoutBytes = R"({"Ready":true,"TransactionId":"9cdb031c-2eb4-4765-a6f9-62bbeb5f3a15","JournalDigest":")" +
                        std::string(64, 'a') + "\"}\n"});
    EXPECT_FALSE(runProjectEditPlan(processes, launch(), edit()));
    EXPECT_EQ(processes.launches()[0].stdinBytes.find("Decision"), std::string::npos);
    processes.expect({.stdoutBytes = answer(MemTree::path("Other.vcxproj")) + "\n"});
    EXPECT_FALSE(runProjectEditPlan(processes, launch(), edit()));
}

TEST(ProjectEditPlan, CancellationBeforeLaunchStartsNothingAndDirtyDefinitionRefusesThePlan) {
    io::ScriptedProcessLauncher processes;
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = runProjectEditPlan(processes, launch(), edit(), stop.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE(processes.launches().empty());
    const auto request = edit();
    std::vector<VcxprojRenameInput> inputs{{request.definition, "<Project><!--dirty--></Project>"},
                                           {request.definition + ".filters", std::nullopt}};
    processes.expect({.stdoutBytes = answer() + "\n"});
    auto dirty = planVcxprojRenameWithHost(processes, launch(), request, inputs);
    ASSERT_FALSE(dirty);
    EXPECT_EQ(dirty.error().code(), ErrorCode::Busy);
    EXPECT_EQ(*inputs[0].contents, "<Project><!--dirty--></Project>");
}

} // namespace
} // namespace frost::project
namespace frost::project {
namespace {
TEST(ProjectEditPlan, PreCreatePlanHasItsOwnModeAndCannotReachTheCommittingPath) {
    auto request = edit();
    request.kind = ProjectEditKind::AddFiles;
    request.renamedTo.clear();
    request.beforeFileMove = false;
    request.beforeFileCreate = true;
    auto line = projectEditRequest(request);
    ASSERT_TRUE(line);
    EXPECT_NE(line->find("\"BeforeFileCreate\":\"true\""), std::string::npos);
    auto response = answer();
    const auto mode = response.find("BeforeFileMoveRename");
    ASSERT_NE(mode, std::string::npos);
    response.replace(mode, std::string_view("BeforeFileMoveRename").size(), "BeforeFileCreateAdd");
    io::ScriptedProcessLauncher processes;
    processes.expect({.stdoutBytes = response + "\n", .outputHeld = true});
    auto plan = runProjectEditPlan(processes, launch(), request);
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_EQ(plan->mode, "BeforeFileCreateAdd");
    EXPECT_EQ(processes.launches().front().stdinBytes.find("Decision"), std::string::npos);
    request.commit = true;
    EXPECT_FALSE(projectEditRequest(request));
    request.commit = false;
    request.beforeFileMove = true;
    EXPECT_FALSE(projectEditRequest(request));
}
TEST(ProjectEditPlan, APreMoveResponseCannotSubstituteForAPreCreateReceipt) {
    auto request = edit();
    request.kind = ProjectEditKind::AddFiles;
    request.renamedTo.clear();
    request.beforeFileMove = false;
    request.beforeFileCreate = true;
    io::ScriptedProcessLauncher processes;
    processes.expect({.stdoutBytes = answer() + "\n", .outputHeld = true});
    EXPECT_FALSE(runProjectEditPlan(processes, launch(), request));
}
} // namespace
} // namespace frost::project
