// SPDX-License-Identifier: Apache-2.0
//
// End-to-end tests of frost::ui_vcs against real git on temporary repositories (GitUiE2E.*,
// offscreen): the Commit window committing the checked files only, keeping its selection after a
// failing pre-commit hook, Amend and the message history; the diff from the Commit window and
// Compare with HEAD; the editor's change markers against the real HEAD version; the branch widget
// with a refused checkout, a checkout and a new branch; and the VCS actions with the git console
// pushing to a local bare repository. Every git process goes through GitRunner, which ends its
// whole tree before returning; each controller's destructor joins its workers.

#include "frost/input/action.hpp"
#include "frost/ui/editor/display_list.hpp"
#include "frost/ui/vcs/branch_widget.hpp"
#include "frost/ui/vcs/change_markers.hpp"
#include "frost/ui/vcs/commit_window.hpp"
#include "frost/ui/vcs/diff_viewer.hpp"
#include "frost/ui/vcs/git_actions.hpp"
#include "frost/ui/vcs/git_console.hpp"
#include "frost/ui/vcs/git_controller.hpp"

#include "editor/editor_harness.hpp"
#include "vcs/git_fixture.hpp"
#include <QCheckBox>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTreeView>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace frost::ui {
namespace {

using vcs::test::TempRepo;
using namespace std::chrono_literals;

constexpr auto kWait = 60s;

std::string trimmed(const vcs::GitOutput& output) { return std::string(vcs::trimGitText(output.stdoutBytes)); }

std::unique_ptr<GitController> controllerFor(const TempRepo& repo) {
    // A short debounce: the tests ask for refreshes explicitly.
    vcs::GitServiceOptions options;
    options.debounce = 20ms;
    auto controller = std::make_unique<GitController>(repo.runner(), repo.fs(), repo.info(), options);
    EXPECT_TRUE(controller->waitForIdle(kWait));
    EXPECT_NE(controller->snapshot(), nullptr);
    return controller;
}

TEST(GitUiE2E, RejectsSecondCommitUntilFirstCompletionIsDelivered) {
    FROST_REQUIRE_OFFSCREEN();
    TempRepo repo("ui-held-completion");
    repo.init();
    repo.write("a.txt", "a\n");
    repo.write("b.txt", "b\n");
    repo.commitAll("initial");
    repo.write("a.txt", "changed a\n");
    repo.write("b.txt", "changed b\n");
    auto controller = controllerFor(repo);
    controller->commit({repo.path("a.txt")}, QStringLiteral("first"), false, false);
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (controller->service().operationBusy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms); // Deliberately hold GUI completion.
    ASSERT_FALSE(controller->service().operationBusy());
    ASSERT_TRUE(controller->busy());
    controller->commit({repo.path("b.txt")}, QStringLiteral("second"), false, true);
    ASSERT_TRUE(controller->waitForIdle(kWait));
    ASSERT_TRUE(controller->lastResult().has_value());
    EXPECT_TRUE(controller->lastResult()->succeeded());
    EXPECT_EQ(controller->messageHistory().front(), QStringLiteral("first"));
    EXPECT_EQ(trimmed(repo.git({"log", "-1", "--format=%s"})), "first");
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:b.txt"})), "b");
    EXPECT_EQ(trimmed(repo.git({"rev-list", "--count", "HEAD"})), "2");
}

TEST(GitUiE2E, CommitAndPushRunsAfterTheCommitCompletion) {
    FROST_REQUIRE_OFFSCREEN();
    TempRepo repo("ui-commit-push");
    const std::string bare = TempRepo::join(repo.root(), "origin.git");
    repo.git({"init", "-q", "--bare", bare}, 0, repo.root());
    repo.init();
    repo.git({"remote", "add", "origin", bare});
    repo.write("a.txt", "base\n");
    repo.commitAll("initial");
    repo.write("a.txt", "changed\n");
    auto controller = controllerFor(repo);
    controller->commit({repo.path("a.txt")}, QStringLiteral("commit then push"), false, true);
    ASSERT_TRUE(controller->waitForIdle(kWait));
    ASSERT_TRUE(controller->waitForIdle(kWait));
    ASSERT_TRUE(controller->lastResult().has_value());
    EXPECT_EQ(controller->lastResult()->kind, vcs::GitOperationKind::Push);
    EXPECT_TRUE(controller->lastResult()->succeeded());
    EXPECT_EQ(trimmed(repo.git({"rev-parse", "main"}, 0, bare)), trimmed(repo.git({"rev-parse", "HEAD"})));
}

TEST(GitUiE2E, TheCommitWindowCommitsTheCheckedFilesAndKeepsItsSelectionWhenAHookFails) {
    FROST_REQUIRE_OFFSCREEN();
    TempRepo repo("ui-commit");
    repo.init();
    repo.write("a.txt", "a\n");
    repo.write("b.txt", "b\n");
    repo.commitAll("initial");
    repo.write("a.txt", "a2\n");
    repo.write("b.txt", "b2\n");
    repo.write("new.txt", "new\n");
    auto controller = controllerFor(repo);
    CommitToolWindow window(*controller);
    window.resize(500, 600);
    window.show();
    ASSERT_NE(window.model().changesGroup(), nullptr);
    ASSERT_NE(window.model().unversionedGroup(), nullptr);
    EXPECT_EQ(window.model().checkedPaths(), (std::vector<std::string>{repo.path("a.txt"), repo.path("b.txt")}));

    // Commit a.txt and new.txt, not b.txt; the pre-commit hook refuses first.
    window.model().setChecked(repo.path("b.txt"), false);
    window.model().setChecked(repo.path("new.txt"), true);
    window.setMessage(QStringLiteral("Add new.txt"));
    repo.hook("pre-commit", "echo 'lint: no tabs allowed' >&2\nexit 1");
    const std::string before = trimmed(repo.git({"rev-parse", "HEAD"}));
    ASSERT_TRUE(window.commit());
    EXPECT_FALSE(window.commitButton()->isEnabled()); // While it runs.
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_TRUE(window.resultLabel()->text().startsWith(QStringLiteral("Commit failed")))
        << window.resultLabel()->text().toStdString();
    EXPECT_TRUE(window.resultLabel()->text().contains(QStringLiteral("lint: no tabs allowed")));
    EXPECT_EQ(trimmed(repo.git({"rev-parse", "HEAD"})), before);
    EXPECT_EQ(window.message(), QStringLiteral("Add new.txt"));
    EXPECT_EQ(window.model().checkedPaths(), (std::vector<std::string>{repo.path("a.txt"), repo.path("new.txt")}));
    EXPECT_TRUE(window.commitButton()->isEnabled());

    // Without the hook the same selection commits.
    repo.hook("pre-commit", "exit 0");
    ASSERT_TRUE(window.commit());
    ASSERT_TRUE(controller->waitForIdle(kWait));
    const std::string head = trimmed(repo.git({"rev-parse", "HEAD"}));
    EXPECT_NE(head, before);
    EXPECT_EQ(window.resultLabel()->text(),
              QStringLiteral("Committed %1.").arg(QString::fromStdString(head.substr(0, 8))));
    EXPECT_TRUE(window.message().isEmpty());
    EXPECT_EQ(trimmed(repo.git({"show", "--name-only", "--format=", "HEAD"})), "a.txt\nnew.txt");
    // The window follows the refresh after the commit: only b.txt is left.
    EXPECT_EQ(window.model().allPaths(), std::vector<std::string>{repo.path("b.txt")});
    EXPECT_FALSE(window.model().isChecked(repo.path("b.txt")));
    EXPECT_EQ(window.model().unversionedGroup(), nullptr);
    ASSERT_FALSE(window.messageHistory().isEmpty());
    EXPECT_EQ(window.messageHistory().front(), QStringLiteral("Add new.txt"));

    // Amend loads the previous message into the empty editor, and unchecking restores it.
    window.amendCheck()->setChecked(true);
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_EQ(window.message().trimmed(), QStringLiteral("Add new.txt"));
    window.amendCheck()->setChecked(false);
    EXPECT_TRUE(window.message().isEmpty());
    // Amend with a new message rewords HEAD and keeps its files.
    window.setMessage(QStringLiteral("Add new.txt and change a.txt"));
    window.amendCheck()->setChecked(true);
    window.model().setChecked(repo.path("b.txt"), false);
    ASSERT_TRUE(window.commit());
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_EQ(trimmed(repo.git({"log", "-1", "--format=%s"})), "Add new.txt and change a.txt");
    EXPECT_EQ(trimmed(repo.git({"rev-list", "--count", "HEAD"})), "2");
    EXPECT_FALSE(window.amendCheck()->isChecked());
    window.useHistoryMessage(0);
    EXPECT_EQ(window.message(), QStringLiteral("Add new.txt and change a.txt"));
}

TEST(GitUiE2E, RollbackAsksFirstAndRestoresTheCheckedFiles) {
    FROST_REQUIRE_OFFSCREEN();
    TempRepo repo("ui-rollback");
    repo.init();
    repo.write("a.txt", "a\n");
    repo.commitAll("initial");
    repo.write("a.txt", "changed\n");
    auto controller = controllerFor(repo);
    CommitToolWindow window(*controller);
    std::vector<QString> asked;
    bool answer = false;
    window.setConfirmHandler([&](const QString&, const QString& question) {
        asked.push_back(question);
        return answer;
    });
    EXPECT_FALSE(window.rollbackSelected()); // Declined.
    ASSERT_EQ(asked.size(), 1U);
    EXPECT_TRUE(asked[0].contains(QStringLiteral("a.txt")));
    EXPECT_EQ(repo.read("a.txt"), "changed\n");
    answer = true;
    EXPECT_TRUE(window.rollbackSelected());
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_EQ(repo.read("a.txt"), "a\n");
    EXPECT_EQ(window.resultLabel()->text(), QStringLiteral("Rolled back."));
    EXPECT_EQ(window.model().rowCount(), 0);
}

TEST(GitUiE2E, TheDiffOpensFromTheCommitWindowAndCompareWithHead) {
    FROST_REQUIRE_OFFSCREEN();
    TempRepo repo("ui-diff");
    repo.init();
    repo.write("code.cpp", "int a = 1;\nint b = 2;\nint c = 3;\n");
    repo.commitAll("initial");
    repo.write("code.cpp", "int a = 1;\nint b = 20;\nint c = 3;\nint d = 4;\n");
    auto controller = controllerFor(repo);
    CommitToolWindow window(*controller);
    window.show();

    // Double click (or Ctrl+D) on the file asks for its diff.
    std::vector<QString> requested;
    QObject::connect(&window, &CommitToolWindow::diffRequested,
                     [&requested](const QString& path) { requested.push_back(path); });
    QStandardItem* item = window.model().itemFor(repo.path("code.cpp"));
    ASSERT_NE(item, nullptr);
    window.tree()->setCurrentIndex(item->index());
    EXPECT_EQ(window.currentPath(), repo.path("code.cpp"));
    QKeyEvent ctrlD(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier);
    QCoreApplication::sendEvent(&window, &ctrlD);
    emit window.tree()->doubleClicked(item->index());
    ASSERT_EQ(requested.size(), 2U);
    EXPECT_EQ(requested[0].toStdString(), repo.path("code.cpp"));

    // Compare with HEAD shows HEAD against the working copy.
    DiffViewer viewer;
    viewer.resize(800, 400);
    viewer.show();
    std::optional<Result<void>> outcome;
    showDiffAgainst(viewer, *controller, DiffBase::Head, requested[0].toStdString(), repo.read("code.cpp"),
                    [&outcome](Result<void> result) { outcome = std::move(result); });
    ASSERT_TRUE(controller->waitForIdle(kWait));
    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->has_value()) << outcome->error().describe();
    EXPECT_EQ(viewer.leftTitle(), QStringLiteral("HEAD"));
    EXPECT_EQ(viewer.leftPane()->toPlainText(), QStringLiteral("int a = 1;\nint b = 2;\nint c = 3;"));
    EXPECT_EQ(viewer.rightPane()->toPlainText(), QStringLiteral("int a = 1;\nint b = 20;\nint c = 3;\nint d = 4;"));
    ASSERT_EQ(viewer.diff().blocks().size(), 2U);
    EXPECT_EQ(viewer.currentBlock(), std::optional<std::size_t>(0));
    EXPECT_EQ(viewer.rightPane()->textCursor().blockNumber(), 1);
    EXPECT_TRUE(viewer.nextChange());
    EXPECT_EQ(viewer.rightPane()->textCursor().blockNumber(), 3);

    // The index version of an unversioned file is missing: an empty left side.
    repo.write("fresh.txt", "fresh\n");
    outcome.reset();
    showDiffAgainst(viewer, *controller, DiffBase::Index, repo.path("fresh.txt"), "fresh\n",
                    [&outcome](Result<void> result) { outcome = std::move(result); });
    ASSERT_TRUE(controller->waitForIdle(kWait));
    ASSERT_TRUE(outcome.has_value() && outcome->has_value());
    EXPECT_TRUE(viewer.leftTitle().startsWith(QStringLiteral("Index")));
    EXPECT_TRUE(viewer.leftPane()->toPlainText().isEmpty());
    ASSERT_EQ(viewer.diff().blocks().size(), 1U);
    EXPECT_EQ(viewer.diff().blocks()[0].kind(), vcs::LineChangeKind::Added);
}

TEST(GitUiE2E, ChangeMarkersFollowTheHeadVersion) {
    FROST_REQUIRE_OFFSCREEN();
    TempRepo repo("ui-markers");
    repo.init();
    repo.write("notes.txt", "one\ntwo\nthree\n");
    repo.commitAll("initial");
    repo.write("notes.txt", "one\nTWO\nthree\nfour\n");
    auto controller = controllerFor(repo);
    test::HarnessOptions options;
    options.path = repo.path("notes.txt");
    test::EditorHarness h(repo.read("notes.txt"), options);
    GitController* git = controller.get();
    EditorChangeMarkers markers(*h.view, [git](const std::string& path, GitController::ContentCallback done) {
        git->readHeadContent(path, std::move(done));
    });
    ASSERT_TRUE(controller->waitForIdle(kWait));
    ASSERT_TRUE(markers.hasBase());
    ASSERT_EQ(markers.changes().size(), 2U);
    EXPECT_EQ(markers.changes()[0].kind, vcs::LineChangeKind::Modified);
    EXPECT_EQ(markers.changes()[1].kind, vcs::LineChangeKind::Added);
    EXPECT_EQ(h.view->buildDisplayList().changeStripes.size(), 2U);
    EXPECT_EQ(markers.baseText(0), "two\n");

    // After a commit of the file the base is the new HEAD: no markers are left.
    QObject::connect(controller.get(), &GitController::statusChanged, &markers, &EditorChangeMarkers::refreshBase);
    controller->commit({repo.path("notes.txt")}, QStringLiteral("Edit notes"), false);
    ASSERT_TRUE(controller->waitForIdle(kWait));
    ASSERT_TRUE(controller->waitForIdle(kWait)); // The base read the status change queued.
    EXPECT_TRUE(markers.changes().empty());
    EXPECT_TRUE(h.view->lineChanges().empty());

    // A file with no HEAD version has no base.
    repo.write("other.txt", "x\n");
    test::HarnessOptions otherOptions;
    otherOptions.path = repo.path("other.txt");
    test::EditorHarness other("x\n", otherOptions);
    EditorChangeMarkers none(*other.view, [git](const std::string& path, GitController::ContentCallback done) {
        git->readHeadContent(path, std::move(done));
    });
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_FALSE(none.hasBase());
}

TEST(GitUiE2E, TheBranchWidgetChecksOutCreatesAndReportsARefusedCheckout) {
    FROST_REQUIRE_OFFSCREEN();
    TempRepo repo("ui-branches");
    repo.init();
    repo.write("a.txt", "main\n");
    repo.commitAll("initial");
    repo.git({"switch", "-q", "-c", "feature"});
    repo.write("a.txt", "feature\n");
    repo.commitAll("on feature");
    repo.git({"switch", "-q", "main"});
    repo.write("a.txt", "local edit\n"); // Blocks the checkout of feature.
    auto controller = controllerFor(repo);
    QWidget window;
    window.resize(400, 200);
    auto* widget = new BranchStatusWidget(*controller, &window);
    window.show();
    EXPECT_EQ(widget->text(), QStringLiteral("main"));

    ListPopup* popup = widget->openPopup();
    ASSERT_NE(popup, nullptr);
    ASSERT_TRUE(controller->waitForIdle(kWait));
    const std::vector<ListPopupItem> items = branchPopupItems(*controller);
    ASSERT_EQ(items.size(), 3U);
    EXPECT_EQ(items[0].data.toString(), QStringLiteral("new:"));
    EXPECT_EQ(items[1].text, QStringLiteral("feature"));
    EXPECT_EQ(items[2].text, QStringLiteral("main"));
    EXPECT_EQ(items[2].detail, QStringLiteral("current"));
    popup->closePopup(Popup::CloseReason::Dismissed);

    // The local edit blocks the checkout: the refusal names the file and nothing moves.
    widget->runItem(QStringLiteral("local:feature"));
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_TRUE(widget->lastError().contains(QStringLiteral("refused"))) << widget->lastError().toStdString();
    EXPECT_TRUE(widget->lastError().contains(QStringLiteral("a.txt")));
    EXPECT_TRUE(widget->toolTip().contains(widget->lastError()));
    EXPECT_EQ(controller->currentBranch(), QStringLiteral("main"));
    EXPECT_EQ(repo.read("a.txt"), "local edit\n");

    // Without the edit the checkout goes through.
    repo.git({"checkout", "--", "a.txt"});
    widget->runItem(QStringLiteral("local:feature"));
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_TRUE(widget->lastError().isEmpty());
    EXPECT_EQ(controller->currentBranch(), QStringLiteral("feature"));
    EXPECT_EQ(widget->text(), QStringLiteral("feature"));
    EXPECT_EQ(repo.read("a.txt"), "feature\n");

    // New Branch... asks for the name and switches to it.
    widget->runItem(QStringLiteral("new:"));
    ASSERT_NE(widget->newBranchPrompt(), nullptr);
    emit widget->newBranchPrompt()->submitted(QStringLiteral("topic"));
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_EQ(controller->currentBranch(), QStringLiteral("topic"));
    EXPECT_EQ(widget->text(), QStringLiteral("topic"));
    widget->newBranchPrompt()->closePopup(Popup::CloseReason::Accepted);
}

TEST(GitUiE2E, TheActionsPushThroughTheControllerAndTheConsoleShowsTheCommands) {
    FROST_REQUIRE_OFFSCREEN();
    TempRepo repo("ui-actions");
    const std::string bare = TempRepo::join(repo.root(), "origin.git");
    repo.git({"init", "-q", "--bare", bare}, 0, repo.root());
    repo.init();
    repo.git({"remote", "add", "origin", bare});
    repo.write("a.txt", "one\n");
    repo.commitAll("first");
    auto controller = controllerFor(repo);
    GitConsoleView console(*controller);

    input::ActionRegistry registry;
    int commitWindows = 0;
    std::vector<QString> confirmations;
    GitActionTargets targets;
    targets.openCommitWindow = [&commitWindows] { ++commitWindows; };
    targets.currentFile = [&repo] { return repo.path("a.txt"); };
    targets.confirm = [&confirmations](const QString& title, const QString&) {
        confirmations.push_back(title);
        return false;
    };
    ASSERT_TRUE(registerGitActions(registry, *controller, targets).has_value());
    EXPECT_FALSE(registerGitActions(registry, *controller, {}).has_value()); // Ids are taken.
    for (const std::string_view id : {git_actions::kCommit, git_actions::kPush, git_actions::kUpdateProject,
                                      git_actions::kRollback, git_actions::kShowDiff, git_actions::kNextChange}) {
        EXPECT_TRUE(registry.find(id).has_value()) << id;
    }

    input::ActionContext trusted;
    trusted.trusted = true;
    input::ActionContext untrusted;
    const input::ActionId push = *registry.find(git_actions::kPush);
    EXPECT_EQ(registry.whyDisabled(push, untrusted), input::DisabledReason::Untrusted);
    // Show Diff only applies in the Commit window, Next Change only in a diff viewer.
    EXPECT_EQ(registry.whyDisabled(*registry.find(git_actions::kShowDiff), trusted),
              input::DisabledReason::NotApplicable);
    input::ActionContext inDiff = trusted;
    inDiff.keys.set(input::ContextKey::DiffViewFocus);
    EXPECT_EQ(registry.whyDisabled(*registry.find(git_actions::kNextChange), inDiff), input::DisabledReason::Callback);
    EXPECT_EQ(registry.info(*registry.find(git_actions::kNextChange)).priority, 1);

    EXPECT_EQ(registry.tryInvoke(*registry.find(git_actions::kCommit), trusted), input::InvokeResult::Ran);
    EXPECT_EQ(commitWindows, 1);
    // Rollback asks first; declined, nothing runs.
    repo.write("a.txt", "edited\n");
    EXPECT_EQ(registry.tryInvoke(*registry.find(git_actions::kRollback), trusted), input::InvokeResult::Ran);
    EXPECT_EQ(confirmations, std::vector<QString>{QStringLiteral("Rollback Changes")});
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_EQ(repo.read("a.txt"), "edited\n");

    // Push: the first one sets the upstream.
    std::vector<std::pair<int, bool>> finished;
    QObject::connect(controller.get(), &GitController::operationFinished,
                     [&finished](int kind, bool ok, const QString&) { finished.emplace_back(kind, ok); });
    EXPECT_EQ(registry.tryInvoke(push, trusted), input::InvokeResult::Ran);
    ASSERT_TRUE(controller->waitForIdle(kWait));
    ASSERT_EQ(finished.size(), 1U);
    EXPECT_EQ(finished[0], std::make_pair(static_cast<int>(vcs::GitOperationKind::Push), true));
    EXPECT_EQ(trimmed(repo.git({"rev-parse", "main"}, 0, bare)), trimmed(repo.git({"rev-parse", "HEAD"})));
    EXPECT_TRUE(console.toPlainText().contains(QStringLiteral("> git push --set-upstream origin main")))
        << console.toPlainText().toStdString();

    // Update Project pulls (nothing new) and Fetch fetches; both land in the console.
    EXPECT_EQ(registry.tryInvoke(*registry.find(git_actions::kUpdateProject), trusted), input::InvokeResult::Ran);
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_EQ(registry.tryInvoke(*registry.find(git_actions::kFetch), trusted), input::InvokeResult::Ran);
    ASSERT_TRUE(controller->waitForIdle(kWait));
    ASSERT_EQ(finished.size(), 3U);
    EXPECT_TRUE(finished[1].second);
    EXPECT_TRUE(finished[2].second);
    EXPECT_TRUE(console.toPlainText().contains(QStringLiteral("> git pull --no-edit --no-rebase")));
    EXPECT_TRUE(console.toPlainText().contains(QStringLiteral("> git fetch --all --prune")));

    // A force push without the confirmation does not run.
    EXPECT_EQ(registry.tryInvoke(*registry.find(git_actions::kForcePush), trusted), input::InvokeResult::Ran);
    ASSERT_TRUE(controller->waitForIdle(kWait));
    EXPECT_EQ(finished.size(), 3U);
    EXPECT_EQ(confirmations.back(), QStringLiteral("Force Push"));
}

} // namespace
} // namespace frost::ui
