// SPDX-License-Identifier: Apache-2.0
// Actual Workbench Git actions on isolated throwaway repositories.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/vcs/branch_widget.hpp"
#include "frost/ui/vcs/change_markers.hpp"
#include "frost/ui/vcs/commit_window.hpp"
#include "frost/ui/vcs/diff_viewer.hpp"
#include "frost/ui/vcs/git_actions.hpp"
#include "frost/ui/vcs/git_controller.hpp"
#include "frost/ui/vcs/vcs_colors.hpp"

#include "vcs/git_fixture.hpp"
#include "workbench_rig.hpp"
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace {
using namespace std::chrono_literals;
using namespace frost;
using app::test::waitFor;
using app::test::WorkbenchRig;
using app::test::WorkDir;
using vcs::test::TempRepo;

// Isolate the child Git processes from the user's config, including hook/signing settings.
class GitConfig final {
public:
    explicit GitConfig(const TempRepo& repo)
        : m_global(qgetenv("GIT_CONFIG_GLOBAL")), m_system(qgetenv("GIT_CONFIG_NOSYSTEM")) {
        qputenv("GIT_CONFIG_GLOBAL", QByteArray::fromStdString(TempRepo::join(repo.root(), "gitconfig")));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }
    ~GitConfig() {
        if (m_global.isNull())
            qunsetenv("GIT_CONFIG_GLOBAL");
        else
            qputenv("GIT_CONFIG_GLOBAL", m_global);
        if (m_system.isNull())
            qunsetenv("GIT_CONFIG_NOSYSTEM");
        else
            qputenv("GIT_CONFIG_NOSYSTEM", m_system);
    }

private:
    QByteArray m_global;
    QByteArray m_system;
};

ui::GitController* controller(WorkbenchRig& rig) { return rig.workbench->findChild<ui::GitController*>(); }
ui::CommitToolWindow* commitWindow(WorkbenchRig& rig) {
    return qobject_cast<ui::CommitToolWindow*>(rig.window->toolWindows().content("commit"));
}

TEST(WorkbenchGit, NestedProjectShowsDirtyBufferDiffAndCommitsSelectedFile) {
    TempRepo repo("app-git-dirty");
    GitConfig config(repo);
    repo.init();
    repo.write("src/a.cpp", "int original;\n");
    repo.write("src/b.cpp", "int second;\n");
    repo.commitAll("initial");
    repo.write("src/b.cpp", "int changed;\n");
    WorkDir dir("git-workbench");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.path("src")));
    ASSERT_TRUE(rig.workbench->openPath(repo.path("src/a.cpp")));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    EXPECT_EQ(controller(rig)->info().workTree, repo.work());
    ASSERT_TRUE(rig.view()->editor().typeText("// unsaved\n"));
    const auto dirtyColor = ui::vcsStatusColor(vcs::VcsFileStatus::Modified);
    ASSERT_TRUE(dirtyColor.has_value());
    EXPECT_EQ(rig.window->tabs().titleColor(0), *dirtyColor);
    ASSERT_TRUE(waitFor([&] { return !rig.view()->editor().lineChanges().empty(); }, 10s));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCompareWithHead), input::InvokeResult::Ran);
    auto* diff = qobject_cast<ui::DiffViewer*>(rig.window->toolWindows().content("gitDiff"));
    ASSERT_NE(diff, nullptr);
    ASSERT_TRUE(waitFor([&] { return diff->rightPane()->toPlainText().contains(QStringLiteral("unsaved")); }, 10s));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCommit), input::InvokeResult::Ran);
    auto* commit = commitWindow(rig);
    ASSERT_NE(commit, nullptr);
    ASSERT_TRUE(waitFor([&] { return commit->model().itemFor(repo.path("src/a.cpp")) != nullptr; }, 5s));
    commit->model().setChecked(repo.path("src/b.cpp"), false);
    commit->model().setChecked(repo.path("src/a.cpp"), true);
    commit->setMessage(QStringLiteral("save selected"));
    ASSERT_TRUE(commit->commit());
    ASSERT_TRUE(waitFor([&] { return controller(rig)->lastResult().has_value(); }, 20s));
    ASSERT_TRUE(controller(rig)->lastResult()->succeeded());
    EXPECT_EQ(repo.read("src/a.cpp"), "// unsaved\nint original;\n");
    EXPECT_EQ(repo.git({"show", "HEAD:src/a.cpp"}).stdoutBytes, "// unsaved\nint original;\n");
    EXPECT_EQ(repo.git({"show", "HEAD:src/b.cpp"}).stdoutBytes, "int second;\n");
    EXPECT_EQ(repo.read("src/b.cpp"), "int changed;\n");
}

TEST(WorkbenchGit, IndexDiffComparesStagedContentWithUnsavedEditor) {
    TempRepo repo("app-git-index");
    GitConfig config(repo);
    repo.init();
    repo.write("a.txt", "base\n");
    repo.commitAll("initial");
    repo.write("a.txt", "staged\n");
    repo.git({"add", "a.txt"});
    WorkDir dir("git-workbench-index");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.path("a.txt")));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    ASSERT_TRUE(rig.view()->editor().typeText("unsaved "));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCompareWithIndex), input::InvokeResult::Ran);
    auto* diff = qobject_cast<ui::DiffViewer*>(rig.window->toolWindows().content("gitDiff"));
    ASSERT_NE(diff, nullptr);
    ASSERT_TRUE(waitFor([&] { return diff->leftTitle() == QStringLiteral("Index"); }, 10s));
    EXPECT_TRUE(diff->leftPane()->toPlainText().contains(QStringLiteral("staged")));
    EXPECT_TRUE(diff->rightPane()->toPlainText().contains(QStringLiteral("unsaved staged")));
    ASSERT_TRUE(rig.view()->editor().typeText("later "));
    EXPECT_TRUE(waitFor(
        [&] { return diff->rightPane()->toPlainText().contains(QStringLiteral("unsaved later staged")); }, 10s));
}

TEST(WorkbenchGit, HookFailureKeepsSelectionMessageAndOtherStagedFiles) {
    TempRepo repo("app-git-hook");
    GitConfig config(repo);
    repo.init();
    repo.write("a.txt", "a\n");
    repo.write("b.txt", "b\n");
    repo.commitAll("initial");
    repo.write("a.txt", "changed a\n");
    repo.write("b.txt", "changed b\n");
    repo.git({"add", "b.txt"});
    repo.hook("pre-commit", "echo 'fixture hook refused' >&2\nexit 1");
    const auto head = repo.git({"rev-parse", "HEAD"}).stdoutBytes;
    WorkDir dir("git-workbench-hook");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.work()));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCommit), input::InvokeResult::Ran);
    auto* commit = commitWindow(rig);
    ASSERT_NE(commit, nullptr);
    ASSERT_NE(commit->model().itemFor(repo.path("a.txt")), nullptr);
    commit->model().setChecked(repo.path("b.txt"), false);
    commit->model().setChecked(repo.path("a.txt"), true);
    commit->setMessage(QStringLiteral("keep this message"));
    ASSERT_TRUE(commit->commit());
    ASSERT_TRUE(waitFor([&] { return controller(rig)->lastResult().has_value(); }, 20s));
    EXPECT_FALSE(controller(rig)->lastResult()->succeeded());
    EXPECT_TRUE(commit->resultLabel()->text().contains(QStringLiteral("fixture hook refused")));
    EXPECT_EQ(commit->message(), QStringLiteral("keep this message"));
    EXPECT_TRUE(commit->model().isChecked(repo.path("a.txt")));
    EXPECT_FALSE(commit->model().isChecked(repo.path("b.txt")));
    EXPECT_EQ(repo.git({"rev-parse", "HEAD"}).stdoutBytes, head);
    EXPECT_EQ(repo.git({"show", ":b.txt"}).stdoutBytes, "changed b\n");
}

TEST(WorkbenchGit, ExternalEditsRefreshAndSwitchingRepositoriesDropsOldController) {
    TempRepo first("app-git-switch-a");
    GitConfig config(first);
    first.init();
    first.write("a.txt", "a\n");
    first.commitAll("initial");
    TempRepo second("app-git-switch-b");
    second.init();
    second.write("b.txt", "b\n");
    second.commitAll("initial");
    WorkDir dir("git-workbench-switch");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(first.work()));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    first.write("a.txt", "external change\n");
    ASSERT_TRUE(waitFor(
        [&] {
            auto* git = controller(rig);
            return git && git->snapshot() && git->snapshot()->status.find("a.txt");
        },
        10s));
    QPointer<ui::GitController> old(controller(rig));
    ASSERT_TRUE(rig.workbench->openPath(second.work()));
    EXPECT_TRUE(old.isNull());
    ASSERT_TRUE(waitFor(
        [&] {
            return controller(rig) && controller(rig)->snapshot() && controller(rig)->info().workTree == second.work();
        },
        20s));
    EXPECT_EQ(controller(rig)->snapshot()->status.find("a.txt"), nullptr);
}

TEST(WorkbenchGit, EditsMadeDuringCommitRemainDirtyAndOutOfCommit) {
    TempRepo repo("app-git-late-edit");
    GitConfig config(repo);
    repo.init();
    repo.write("file.txt", "original\n");
    repo.commitAll("initial");
    repo.hook("pre-commit", "echo started > hook-started\nsleep 2");
    WorkDir dir("git-workbench-late");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.path("file.txt")));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    ASSERT_TRUE(rig.view()->editor().typeText("first "));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCommit), input::InvokeResult::Ran);
    auto* commit = commitWindow(rig);
    ASSERT_NE(commit, nullptr);
    commit->setMessage(QStringLiteral("first edit"));
    ASSERT_TRUE(commit->commit());
    ASSERT_TRUE(waitFor([&] { return repo.exists("hook-started"); }, 20s));
    controller(rig)->rollback({repo.path("file.txt")}); // Must not queue behind the held commit.
    ASSERT_TRUE(rig.view()->editor().typeText("late "));
    ASSERT_TRUE(waitFor([&] { return controller(rig)->lastResult().has_value(); }, 20s));
    ASSERT_TRUE(controller(rig)->lastResult()->succeeded());
    EXPECT_EQ(repo.git({"show", "HEAD:file.txt"}).stdoutBytes, "first original\n");
    EXPECT_EQ(rig.view()->editor().document().text().text(), "first late original\n");
    EXPECT_TRUE(rig.view()->document().dirty());
}

TEST(WorkbenchGit, EditingDuringRollbackCancelsItAndBlocksSavingUntilCompletion) {
    TempRepo repo("app-git-rollback-late");
    GitConfig config(repo);
    repo.init();
    repo.write("file.txt", "original\n");
    repo.commitAll("initial");
    repo.write("file.txt", "changed\n");
    WorkDir dir("git-workbench-rollback-late");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.path("file.txt")));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    repo.hook("post-index-change", "echo started > rollback-started\nsleep 60");
    controller(rig)->rollback({repo.path("file.txt")});
    ASSERT_TRUE(waitFor([&] { return repo.exists("rollback-started"); }, 20s));
    ASSERT_TRUE(rig.view()->editor().typeText("late "));
    auto saved = rig.workbench->save(*rig.view());
    ASSERT_FALSE(saved.has_value());
    EXPECT_EQ(saved.error().code(), ErrorCode::Busy);
    EXPECT_TRUE(rig.view()->document().dirty());
    EXPECT_TRUE(waitFor([&] { return controller(rig)->lastResult().has_value(); }, 20s));
    EXPECT_FALSE(controller(rig)->lastResult()->succeeded());
    EXPECT_TRUE(rig.view()->editor().document().text().text().starts_with("late "));
    EXPECT_TRUE(rig.view()->document().dirty());
}

TEST(WorkbenchGit, ProjectSwitchCancelsRunningCommitHook) {
    TempRepo first("app-git-cancel-a");
    GitConfig config(first);
    first.init();
    first.write("file.txt", "original\n");
    first.commitAll("initial");
    first.write("file.txt", "changed\n");
    first.hook("pre-commit", "echo started > hook-started\nsleep 30");
    const std::string before = first.git({"rev-parse", "HEAD"}).stdoutBytes;
    TempRepo second("app-git-cancel-b");
    second.init();
    second.write("other.txt", "other\n");
    second.commitAll("initial");
    WorkDir dir("git-workbench-cancel");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(first.work()));
    ASSERT_TRUE(rig.workbench->openPath(first.work()));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCommit), input::InvokeResult::Ran);
    auto* commit = commitWindow(rig);
    ASSERT_NE(commit, nullptr);
    commit->setMessage(QStringLiteral("must be cancelled"));
    ASSERT_TRUE(commit->commit());
    ASSERT_TRUE(waitFor([&] { return first.exists("hook-started"); }, 20s));
    QPointer<ui::GitController> old(controller(rig));
    ASSERT_TRUE(rig.workbench->openPath(second.work()));
    EXPECT_TRUE(old.isNull());
    EXPECT_EQ(first.git({"rev-parse", "HEAD"}).stdoutBytes, before);
    EXPECT_EQ(first.read("file.txt"), "changed\n");
}

TEST(WorkbenchGit, ShutdownCancelsGitAndQueuedActionsStayInert) {
    TempRepo repo("app-git-shutdown");
    GitConfig config(repo);
    repo.init();
    repo.write("file.txt", "original\n");
    repo.commitAll("initial");
    repo.write("file.txt", "changed\n");
    repo.hook("pre-commit", "echo started > hook-started\nsleep 30");
    const auto head = repo.git({"rev-parse", "HEAD"}).stdoutBytes;
    WorkDir dir("git-workbench-shutdown");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.work()));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCommit), input::InvokeResult::Ran);
    auto* commit = commitWindow(rig);
    ASSERT_NE(commit, nullptr);
    commit->setMessage(QStringLiteral("must be cancelled"));
    ASSERT_TRUE(commit->commit());
    ASSERT_TRUE(waitFor([&] { return repo.exists("hook-started"); }, 20s));
    QPointer<ui::GitController> old(controller(rig));
    QTimer::singleShot(0, rig.window.get(), [&rig] { (void)rig.window->invoke(ui::git_actions::kCommit); });
    rig.workbench->shutdown();
    QApplication::processEvents();
    EXPECT_TRUE(old.isNull());
    EXPECT_EQ(controller(rig), nullptr);
    EXPECT_EQ(repo.git({"rev-parse", "HEAD"}).stdoutBytes, head);
}

TEST(WorkbenchGit, TrustRevocationCancelsRunningCommitHook) {
    TempRepo repo("app-git-revoke-running");
    GitConfig config(repo);
    repo.init();
    repo.write("file.txt", "original\n");
    repo.commitAll("initial");
    repo.write("file.txt", "changed\n");
    repo.hook("pre-commit", "echo started > hook-started\nsleep 30");
    const auto head = repo.git({"rev-parse", "HEAD"}).stdoutBytes;
    WorkDir dir("git-workbench-revoke-running");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.work()));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCommit), input::InvokeResult::Ran);
    auto* commit = commitWindow(rig);
    ASSERT_NE(commit, nullptr);
    commit->setMessage(QStringLiteral("must be cancelled"));
    ASSERT_TRUE(commit->commit());
    ASSERT_TRUE(waitFor([&] { return repo.exists("hook-started"); }, 20s));
    QPointer<ui::GitController> old(controller(rig));
    ASSERT_TRUE(rig.trust->untrust(repo.work()));
    ASSERT_TRUE(waitFor([&] { return old.isNull(); }, 10s));
    EXPECT_EQ(repo.git({"rev-parse", "HEAD"}).stdoutBytes, head);
    EXPECT_EQ(repo.read("file.txt"), "changed\n");
}

TEST(WorkbenchGit, LinkedWorktreeDiscoversSeparateGitDirectoryAndRefreshes) {
    TempRepo repo("app-git-worktree");
    GitConfig config(repo);
    repo.init();
    repo.write("nested/file.txt", "original\n");
    repo.commitAll("initial");
    const std::string linked = TempRepo::join(repo.root(), "linked");
    repo.git({"worktree", "add", "-q", "-b", "linked-branch", linked});
    WorkDir dir("git-workbench-linked");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    const auto nested = TempRepo::join(linked, "nested");
    ASSERT_TRUE(rig.workbench->openPath(nested));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    EXPECT_EQ(controller(rig)->info().workTree, linked);
    EXPECT_NE(controller(rig)->info().gitDir, TempRepo::join(linked, ".git"));
    EXPECT_EQ(controller(rig)->currentBranch(), QStringLiteral("linked-branch"));
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(TempRepo::join(nested, "file.txt"), "external\n"));
    ASSERT_TRUE(waitFor(
        [&] {
            auto* git = controller(rig);
            return git && git->snapshot() && git->snapshot()->status.find("nested/file.txt");
        },
        10s));
}

TEST(WorkbenchGit, BranchWidgetAndPushUseOnlyLocalScratchRemote) {
    TempRepo repo("app-git-branch");
    GitConfig config(repo);
    const std::string bare = TempRepo::join(repo.root(), "origin.git");
    repo.git({"init", "-q", "--bare", bare}, 0, repo.root());
    repo.init();
    repo.git({"remote", "add", "origin", bare});
    repo.write("a.txt", "a\n");
    repo.commitAll("initial");
    WorkDir dir("git-workbench-branch");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.work()));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    auto* branch = rig.window->findChild<ui::BranchStatusWidget*>();
    ASSERT_NE(branch, nullptr);
    branch->runItem(QStringLiteral("new:"));
    ASSERT_NE(branch->newBranchPrompt(), nullptr);
    emit branch->newBranchPrompt()->submitted(QStringLiteral("feature"));
    branch->newBranchPrompt()->closePopup(ui::Popup::CloseReason::Accepted);
    ASSERT_TRUE(waitFor([&] { return controller(rig)->currentBranch() == QStringLiteral("feature"); }, 20s));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kPush), input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor(
        [&] {
            const auto& result = controller(rig)->lastResult();
            return result && result->kind == vcs::GitOperationKind::Push;
        },
        20s));
    ASSERT_TRUE(controller(rig)->lastResult()->succeeded());
    EXPECT_EQ(repo.git({"show-ref", "--verify", "refs/heads/feature"}, 0, bare).exitCode, 0);
}

TEST(WorkbenchGit, RestrictedReadDoesNotRunFsmonitorAndExplicitTrustEnablesOperations) {
    TempRepo repo("app-git-restricted");
    GitConfig config(repo);
    repo.init();
    repo.write("file.txt", "original\n");
    repo.commitAll("initial");
    repo.hook("fsmonitor-watchman", "echo ran > fsmonitor-ran\nexit 0");
    repo.git({"config", "core.fsmonitor", TempRepo::join(repo.info().gitDir, "hooks/fsmonitor-watchman")});
    repo.write("file.txt", "changed\n");
    WorkDir dir("git-workbench-restricted");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(repo.work()));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    EXPECT_FALSE(repo.exists("fsmonitor-ran"));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCommit), input::InvokeResult::Ran);
    auto* commit = commitWindow(rig);
    ASSERT_NE(commit, nullptr);
    commit->setMessage(QStringLiteral("refused"));
    ASSERT_TRUE(commit->commit());
    ASSERT_TRUE(controller(rig)->lastResult().has_value());
    ASSERT_TRUE(controller(rig)->lastResult()->error.has_value());
    EXPECT_EQ(controller(rig)->lastResult()->error->code(), ErrorCode::Untrusted);
    EXPECT_FALSE(repo.exists("fsmonitor-ran"));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kTrustRepository), input::InvokeResult::Ran);
    auto* dialog = rig.window->findChild<ui::TrustDialog*>();
    ASSERT_NE(dialog, nullptr);
    dialog->trustButton()->click();
    ASSERT_TRUE(waitFor([&] { return rig.trust->isTrusted(repo.work()); }, 5s));
    ASSERT_TRUE(waitFor([&] { return rig.window->projectTrusted(); }, 5s));
    ASSERT_NE(commitWindow(rig), nullptr);
    EXPECT_EQ(commitWindow(rig)->message(), QStringLiteral("refused"));
}

TEST(WorkbenchGit, RevokedTrustAndUnsavedRollbackDoNotModifyDocuments) {
    TempRepo repo("app-git-trust");
    GitConfig config(repo);
    repo.init();
    repo.write("file.txt", "original\n");
    repo.commitAll("initial");
    WorkDir dir("git-workbench-trust");
    app::test::RigOptions options;
    options.workbench.git = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.work()));
    ASSERT_TRUE(rig.workbench->openPath(repo.path("file.txt")));
    rig.show();
    ASSERT_TRUE(waitFor([&] { return controller(rig) && controller(rig)->snapshot(); }, 20s));
    ASSERT_TRUE(rig.view()->editor().typeText("dirty "));
    ASSERT_EQ(rig.window->invoke(ui::git_actions::kCommit), input::InvokeResult::Ran);
    auto* commit = commitWindow(rig);
    ASSERT_NE(commit, nullptr);
    commit->setConfirmHandler([](const QString&, const QString&) { return true; });
    ASSERT_TRUE(waitFor([&] { return commit->model().itemFor(repo.path("file.txt")) != nullptr; }, 5s));
    ASSERT_TRUE(commit->rollbackSelected());
    EXPECT_EQ(rig.view()->editor().document().text().text(), "dirty original\n");
    EXPECT_EQ(repo.read("file.txt"), "original\n");
    ASSERT_TRUE(rig.trust->untrust(repo.work()));
    commit->setMessage(QStringLiteral("must not commit"));
    ASSERT_TRUE(commit->commit());
    EXPECT_EQ(rig.view()->editor().document().text().text(), "dirty original\n");
    EXPECT_EQ(repo.read("file.txt"), "original\n");
    ASSERT_TRUE(controller(rig)->lastResult().has_value());
    EXPECT_FALSE(controller(rig)->lastResult()->succeeded());
}

// --- Get from VCS (welcome screen Clone): git clone off the GUI thread ---

TEST(WorkbenchGit, CloneRunsOffTheGuiThreadWithTheResolvedGitAndOpensTheClone) {
    TempRepo repo("app-git-clone");
    GitConfig config(repo);
    repo.init();
    for (int i = 0; i < 40; ++i)
        repo.write("src/file" + std::to_string(i) + ".cpp", std::string(4096, static_cast<char>('a' + i % 26)) + "\n");
    repo.commitAll("initial");
    WorkDir dir("git-clone");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.settings->set(settings::Scope::Application, settings::key::toolsGitPath, vcs::test::testGitPath()));
    const std::string target = dir.join("cloned");
    int ticks = 0;
    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, [&] { ++ticks; });
    ticker.start(1);
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(rig.workbench->cloneRepository(repo.work(), target));
    // The call returns at once: git runs on a worker while the event loop keeps turning.
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_TRUE(rig.workbench->cloneRunning());
    const QPointer<QWidget> progress = rig.window->findChild<QWidget*>(QStringLiteral("gitCloneProgressDialog"));
    EXPECT_NE(progress, nullptr);
    // A second clone while one runs is refused.
    EXPECT_EQ(rig.workbench->cloneRepository(repo.work(), dir.join("other")).error().code(), ErrorCode::Busy);
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->cloneRunning(); }, 60s));
    EXPECT_GT(ticks, 0);
    EXPECT_TRUE(QFileInfo::exists(QString::fromStdString(dir.join("cloned/src/file0.cpp"))));
    EXPECT_EQ(rig.workbench->projectRoot(), target);
    EXPECT_TRUE(!progress || !progress->isVisible()); // Closed (deleted at the next deferred delete).
}

TEST(WorkbenchGit, CloneRefusesBadArgumentsAndAFailedCloneShowsGitsOwnMessage) {
    TempRepo repo("app-git-clone-fail");
    GitConfig config(repo);
    WorkDir dir("git-clone-fail");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.settings->set(settings::Scope::Application, settings::key::toolsGitPath, vcs::test::testGitPath()));
    EXPECT_EQ(rig.workbench->cloneRepository("", dir.join("a")).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(rig.workbench->cloneRepository("--upload-pack=evil", dir.join("a")).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(rig.workbench->cloneRepository(repo.work(), "relative/dir").error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(rig.workbench->cloneRunning());
    // Not a repository: git fails and its own words reach the notification.
    ASSERT_TRUE(rig.workbench->cloneRepository(dir.join("no-such-repository"), dir.join("b")));
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->cloneRunning(); }, 30s));
    const auto& log = rig.window->notifications().log();
    ASSERT_FALSE(log.empty());
    EXPECT_EQ(log.back().notification.kind, ui::NotificationKind::Warning);
    EXPECT_TRUE(log.back().notification.text.contains(QStringLiteral("fatal")))
        << log.back().notification.text.toStdString();
    EXPECT_FALSE(rig.workbench->hasProject());
}

TEST(WorkbenchGit, CancellingACloneTerminatesGitAndOpensNothing) {
    WorkDir dir("git-clone-cancel");
    dir.write("tools/git.exe", "fixture");
    io::ScriptedProcessLauncher launcher;
    // A clone that reports progress and then hangs (a stalled server) until it is terminated.
    launcher.expect(io::ScriptedProcess{.expectExecutable = dir.join("tools/git.exe"),
                                        .stderrBytes = "Receiving objects:  10% (1/10)\r",
                                        .neverExits = true,
                                        .outputHeld = true});
    app::test::RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(
        rig.settings->set(settings::Scope::Application, settings::key::toolsGitPath, dir.join("tools/git.exe")));
    ASSERT_TRUE(rig.workbench->cloneRepository("https://example.invalid/repo.git", dir.join("cloned")));
    ASSERT_TRUE(waitFor([&] { return !launcher.launches().empty(); }, 5s));
    const auto spec = launcher.launches().front().spec; // launches() returns a copy.
    EXPECT_EQ(spec.executable, dir.join("tools/git.exe"));
    EXPECT_NE(std::ranges::find(spec.arguments, std::string("--")), spec.arguments.end());
    EXPECT_NE(std::ranges::find(spec.arguments, std::string("protocol.ext.allow=never")), spec.arguments.end());
    static_cast<void>(waitFor([] { return false; }, 200ms));
    EXPECT_TRUE(rig.workbench->cloneRunning());
    rig.workbench->cancelClone();
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->cloneRunning(); }, 5s));
    EXPECT_TRUE(launcher.launches().front().terminated);
    EXPECT_FALSE(rig.workbench->hasProject());
    const auto& log = rig.window->notifications().log();
    ASSERT_FALSE(log.empty());
    EXPECT_TRUE(log.back().notification.title.contains(QStringLiteral("cancel"), Qt::CaseInsensitive) ||
                log.back().notification.text.contains(QStringLiteral("cancel"), Qt::CaseInsensitive))
        << log.back().notification.text.toStdString();
}
} // namespace
