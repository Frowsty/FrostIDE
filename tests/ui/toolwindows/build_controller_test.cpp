// SPDX-License-Identifier: Apache-2.0
//
// Tests for BuildController (frost/ui/toolwindows/build_controller.hpp): build service callbacks
// made on another thread arrive on the GUI thread, the Save-All hook reaches the shell's handler
// there, a controller that is gone answers late callbacks safely, and request failures are
// reported. Offscreen.
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/save_all_handler.hpp"

#include "toolwindows/toolwindow_test_util.hpp"
#include <QThread>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frost::ui {
namespace {

using build::BuildState;
using test::FakeBuildBackend;
using test::runPostedEvents;

class RecordingSaveAll final : public ISaveAllHandler {
public:
    void saveAll(Completion done) override {
        thread = QThread::currentThread();
        ++calls;
        pending = std::move(done);
    }
    QThread* thread = nullptr;
    int calls = 0;
    Completion pending;
};

TEST(BuildController, DeliversServiceCallbacksOnTheGuiThread) {
    FakeBuildBackend backend;
    BuildController controller;
    controller.setBackend(&backend);
    std::vector<QThread*> threads;
    std::vector<QString> lines;
    QObject::connect(&controller, &BuildController::outputLine, &controller,
                     [&](build::BuildOutputKind, const QString& line) {
                         threads.push_back(QThread::currentThread());
                         lines.push_back(line);
                     });
    test::SignalLog states(&controller, &BuildController::stateChanged);
    test::SignalLog finished(&controller, &BuildController::buildFinished);

    const build::BuildServiceCallbacks callbacks = controller.serviceCallbacks();
    std::thread service([&callbacks] {
        callbacks.onStateChanged(BuildState::Running);
        for (int i = 0; i < 100; ++i) {
            callbacks.onOutput(build::BuildOutputKind::Stdout, "line " + std::to_string(i));
        }
        build::BuildOutcome outcome;
        outcome.result.success = true;
        callbacks.onFinished(outcome);
        callbacks.onStateChanged(BuildState::Idle);
    });
    service.join();
    EXPECT_TRUE(lines.empty());
    runPostedEvents();

    ASSERT_EQ(lines.size(), 100U);
    EXPECT_EQ(lines.front(), QStringLiteral("line 0"));
    EXPECT_EQ(lines.back(), QStringLiteral("line 99")); // In order.
    for (QThread* thread : threads) {
        EXPECT_EQ(thread, QThread::currentThread());
    }
    EXPECT_EQ(states.count(), 2);
    ASSERT_EQ(finished.count(), 1);
    EXPECT_EQ(controller.state(), BuildState::Idle);
    ASSERT_TRUE(controller.lastOutcome().has_value());
    EXPECT_TRUE(controller.lastOutcome()->succeeded());
    EXPECT_EQ(controller.currentOutput().size(), 100U); // Kept for tool windows opened later.
}

TEST(BuildController, SupersededFencedOutputIsDroppedAfterGuiQueueing) {
    FakeBuildBackend backend;
    BuildController controller;
    controller.setBackend(&backend);
    std::vector<QString> lines;
    QObject::connect(&controller, &BuildController::outputLine, &controller,
                     [&](build::BuildOutputKind, const QString& line) { lines.push_back(line); });
    const auto callbacks = controller.serviceCallbacks();
    std::atomic<bool> current{true};
    callbacks.onOutputFenced(build::BuildOutputKind::Stdout, "old", [&] { return current.load(); });
    test::SignalLog finished(&controller, &BuildController::buildFinished);
    callbacks.onFinishedFenced(build::BuildOutcome{}, [&] { return current.load(); });
    current.store(false);
    callbacks.onOutputFenced(build::BuildOutputKind::Stdout, "new", [] { return true; });
    runPostedEvents();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines.front(), QStringLiteral("new"));
    EXPECT_EQ(finished.count(), 0);
}

TEST(BuildController, RunsTheSaveAllHookOnTheGuiThread) {
    FakeBuildBackend backend;
    BuildController controller;
    RecordingSaveAll handler;
    controller.setSaveAllHandler(&handler);
    controller.setBackend(&backend);
    ASSERT_TRUE(static_cast<bool>(backend.hook));

    std::atomic<int> completions = 0;
    std::optional<bool> saved;
    std::thread service([&] {
        backend.hook([&](Result<void> result) {
            saved = result.has_value();
            ++completions;
        });
    });
    service.join();
    EXPECT_EQ(handler.calls, 0);
    runPostedEvents();
    ASSERT_EQ(handler.calls, 1);
    EXPECT_EQ(handler.thread, QThread::currentThread());
    EXPECT_EQ(completions.load(), 0); // The shell finishes saving when it is done.
    handler.pending({});
    EXPECT_EQ(completions.load(), 1);
    EXPECT_EQ(saved, std::optional<bool>(true));

    // Without a handler there is nothing to save.
    controller.setSaveAllHandler(nullptr);
    saved.reset();
    backend.hook([&](Result<void> result) { saved = result.has_value(); });
    runPostedEvents();
    EXPECT_EQ(saved, std::optional<bool>(true));
}

TEST(BuildController, LateCallbacksAfterTheControllerIsGoneAreSafe) {
    FakeBuildBackend backend;
    build::BuildServiceCallbacks callbacks;
    {
        BuildController controller;
        controller.setBackend(&backend);
        callbacks = controller.serviceCallbacks();
        callbacks.onOutput(build::BuildOutputKind::Stdout, "posted before the controller goes");
    }
    runPostedEvents(); // The posted call finds no controller.
    callbacks.onOutput(build::BuildOutputKind::Stdout, "after");
    callbacks.onStateChanged(BuildState::Idle);
    runPostedEvents();
    // The Save-All hook the service still holds answers Cancelled instead of hanging the build.
    std::optional<ErrorCode> code;
    backend.hook([&](Result<void> result) { code = result ? std::nullopt : std::optional(result.error().code()); });
    EXPECT_EQ(code, std::optional(ErrorCode::Cancelled));
}

TEST(BuildController, DropsQueuedEventsFromAnInactiveBackend) {
    BuildController controller;
    bool active = true;
    const auto callbacks = controller.serviceCallbacks([&active] { return active; });
    callbacks.onStateChanged(BuildState::Running);
    callbacks.onOutput(build::BuildOutputKind::Stdout, "old project");
    active = false;
    runPostedEvents();
    EXPECT_EQ(controller.state(), BuildState::Idle);
    EXPECT_TRUE(controller.currentOutput().empty());
}

TEST(BuildController, ReportsRefusedRequestsAndTracksTheSolution) {
    FakeBuildBackend backend;
    BuildController controller;
    test::SignalLog failures(&controller, &BuildController::requestFailed);
    EXPECT_FALSE(controller.build(build::BuildAction::Build).has_value()); // No backend.
    controller.setBackend(&backend);
    test::SignalLog solutions(&controller, &BuildController::solutionChanged);
    ASSERT_TRUE(controller.openSolution(test::loadSdk()).has_value());
    EXPECT_EQ(solutions.count(), 1);
    EXPECT_TRUE(controller.hasSolution());
    EXPECT_TRUE(controller.canBuild());
    EXPECT_FALSE(controller.configurations().empty());
    EXPECT_EQ(configurationLabel(project::SolutionConfiguration{"Debug", "x64"}), QStringLiteral("Debug | x64"));

    const int before = static_cast<int>(failures.count());
    backend.buildError = Error(ErrorCode::NotFound, "MSBuild.exe was not found");
    EXPECT_FALSE(controller.build(build::BuildAction::Build).has_value());
    ASSERT_EQ(failures.count(), before + 1);
    EXPECT_TRUE(failures.last().at(0).toString().contains(QStringLiteral("MSBuild.exe")));

    ASSERT_TRUE(controller.closeSolution().has_value());
    EXPECT_EQ(solutions.count(), 2);
    EXPECT_FALSE(controller.canBuild());
}

} // namespace
} // namespace frost::ui
