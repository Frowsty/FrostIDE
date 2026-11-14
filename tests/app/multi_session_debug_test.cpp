// SPDX-License-Identifier: Apache-2.0
// Compound launch transactions and project lifetime gates. Real adapters have a separate acceptance.
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "../../src/app/src/multi_session_debug.hpp"
#include "debug/debug_test_util.hpp"
#include "workbench_rig.hpp"
#include <QAction>
#include <QMenu>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>
namespace frost::app {
namespace {
using namespace std::chrono_literals;
using test::RigOptions;
using test::WorkbenchRig;
using test::WorkDir;

TEST(CompoundDebugCoordinator, PreparesEveryMemberBeforeLaunchAndCancelsRevokedWork) {
    WorkDir dir("compound-prepare");
    std::vector<std::unique_ptr<debug::FakeDebugAdapter>> peers;
    RigOptions options;
    options.workbench.debugAdapterFactory =
        [&](debug::DapHandlers handlers) -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
        auto peer = std::make_unique<debug::FakeDebugAdapter>();
        debug::scriptProgram(*peer, debug::FakeProgram{.runsForever = true});
        auto transport = peer->transport();
        peers.push_back(std::move(peer));
        return debug::makeDapClient(std::move(transport), std::move(handlers));
    };
    WorkbenchRig rig(dir, std::move(options));
    auto* sessions = rig.workbench->debugSessions();
    ASSERT_NE(sessions, nullptr);
    MultiSessionDebugCoordinator coordinator(*sessions);
    bool admitted = true;
    int launches = 0;
    std::vector<MultiSessionDebugCoordinator::Prepared> waiting;
    std::vector<run::RunConfiguration> configs;
    for (const auto id : {"one", "two"}) {
        run::RunConfiguration member;
        member.id = id;
        member.name = id;
        member.program = dir.join("App.exe");
        configs.push_back(std::move(member));
    }
    auto group = coordinator.start(
        configs, [&] { return admitted; },
        [&](const run::RunConfiguration&, std::stop_token,
            MultiSessionDebugCoordinator::Prepared done) -> Result<void> {
            waiting.push_back(std::move(done));
            return {};
        });
    ASSERT_TRUE(group);
    ASSERT_EQ(waiting.size(), 1U);
    auto firstDone = waiting[0];
    firstDone(PreparedDebugLaunch{[&](ui::DebugSessionRegistry&, ui::DebugGroupId) -> Result<ui::DebugSessionId> {
        ++launches;
        return fail(ErrorCode::IoError, "not reached before all preparation");
    }});
    ASSERT_EQ(waiting.size(), 2U);
    EXPECT_EQ(launches, 0);
    admitted = false;
    auto secondDone = waiting[1];
    secondDone(PreparedDebugLaunch{[&](ui::DebugSessionRegistry&, ui::DebugGroupId) -> Result<ui::DebugSessionId> {
        ++launches;
        return fail(ErrorCode::IoError, "not reached after trust revocation");
    }});
    EXPECT_EQ(launches, 0);
    EXPECT_FALSE(coordinator.busy());
    EXPECT_FALSE(sessions->hasPendingWork());
}

TEST(CompoundDebugCoordinator, StopAllCancelsPendingPreparationAndLateCompletionCannotLaunch) {
    WorkDir dir("compound-cancel");
    WorkbenchRig rig(dir);
    auto* sessions = rig.workbench->debugSessions();
    ASSERT_NE(sessions, nullptr);
    MultiSessionDebugCoordinator coordinator(*sessions);
    MultiSessionDebugCoordinator::Prepared completed;
    std::stop_token stop;
    run::RunConfiguration member;
    member.id = "one";
    member.name = "One";
    member.program = dir.join("App.exe");
    auto group = coordinator.start(
        {member}, [] { return true; },
        [&](const run::RunConfiguration&, std::stop_token token,
            MultiSessionDebugCoordinator::Prepared done) -> Result<void> {
            stop = token;
            completed = std::move(done);
            return {};
        });
    ASSERT_TRUE(group);
    EXPECT_TRUE(coordinator.busy());
    EXPECT_TRUE(sessions->hasPendingWork());
    sessions->stopAll();
    EXPECT_TRUE(stop.stop_requested());
    EXPECT_FALSE(coordinator.busy());
    int launches = 0;
    completed(PreparedDebugLaunch{[&](ui::DebugSessionRegistry&, ui::DebugGroupId) -> Result<ui::DebugSessionId> {
        ++launches;
        return fail(ErrorCode::IoError, "late launch must not run");
    }});
    EXPECT_EQ(launches, 0);
}

class PhasedDebugTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory = std::make_unique<WorkDir>("phased-debug");
        RigOptions options;
        options.workbench.debugAdapterFactory =
            [&](debug::DapHandlers handlers) -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
            auto peer = std::make_unique<debug::FakeDebugAdapter>();
            debug::scriptProgram(*peer, debug::FakeProgram{.runsForever = true});
            auto client = debug::makeDapClient(peer->transport(), std::move(handlers));
            peers.push_back(std::move(peer));
            return client;
        };
        rig = std::make_unique<WorkbenchRig>(*directory, std::move(options));
        ASSERT_TRUE(rig->trust->trust(directory->path()));
        sessions = rig->workbench->debugSessions();
        coordinator = std::make_unique<MultiSessionDebugCoordinator>(*sessions);
        for (const auto id : {"host-package", "later-member"}) {
            run::RunConfiguration member;
            member.id = id;
            member.name = id;
            member.program = directory->join("Engine.exe");
            members.push_back(std::move(member));
        }
    }
    void TearDown() override {
        coordinator.reset();
        rig.reset();
    }
    Result<ui::DebugSessionId> child(ui::DebugGroupId group, const char* name) {
        return sessions->startTarget(debug::DebugTarget{.name = name,
                                                        .program = directory->join("Engine.exe"),
                                                        .trustRoots = {directory->path()}},
                                     group);
    }
    PreparedDebugLaunch package() {
        PreparedDebugLaunch prepared;
        prepared.startPhased = [this](ui::DebugSessionRegistry&, ui::DebugGroupId group, std::stop_token token,
                                      PreparedDebugLaunch::Started done) -> Result<void> {
            phaseStop = token;
            completion = std::move(done);
            auto started = child(group, "Partial native host");
            if (!started)
                return std::unexpected(started.error());
            partial = *started;
            return {};
        };
        return prepared;
    }
    MultiSessionDebugCoordinator::Prepare prepare(bool holdAll = false) {
        return [this, holdAll](const run::RunConfiguration& member, std::stop_token,
                               MultiSessionDebugCoordinator::Prepared done) -> Result<void> {
            if (holdAll) {
                preparation.push_back(std::move(done));
                return {};
            }
            if (member.id == "host-package")
                done(package());
            else
                done(PreparedDebugLaunch{[this](ui::DebugSessionRegistry&, ui::DebugGroupId group) {
                    ++laterStarts;
                    return child(group, "Later native member");
                }});
            return {};
        };
    }
    std::unique_ptr<WorkDir> directory;
    std::vector<std::unique_ptr<debug::FakeDebugAdapter>> peers;
    std::unique_ptr<WorkbenchRig> rig;
    ui::DebugSessionRegistry* sessions = nullptr;
    std::unique_ptr<MultiSessionDebugCoordinator> coordinator;
    std::vector<run::RunConfiguration> members;
    std::vector<MultiSessionDebugCoordinator::Prepared> preparation;
    PreparedDebugLaunch::Started completion;
    std::stop_token phaseStop;
    ui::DebugSessionId partial = 0;
    int laterStarts = 0;
};
TEST_F(PhasedDebugTest, AllPreparationCompletesBeforeHostStartsAndDependentsHoldLaterMembers) {
    ASSERT_TRUE(coordinator->start(members, [] { return true; }, prepare(true)));
    ASSERT_EQ(preparation.size(), 1U);
    auto first = preparation.front();
    first(package());
    ASSERT_EQ(preparation.size(), 2U);
    EXPECT_EQ(partial, 0U);
    EXPECT_TRUE(peers.empty());
    auto second = preparation.back();
    second(PreparedDebugLaunch{[this](ui::DebugSessionRegistry&, ui::DebugGroupId group) {
        ++laterStarts;
        return child(group, "Later");
    }});
    ASSERT_NE(partial, 0U);
    EXPECT_TRUE(coordinator->busy());
    EXPECT_TRUE(sessions->hasPendingWork());
    EXPECT_EQ(laterStarts, 0);
    completion(std::vector<ui::DebugSessionId>{partial});
    EXPECT_EQ(laterStarts, 1);
    EXPECT_FALSE(coordinator->busy());
    EXPECT_FALSE(sessions->hasPendingWork());
}
TEST_F(PhasedDebugTest, CancelDuringDependentWaitStopsPartialGroupAndIgnoresLateCompletion) {
    auto unrelated = child(0, "Unrelated");
    ASSERT_TRUE(unrelated);
    ASSERT_TRUE(coordinator->start(members, [] { return true; }, prepare()));
    ASSERT_NE(partial, 0U);
    coordinator->cancel();
    EXPECT_TRUE(phaseStop.stop_requested());
    EXPECT_FALSE(coordinator->busy());
    ASSERT_TRUE(test::waitFor([&] { return !sessions->controller(partial)->debugging(); }, 10s));
    completion(std::vector<ui::DebugSessionId>{partial});
    EXPECT_EQ(laterStarts, 0);
    EXPECT_TRUE(sessions->controller(*unrelated)->debugging());
}
TEST_F(PhasedDebugTest, FailedLuaDependencyRollsBackOnlyItsPartialHostGroup) {
    auto unrelated = child(0, "Unrelated");
    ASSERT_TRUE(unrelated);
    ASSERT_TRUE(coordinator->start(members, [] { return true; }, prepare()));
    ASSERT_NE(partial, 0U);
    completion(fail(ErrorCode::IoError, "actual dependent adapter failed to attach"));
    EXPECT_TRUE(phaseStop.stop_requested());
    EXPECT_FALSE(coordinator->busy());
    ASSERT_TRUE(test::waitFor([&] { return !sessions->controller(partial)->debugging(); }, 10s));
    EXPECT_EQ(laterStarts, 0);
    EXPECT_TRUE(sessions->controller(*unrelated)->debugging());
}

void rejectsProjectChanges(bool pendingOnly) {
    WorkDir dir(pendingOnly ? "pending-project-gate" : "secondary-project-gate", "tiny_sln");
    dir.write("host.exe", "unused fixture");
    std::vector<std::unique_ptr<debug::FakeDebugAdapter>> peers;
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.disableProjectEvaluation = true;
    options.launcher = &launcher;
    options.workbench.projectEditHostPath = dir.join("host.exe");
    options.workbench.projectEditStateDirectory = dir.join("edit-lineage");
    options.workbench.debugAdapterFactory =
        [&](debug::DapHandlers handlers) -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
        auto peer = std::make_unique<debug::FakeDebugAdapter>();
        debug::scriptProgram(*peer, debug::FakeProgram{.runsForever = true});
        auto transport = peer->transport();
        peers.push_back(std::move(peer));
        return debug::makeDapClient(std::move(transport), std::move(handlers));
    };
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto* sessions = rig.workbench->debugSessions();
    ASSERT_NE(sessions, nullptr);
    if (pendingOnly) {
        EXPECT_NE(sessions->createGroup(), 0U);
    } else {
        auto child = sessions->startTarget(
            debug::DebugTarget{.name = "Secondary", .program = dir.join("App/App.exe"), .trustRoots = {dir.path()}});
        ASSERT_TRUE(child);
        EXPECT_NE(*child, sessions->primaryId());
    }
    EXPECT_FALSE(rig.workbench->debugController()->debugging());
    EXPECT_TRUE(rig.workbench->anyDebugSessionActive());
    EXPECT_FALSE(rig.workbench->closeProject());
    const auto before = dir.read("App/App.vcxproj");
    auto* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    const auto source = explorer->model()->indexOfPath(dir.join("App/main.cpp"));
    ASSERT_TRUE(source.isValid());
    const auto shown = explorer->filterModel()->mapFromSource(source);
    std::unique_ptr<QMenu> menu(explorer->createContextMenu(shown));
    ASSERT_NE(menu, nullptr);
    const auto actions = menu->actions();
    const auto remove = std::ranges::find_if(
        actions, [](const auto* action) { return action->objectName() == QStringLiteral("removeFromProject"); });
    ASSERT_NE(remove, actions.end());
    (*remove)->trigger();
    QCoreApplication::processEvents();
    EXPECT_EQ(dir.read("App/App.vcxproj"), before);
    EXPECT_FALSE(rig.workbench->buildService().projectEditInProgress());
    EXPECT_TRUE(launcher.launches().empty());
    sessions->stopAll();
    ASSERT_TRUE(test::waitFor([&] { return !sessions->anyDebugging(); }, 10s));
}
TEST(MultiSessionWorkbench, SecondarySessionBlocksProjectCloseAndDefinitionEdits) { rejectsProjectChanges(false); }
TEST(MultiSessionWorkbench, PendingPreparationBlocksProjectCloseAndDefinitionEdits) { rejectsProjectChanges(true); }
} // namespace
} // namespace frost::app
