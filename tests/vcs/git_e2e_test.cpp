// SPDX-License-Identifier: Apache-2.0
//
// End-to-end tests of frost::vcs against real git on temporary repositories (label "git"):
// discovery, the status of every state (a rename and a conflict included), file versions, the
// status map after an edit and a commit, change markers against HEAD, commits of selected files
// only, amend, a failing and a cancelled pre-commit hook, rollback, branches and a refused checkout,
// fetch, pull and push against a local bare repository, restricted mode, and the side-by-side diff
// against `git diff --histogram` on the fixture pairs.

#include "frost/diff/line_sequence.hpp"
#include "frost/io/path.hpp"
#include "frost/vcs/git_repository.hpp"
#include "frost/vcs/git_service.hpp"
#include "frost/vcs/line_changes.hpp"
#include "frost/vcs/side_by_side.hpp"
#include "frost/vcs/vcs_status.hpp"

#include "diff/support/git_runner.hpp"
#include "diff/support/goldens.hpp"
#include "vcs/git_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::LineIndex;
using namespace frost::vcs;
using frost::vcs::test::TempRepo;
using namespace std::chrono_literals;

std::string trimmed(const GitOutput& output) { return std::string(trimGitText(output.stdoutBytes)); }

// Delegates real scratch-repository I/O except the one index publication under test.
class FailIndexPublication final : public frost::io::IFileSystem {
public:
    FailIndexPublication(frost::io::IFileSystem& inner, std::string index, bool failRead = false)
        : m_inner(inner), m_index(std::move(index)), m_failRead(failRead) {}
    frost::Result<std::string> readFile(std::string_view path) override {
        if (m_failRead && path.ends_with("frost-index"))
            return frost::fail(ErrorCode::IoError, "injected recovery index read failure");
        return m_inner.readFile(path);
    }
    frost::Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        if (!m_failRead && path == m_index)
            return frost::fail(ErrorCode::IoError, "injected index publication failure");
        return m_inner.writeFileAtomic(path, data);
    }
    frost::Result<void> writeFileAtomicWithMtime(std::string_view path, std::string_view data,
                                                 std::filesystem::file_time_type mtime) override {
        if (!m_failRead && path == m_index)
            return frost::fail(ErrorCode::IoError, "injected index publication failure");
        return m_inner.writeFileAtomicWithMtime(path, data, mtime);
    }
    frost::Result<frost::io::FileStatus> status(std::string_view path) override { return m_inner.status(path); }
    frost::Result<std::vector<frost::io::DirEntry>> listDirectory(std::string_view path) override {
        return m_inner.listDirectory(path);
    }
    frost::Result<frost::io::DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        return m_inner.listDirectoryWithDiagnostics(path);
    }
    frost::Result<void> createDirectories(std::string_view path) override { return m_inner.createDirectories(path); }
    frost::Result<void> removeFile(std::string_view path) override { return m_inner.removeFile(path); }
    frost::Result<void> removeDirectory(std::string_view path) override { return m_inner.removeDirectory(path); }
    frost::Result<bool> exists(std::string_view path) override { return m_inner.exists(path); }

private:
    frost::io::IFileSystem& m_inner;
    std::string m_index;
    bool m_failRead;
};

// tests/vcs/fixtures/<relative>, normalised.
std::string fixturePath(std::string_view relative) {
    return frost::io::normalizePath(std::string(FROST_VCS_FIXTURE_DIR) + "/" + std::string(relative)).value();
}

TEST(GitE2E, DiscoversTheWorkTreeFromAFileOrASubdirectory) {
    TempRepo repo("vcs-discover");
    repo.init();
    repo.write("src/deep/a.cpp", "int a;\n");
    repo.write(".gitmodules", "[submodule \"lib\"]\n\tpath = third_party/lib\n\turl = ../lib.git\n");
    auto fromFile = discoverRepository(repo.runner(), repo.path("src/deep/a.cpp"));
    ASSERT_TRUE(fromFile.has_value()) << fromFile.error().describe();
    ASSERT_TRUE(fromFile->has_value());
    EXPECT_EQ(vcsPathKey((*fromFile)->workTree), vcsPathKey(repo.work()));
    EXPECT_EQ(vcsPathKey((*fromFile)->gitDir), vcsPathKey(TempRepo::join(repo.work(), ".git")));
    ASSERT_EQ((*fromFile)->submodules.size(), 1U);
    EXPECT_EQ(vcsPathKey((*fromFile)->submodules.front()), vcsPathKey(repo.path("third_party/lib")));

    auto fromDirectory = discoverRepository(repo.runner(), repo.path("src"));
    ASSERT_TRUE(fromDirectory.has_value()) << fromDirectory.error().describe();
    ASSERT_TRUE(fromDirectory->has_value());
    EXPECT_EQ(vcsPathKey((*fromDirectory)->workTree), vcsPathKey(repo.work()));

    auto outside = discoverRepository(repo.runner(), repo.root());
    ASSERT_TRUE(outside.has_value()) << outside.error().describe();
    EXPECT_FALSE(outside->has_value());
    auto version = repo.runner().version();
    ASSERT_TRUE(version.has_value()) << version.error().describe();
    EXPECT_TRUE(version->starts_with("git version")) << *version;
}

TEST(GitE2E, StatusOfEveryStateIncludingARenameAndAConflict) {
    TempRepo repo("vcs-status");
    repo.init();
    repo.write("modified.txt", "one\n");
    repo.write("staged.txt", "one\n");
    repo.write("deleted.txt", "one\n");
    repo.write("rename-me.txt", "a file that is renamed with enough content to be found\n");
    repo.write("conflict.txt", "base\n");
    repo.commitAll("base");

    repo.git({"switch", "-q", "-c", "other"});
    repo.write("conflict.txt", "other side\n");
    repo.commitAll("other");
    repo.git({"switch", "-q", "main"});
    repo.write("conflict.txt", "main side\n");
    repo.commitAll("main");
    repo.git({"merge", "-q", "other"}, 1);

    repo.write("modified.txt", "two\n");
    repo.write("staged.txt", "two\n");
    repo.git({"add", "staged.txt"});
    ASSERT_TRUE(repo.fs().removeFile(repo.path("deleted.txt")).has_value());
    repo.git({"mv", "rename-me.txt", "renamed.txt"});
    repo.write("added.txt", "new\n");
    repo.git({"add", "added.txt"});
    repo.write("untracked dir/new file.txt", "?\n");

    const GitStatus status = repo.status();
    EXPECT_EQ(status.branch.head, "main");
    EXPECT_FALSE(status.branch.unborn());
    const auto expect = [&](std::string_view path, VcsFileStatus file) {
        const GitStatusEntry* entry = status.find(path);
        ASSERT_NE(entry, nullptr) << path;
        EXPECT_EQ(entry->fileStatus(), file) << path << ": " << toString(entry->fileStatus());
    };
    expect("modified.txt", VcsFileStatus::Modified);
    expect("staged.txt", VcsFileStatus::Modified);
    expect("deleted.txt", VcsFileStatus::Deleted);
    expect("renamed.txt", VcsFileStatus::Renamed);
    expect("added.txt", VcsFileStatus::Added);
    expect("untracked dir/new file.txt", VcsFileStatus::Untracked);
    expect("conflict.txt", VcsFileStatus::Conflicted);
    EXPECT_TRUE(status.find("staged.txt")->staged());
    EXPECT_FALSE(status.find("modified.txt")->staged());
    EXPECT_EQ(status.find("renamed.txt")->originalPath, "rename-me.txt");
    EXPECT_EQ(status.find("conflict.txt")->kind, GitEntryKind::Unmerged);
}

TEST(GitE2E, FileVersionsFromHeadAndTheIndex) {
    TempRepo repo("vcs-content");
    repo.init();
    EXPECT_EQ(repo.repository().headContent(repo.path("a.txt")).value(), std::nullopt); // No commit yet.
    EXPECT_EQ(repo.repository().lastCommitMessage().value(), "");
    repo.write("a.txt", "committed\r\nline\n");
    repo.commitAll("first\n\nbody");
    repo.write("a.txt", "staged\n");
    repo.git({"add", "a.txt"});
    repo.write("a.txt", "working\n");
    repo.write("new.txt", "untracked\n");
    EXPECT_EQ(repo.repository().headContent(repo.path("a.txt")).value(),
              std::optional<std::string>("committed\r\nline\n"));
    EXPECT_EQ(repo.repository().indexContent(repo.path("a.txt")).value(), std::optional<std::string>("staged\n"));
    EXPECT_EQ(repo.repository().headContent(repo.path("new.txt")).value(), std::nullopt);
    EXPECT_EQ(repo.repository().headContent(repo.path("missing/x.txt")).value(), std::nullopt);
    EXPECT_EQ(repo.repository().headContent(TempRepo::join(repo.root(), "outside.txt")).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(repo.repository().lastCommitMessage().value(), "first\n\nbody");
    repo.commitAll("second");
    const auto messages = repo.repository().recentCommitMessages(5).value();
    EXPECT_EQ(messages, (std::vector<std::string>{"second", "first\n\nbody"}));
}

TEST(GitE2E, StatusMapFollowsAnEditAndACommit) {
    TempRepo repo("vcs-service");
    repo.init();
    repo.write("a.txt", "one\n");
    repo.write("b.txt", "one\n");
    repo.commitAll("base");

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<GitOperationResult> results;
    std::vector<std::string> console;
    GitServiceCallbacks callbacks;
    callbacks.onOperationFinished = [&](const GitOperationResult& result) {
        std::lock_guard lock(mutex);
        results.push_back(result);
        changed.notify_all();
    };
    callbacks.onOutput = [&](GitStream stream, std::string_view line) {
        std::lock_guard lock(mutex);
        if (stream == GitStream::Command) {
            console.emplace_back(line);
        }
    };
    GitServiceOptions options;
    options.debounce = 50ms;
    options.maxDelay = 200ms;
    auto service = GitService::create(repo.runner(), repo.fs(), repo.info(), options, std::move(callbacks));
    ASSERT_TRUE(service->waitForIdle(30s));
    ASSERT_NE(service->snapshot(), nullptr);
    const std::uint64_t first = service->generation();
    EXPECT_GE(first, 1U);
    EXPECT_EQ(service->statusOf(repo.path("a.txt")), VcsFileStatus::Unmodified);
    EXPECT_EQ(service->statusOf(TempRepo::join(repo.root(), "elsewhere.txt")), std::nullopt);

    // An edit reported by the watcher refreshes the colours after the debounce.
    repo.write("a.txt", "two\n");
    repo.write("c.txt", "new\n");
    frost::io::FileChangeBatch batch;
    batch.changes.push_back(
        frost::io::FileChange{.kind = frost::io::FileChangeKind::Modified, .path = repo.path("a.txt")});
    EXPECT_TRUE(service->affectsStatus(repo.path("a.txt")));
    EXPECT_TRUE(service->affectsStatus(TempRepo::join(repo.info().gitDir, "index")));
    EXPECT_FALSE(service->affectsStatus(TempRepo::join(TempRepo::join(repo.info().gitDir, "objects"), "cdef")));
    EXPECT_FALSE(service->affectsStatus(TempRepo::join(repo.root(), "elsewhere.txt")));
    service->applyChanges(batch);
    ASSERT_TRUE(service->waitForIdle(30s));
    EXPECT_GT(service->generation(), first);
    EXPECT_EQ(service->statusOf(repo.path("a.txt")), VcsFileStatus::Modified);
    EXPECT_EQ(service->statusOf(repo.path("c.txt")), VcsFileStatus::Untracked);
    EXPECT_EQ(service->statusOf(repo.path("b.txt")), VcsFileStatus::Unmodified);

    // Our own commit refreshes at once.
    const std::uint64_t beforeCommit = service->generation();
    service->commit(
        CommitRequest{.paths = {repo.path("a.txt"), repo.path("c.txt")}, .message = "edit", .amend = false});
    ASSERT_TRUE(service->waitForIdle(30s));
    {
        std::lock_guard lock(mutex);
        ASSERT_EQ(results.size(), 1U);
        EXPECT_TRUE(results[0].succeeded()) << results[0].error->describe();
        EXPECT_EQ(results[0].kind, GitOperationKind::Commit);
        EXPECT_FALSE(results[0].commit.empty());
        EXPECT_FALSE(console.empty());
    }
    EXPECT_GT(service->generation(), beforeCommit);
    EXPECT_EQ(service->statusOf(repo.path("a.txt")), VcsFileStatus::Unmodified);
    EXPECT_EQ(service->statusOf(repo.path("c.txt")), VcsFileStatus::Unmodified);
    EXPECT_EQ(service->snapshot()->files->size(), 0U);

    // Reads run on the read queue.
    std::optional<std::string> head;
    service->readHeadContent(repo.path("a.txt"), [&](frost::Result<std::optional<std::string>> content) {
        std::lock_guard lock(mutex);
        head = content ? content->value_or("<none>") : "<error>";
    });
    ASSERT_TRUE(service->waitForIdle(30s));
    std::lock_guard lock(mutex);
    EXPECT_EQ(head, std::optional<std::string>("two\n"));
}

TEST(GitE2E, ChangeMarkersAgainstHeadAndRollbackOfALineRange) {
    TempRepo repo("vcs-markers");
    repo.init();
    repo.write("code.cpp", "int a;\nint b;\nint c;\nint d;\n");
    repo.commitAll("base");
    auto headBytes = repo.repository().headContent(repo.path("code.cpp"));
    ASSERT_TRUE(headBytes.has_value() && headBytes->has_value());
    const auto base = frost::diff::LineSequence::fromBytes(**headBytes, frost::diff::LineSplit::Lsp).value();
    const std::string buffer = "// new\nint a;\nint B;\nint d;\n";
    const auto current = frost::diff::LineSequence::fromBytes(buffer, frost::diff::LineSplit::Lsp).value();
    auto changes = computeLineChanges(base, current);
    ASSERT_TRUE(changes.has_value()) << changes.error().describe();
    ASSERT_EQ(changes->size(), 2U);
    EXPECT_EQ((*changes)[0].kind, LineChangeKind::Added);
    EXPECT_EQ((*changes)[1].kind, LineChangeKind::Modified); // "int b;\nint c;" -> "int B;"
    EXPECT_EQ(baseText(base, (*changes)[1]), "int b;\nint c;\n");
    const LineRollback rollback = rollbackEdit(base, current, (*changes)[1]);
    std::string rolled = buffer;
    rolled.replace(rollback.range.begin.value(), static_cast<std::size_t>(rollback.range.size()), rollback.text);
    EXPECT_EQ(rolled, "// new\nint a;\nint b;\nint c;\nint d;\n");
}

TEST(GitE2E, CommitsTheSelectedFilesOnly) {
    TempRepo repo("vcs-commit");
    repo.init();
    repo.write("selected.txt", "one\n");
    repo.write("unselected.txt", "one\n");
    repo.write("staged-unselected.txt", "one\n");
    repo.commitAll("base");
    repo.write("selected.txt", "two\n");
    repo.write("unselected.txt", "two\n");
    repo.write("staged-unselected.txt", "two\n");
    repo.git({"add", "staged-unselected.txt"});
    repo.write("brand new.txt", "new\n");

    auto committed = repo.repository().commit(CommitRequest{
        .paths = {repo.path("selected.txt"), repo.path("brand new.txt")}, .message = "  selected\n\n", .amend = false});
    ASSERT_TRUE(committed.has_value()) << committed.error().describe();
    EXPECT_EQ(committed->commit, trimmed(repo.git({"rev-parse", "HEAD"})));
    EXPECT_EQ(trimmed(repo.git({"log", "-1", "--format=%B"})), "selected");
    EXPECT_EQ(trimmed(repo.git({"show", "--name-only", "--format=", "HEAD"})), "brand new.txt\nselected.txt");

    const GitStatus status = repo.status();
    EXPECT_EQ(status.find("selected.txt"), nullptr);
    EXPECT_EQ(status.find("brand new.txt"), nullptr);
    const GitStatusEntry* unselected = status.find("unselected.txt");
    ASSERT_NE(unselected, nullptr);
    EXPECT_FALSE(unselected->staged()); // An unselected modified file stays unstaged.
    EXPECT_TRUE(unselected->unstaged());
    const GitStatusEntry* staged = status.find("staged-unselected.txt");
    ASSERT_NE(staged, nullptr);
    EXPECT_TRUE(staged->staged()); // And unselected index state is left alone.
    EXPECT_FALSE(repo.exists(".git/FROST_COMMIT_MSG"));

    EXPECT_EQ(repo.repository()
                  .commit(CommitRequest{.paths = {repo.path("unselected.txt")}, .message = " \n", .amend = false})
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(repo.repository().commit(CommitRequest{.paths = {}, .message = "x", .amend = false}).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(GitE2E, CommitSupportsUnbornAndSplitIndexesAndRespectsExistingLock) {
    TempRepo repo("vcs-private-index");
    repo.init();
    repo.write("new.txt", "first\n");
    auto first =
        repo.repository().commit(CommitRequest{.paths = {repo.path("new.txt")}, .message = "initial", .amend = false});
    ASSERT_TRUE(first.has_value()) << first.error().describe();
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:new.txt"})), "first");
    EXPECT_EQ(trimmed(repo.git({"status", "--porcelain"})), "");
    repo.git({"update-index", "--split-index"});
    repo.write("new.txt", "second\n");
    auto second =
        repo.repository().commit(CommitRequest{.paths = {repo.path("new.txt")}, .message = "second", .amend = false});
    ASSERT_TRUE(second.has_value()) << second.error().describe();
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:new.txt"})), "second");
    EXPECT_EQ(trimmed(repo.git({"status", "--porcelain"})), "");
    const auto lock = TempRepo::join(repo.info().gitDir, "index.lock");
    ASSERT_TRUE(repo.fs().writeFileAtomic(lock, "external lock").has_value());
    repo.write("new.txt", "third\n");
    auto blocked =
        repo.repository().commit(CommitRequest{.paths = {repo.path("new.txt")}, .message = "blocked", .amend = false});
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error().code(), ErrorCode::Busy);
    EXPECT_EQ(repo.fs().readFile(lock).value(), "external lock");
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:new.txt"})), "second");
}

TEST(GitE2E, AmendRewordsAndExtendsTheLastCommit) {
    TempRepo repo("vcs-amend");
    repo.init();
    repo.write("a.txt", "one\n");
    repo.commitAll("first");
    repo.write("b.txt", "one\n");
    repo.commitAll("second");
    EXPECT_EQ(repo.repository().lastCommitMessage().value(), "second");
    repo.write("b.txt", "two\n");
    auto amended = repo.repository().commit(
        CommitRequest{.paths = {repo.path("b.txt")}, .message = "second, amended", .amend = true});
    ASSERT_TRUE(amended.has_value()) << amended.error().describe();
    EXPECT_EQ(trimmed(repo.git({"rev-list", "--count", "HEAD"})), "2");
    EXPECT_EQ(repo.repository().lastCommitMessage().value(), "second, amended");
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:b.txt"})), "two");
    // Amend with nothing selected only rewords.
    auto reworded = repo.repository().commit(CommitRequest{.paths = {}, .message = "reworded", .amend = true});
    ASSERT_TRUE(reworded.has_value()) << reworded.error().describe();
    EXPECT_EQ(trimmed(repo.git({"rev-list", "--count", "HEAD"})), "2");
    EXPECT_EQ(repo.repository().lastCommitMessage().value(), "reworded");
}

TEST(GitE2E, AFailingPreCommitHookLeavesEverythingAsItWas) {
    TempRepo repo("vcs-hook");
    repo.init();
    repo.write("a.txt", "one\n");
    repo.commitAll("base");
    repo.hook("pre-commit", "echo 'lint: trailing whitespace in a.txt' >&2\nexit 1");
    repo.write("a.txt", "two\n");
    repo.write("new.txt", "new\n");
    const std::string head = trimmed(repo.git({"rev-parse", "HEAD"}));
    std::vector<std::string> lines;
    RunControl control;
    control.onLine = [&](GitStream stream, std::string_view line) {
        if (stream == GitStream::Stderr) {
            lines.emplace_back(line);
        }
    };
    auto committed = repo.repository().commit(
        CommitRequest{.paths = {repo.path("a.txt"), repo.path("new.txt")}, .message = "blocked", .amend = false},
        control);
    ASSERT_FALSE(committed.has_value());
    EXPECT_NE(committed.error().message().find("lint: trailing whitespace"), std::string::npos)
        << committed.error().message();
    EXPECT_NE(std::ranges::find(lines, "lint: trailing whitespace in a.txt"), lines.end());
    EXPECT_EQ(trimmed(repo.git({"rev-parse", "HEAD"})), head);
    const GitStatus status = repo.status();
    ASSERT_NE(status.find("a.txt"), nullptr);
    EXPECT_FALSE(status.find("a.txt")->staged());
    ASSERT_NE(status.find("new.txt"), nullptr);
    EXPECT_EQ(status.find("new.txt")->fileStatus(), VcsFileStatus::Untracked); // Unstaged again.
    EXPECT_EQ(repo.read("a.txt"), "two\n");
}

TEST(GitE2E, CancellingACommitEndsTheHookProcessTree) {
    TempRepo repo("vcs-cancel");
    repo.init();
    repo.write("a.txt", "one\n");
    repo.commitAll("base");
    repo.hook("pre-commit", "echo reached > hook-started\nsleep 60\nexit 0");
    repo.write("a.txt", "two\n");
    std::stop_source stop;
    std::atomic<bool> done{false};
    frost::Result<CommitResult> result = frost::fail(ErrorCode::Internal, "not run");
    const auto started = std::chrono::steady_clock::now();
    std::jthread worker([&] {
        result =
            repo.repository().commit(CommitRequest{.paths = {repo.path("a.txt")}, .message = "slow", .amend = false},
                                     RunControl{stop.get_token(), {}});
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!repo.exists("hook-started") && !done && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    const bool reached = repo.exists("hook-started");
    stop.request_stop();
    worker.join();
    ASSERT_TRUE(done.load());
    ASSERT_TRUE(reached) << (result ? "unexpected success" : result.error().describe());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
    // Nothing was committed and the index is usable (no lock left behind once git is gone).
    EXPECT_EQ(trimmed(repo.git({"rev-list", "--count", "HEAD"})), "1");
    ASSERT_TRUE(
        repo.fs().removeFile(TempRepo::join(TempRepo::join(repo.info().gitDir, "hooks"), "pre-commit")).has_value());
    static_cast<void>(repo.fs().removeFile(TempRepo::join(repo.info().gitDir, "index.lock")));
    auto again =
        repo.repository().commit(CommitRequest{.paths = {repo.path("a.txt")}, .message = "fast", .amend = false});
    EXPECT_TRUE(again.has_value()) << again.error().describe();
}

TEST(GitE2E, SameSizeRacyEditSurvivesPrivateIndexAndCancelledCommit) {
    TempRepo repo("vcs-racy-private-index");
    repo.init();
    repo.git({"config", "core.trustctime", "false"});
    repo.write("a.txt", "one\n");
    repo.commitAll("base");
    const auto file = frost::io::toStdPath(repo.path("a.txt")).value();
    const auto index = frost::io::toStdPath(TempRepo::join(repo.info().gitDir, "index")).value();
    const auto oldTime = std::filesystem::last_write_time(file);
    repo.write("a.txt", "two\n"); // Same byte count and mtime as the cached blob.
    std::filesystem::last_write_time(file, oldTime);
    std::filesystem::last_write_time(index, oldTime);
    std::this_thread::sleep_for(1200ms); // A newly copied index must have a later timestamp.
    ASSERT_EQ(trimmed(repo.git({"show", "HEAD:a.txt"})), "one");
    ASSERT_EQ(repo.read("a.txt"), "two\n");
    GitInvocation status;
    status.workingDirectory = repo.work();
    status.arguments = {"status", "--porcelain"};
    status.background = true;
    auto realStatus = repo.runner().run(status);
    ASSERT_TRUE(realStatus.has_value());
    ASSERT_NE(realStatus->stdoutBytes.find("a.txt"), std::string::npos);
    repo.hook("pre-commit", "echo reached > hook-started\nsleep 60");
    std::stop_source stop;
    frost::Result<CommitResult> result = frost::fail(ErrorCode::Internal, "not run");
    std::atomic<bool> done{false};
    std::jthread worker([&] {
        result =
            repo.repository().commit(CommitRequest{.paths = {repo.path("a.txt")}, .message = "cancel", .amend = false},
                                     RunControl{stop.get_token(), {}});
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!repo.exists("hook-started") && !done && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    const bool reached = repo.exists("hook-started");
    stop.request_stop();
    worker.join();
    ASSERT_TRUE(reached) << (result ? "unexpected success" : result.error().describe());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:a.txt"})), "one");
    EXPECT_EQ(trimmed(repo.git({"show", ":a.txt"})), "one");
    EXPECT_EQ(repo.read("a.txt"), "two\n");
    ASSERT_TRUE(
        repo.fs().removeFile(TempRepo::join(TempRepo::join(repo.info().gitDir, "hooks"), "pre-commit")).has_value());
    auto retried =
        repo.repository().commit(CommitRequest{.paths = {repo.path("a.txt")}, .message = "retry", .amend = false});
    ASSERT_TRUE(retried.has_value()) << retried.error().describe();
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:a.txt"})), "two");
    EXPECT_EQ(trimmed(repo.git({"show", ":a.txt"})), "two");
}

TEST(GitE2E, PublishingIndexKeepsUnselectedRacyEditsVisibleWithSplitIndex) {
    for (const bool split : {false, true}) {
        SCOPED_TRACE(split);
        TempRepo repo(split ? "vcs-racy-publish-split" : "vcs-racy-publish");
        repo.init();
        repo.git({"config", "core.trustctime", "false"});
        repo.write("selected.txt", "old\n");
        repo.write("other.txt", "one\n");
        repo.write("staged.txt", "original\n");
        repo.commitAll("base");
        repo.write("staged.txt", "staged\n");
        repo.git({"add", "staged.txt"});
        if (split)
            repo.git({"update-index", "--split-index"});
        const auto file = frost::io::toStdPath(repo.path("other.txt")).value();
        const auto index = frost::io::toStdPath(TempRepo::join(repo.info().gitDir, "index")).value();
        const auto oldTime = std::filesystem::last_write_time(file);
        repo.write("other.txt", "two\n");
        std::filesystem::last_write_time(file, oldTime);
        std::filesystem::last_write_time(index, oldTime);
        repo.write("selected.txt", "changed\n");
        repo.write("new.txt", "new\n"); // Exercises the intermediate private-index rewrite.
        std::this_thread::sleep_for(1200ms);
        auto result = repo.repository().commit(CommitRequest{
            .paths = {repo.path("selected.txt"), repo.path("new.txt")}, .message = "selected", .amend = false});
        ASSERT_TRUE(result.has_value()) << result.error().describe();
        EXPECT_EQ(trimmed(repo.git({"show", "HEAD:other.txt"})), "one");
        EXPECT_EQ(trimmed(repo.git({"show", ":other.txt"})), "one");
        EXPECT_EQ(trimmed(repo.git({"show", ":staged.txt"})), "staged");
        EXPECT_EQ(repo.read("other.txt"), "two\n");
        GitInvocation status;
        status.workingDirectory = repo.work();
        status.arguments = {"status", "--porcelain"};
        status.background = true;
        auto checked = repo.runner().run(status);
        ASSERT_TRUE(checked.has_value());
        EXPECT_NE(checked->stdoutBytes.find("other.txt"), std::string::npos) << checked->stdoutBytes;
        EXPECT_NE(checked->stdoutBytes.find("staged.txt"), std::string::npos) << checked->stdoutBytes;
    }
}

TEST(GitE2E, CancelledUntrackedCommitRestoresIndexWithoutRunningCleanupHook) {
    TempRepo repo("vcs-cancel-untracked");
    repo.init();
    repo.write("tracked.txt", "base\n");
    repo.commitAll("base");
    repo.write("tracked.txt", "staged\n");
    repo.git({"add", "tracked.txt"});
    repo.write("new.txt", "new\n");
    repo.hook("pre-commit", "echo started > hook-started\nsleep 60");
    std::stop_source stop;
    frost::Result<CommitResult> result = frost::fail(ErrorCode::Internal, "not run");
    std::jthread worker([&] {
        result = repo.repository().commit(
            CommitRequest{.paths = {repo.path("new.txt")}, .message = "cancel", .amend = false},
            RunControl{stop.get_token(), {}});
    });
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!repo.exists("hook-started") && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(repo.exists("hook-started"));
    EXPECT_EQ(repo.git({"add", "new.txt"}, 128).exitCode, 128); // A concurrent Git writer cannot take the index.
    repo.hook("post-index-change", "echo ran > cleanup-hook-ran\nsleep 60");
    const auto started = std::chrono::steady_clock::now();
    stop.request_stop();
    worker.join();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled) << result.error().describe();
    EXPECT_FALSE(repo.exists("cleanup-hook-ran"));
    EXPECT_EQ(result.error().describe().find("Could not restore"), std::string::npos) << result.error().describe();
    EXPECT_EQ(trimmed(repo.git({"diff", "--cached", "--name-only"})), "tracked.txt");
    EXPECT_EQ(repo.read("new.txt"), "new\n");
}

TEST(GitE2E, CancellingPostCommitRetainsIndexForAmbiguousRecovery) {
    TempRepo repo("vcs-post-commit-cancel");
    repo.init();
    repo.write("a.txt", "old\n");
    repo.write("other.txt", "old other\n");
    repo.commitAll("base");
    repo.write("a.txt", "new\n");
    repo.write("other.txt", "staged other\n");
    repo.git({"add", "other.txt"});
    repo.hook("post-commit", "echo started > post-commit-started\nsleep 60");
    std::stop_source stop;
    frost::Result<CommitResult> result = frost::fail(ErrorCode::Internal, "not run");
    std::jthread worker([&] {
        result = repo.repository().commit(
            CommitRequest{.paths = {repo.path("a.txt")}, .message = "committed", .amend = false},
            RunControl{stop.get_token(), {}});
    });
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!repo.exists("post-commit-started") && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    const bool reached = repo.exists("post-commit-started");
    stop.request_stop();
    worker.join();
    ASSERT_TRUE(reached);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("provenance and index recovery are uncertain"), std::string::npos)
        << result.error().message();
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:a.txt"})), "new");
    EXPECT_EQ(trimmed(repo.git({"show", ":a.txt"})), "old");
    EXPECT_EQ(trimmed(repo.git({"show", ":other.txt"})), "staged other");
    const auto recovery = TempRepo::join(TempRepo::join(repo.info().gitDir, "index.lock"), "frost-index");
    ASSERT_TRUE(repo.fs().exists(recovery).value());
    GitInvocation inspect;
    inspect.workingDirectory = repo.work();
    inspect.arguments = {"show", ":a.txt"};
    inspect.environment = {{"GIT_INDEX_FILE", recovery}};
    auto saved = repo.runner().run(inspect);
    ASSERT_TRUE(saved.has_value());
    EXPECT_TRUE(saved->succeeded());
    EXPECT_EQ(trimmed(*saved), "new");
    inspect.arguments = {"show", ":other.txt"};
    saved = repo.runner().run(inspect);
    ASSERT_TRUE(saved.has_value());
    EXPECT_TRUE(saved->succeeded());
    EXPECT_EQ(trimmed(*saved), "staged other");
}

TEST(GitE2E, CancellingCommittedReferenceTransactionRetainsBothPrivateIndexes) {
    TempRepo repo("vcs-ref-transaction-cancel");
    repo.init();
    repo.write("a.txt", "old\n");
    repo.write("other.txt", "old other\n");
    repo.commitAll("base");
    repo.write("a.txt", "new\n");
    repo.write("other.txt", "staged other\n");
    repo.git({"add", "other.txt"});
    repo.hook("reference-transaction", "if test \"$1\" = committed; then\n"
                                       "  echo reached > ref-committed\n"
                                       "  sleep 60\n"
                                       "fi");
    std::stop_source stop;
    frost::Result<CommitResult> result = frost::fail(ErrorCode::Internal, "not run");
    std::jthread worker([&] {
        result = repo.repository().commit(
            CommitRequest{.paths = {repo.path("a.txt")}, .message = "committed", .amend = false},
            RunControl{stop.get_token(), {}});
    });
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!repo.exists("ref-committed") && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    const bool reached = repo.exists("ref-committed");
    stop.request_stop();
    worker.join();
    ASSERT_TRUE(reached);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("provenance and index recovery are uncertain"), std::string::npos)
        << result.error().message();
    EXPECT_NE(result.error().message().find("frost-index.lock"), std::string::npos);
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:a.txt"})), "new");
    EXPECT_EQ(trimmed(repo.git({"show", ":a.txt"})), "old"); // No incorrect index was published.
    const auto recovery = TempRepo::join(TempRepo::join(repo.info().gitDir, "index.lock"), "frost-index");
    ASSERT_TRUE(repo.fs().exists(recovery).value());
    ASSERT_TRUE(repo.fs().exists(recovery + ".lock").value());
    GitInvocation inspect;
    inspect.workingDirectory = repo.work();
    inspect.arguments = {"show", ":a.txt"};
    inspect.environment = {{"GIT_INDEX_FILE", recovery + ".lock"}};
    auto pending = repo.runner().run(inspect);
    ASSERT_TRUE(pending.has_value()) << pending.error().describe();
    ASSERT_TRUE(pending->succeeded()) << pending->stderrBytes;
    EXPECT_EQ(trimmed(*pending), "new");
    inspect.arguments = {"show", ":other.txt"};
    pending = repo.runner().run(inspect);
    ASSERT_TRUE(pending.has_value()) << pending.error().describe();
    ASSERT_TRUE(pending->succeeded()) << pending->stderrBytes;
    EXPECT_EQ(trimmed(*pending), "staged other");
}

TEST(GitE2E, ExternalRefChangeDuringFailedCommitDoesNotPublishPrivateIndex) {
    TempRepo repo("vcs-external-ref");
    repo.init();
    repo.write("tracked.txt", "base\n");
    repo.write("other.txt", "old\n");
    repo.commitAll("base");
    repo.write("other.txt", "staged\n");
    repo.git({"add", "other.txt"});
    repo.write("new.txt", "untracked\n");
    const auto oldHead = trimmed(repo.git({"rev-parse", "HEAD"}));
    const auto tree = trimmed(repo.git({"rev-parse", "HEAD^{tree}"}));
    const auto external = trimmed(repo.git({"commit-tree", tree, "-p", oldHead, "-m", "external"}));
    repo.hook("pre-commit", "echo reached > hook-started\nwhile test ! -f release-hook; do sleep 0.1; done\nexit 1");
    frost::Result<CommitResult> result = frost::fail(ErrorCode::Internal, "not run");
    std::atomic<bool> done{false};
    std::jthread worker([&] {
        result = repo.repository().commit(
            CommitRequest{.paths = {repo.path("new.txt")}, .message = "must fail", .amend = false});
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!repo.exists("hook-started") && !done && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    const bool reached = repo.exists("hook-started");
    if (reached)
        repo.git({"update-ref", "HEAD", external, oldHead});
    repo.write("release-hook", "release\n");
    worker.join();
    ASSERT_TRUE(reached);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("provenance and index recovery are uncertain"), std::string::npos)
        << result.error().message();
    EXPECT_EQ(trimmed(repo.git({"rev-parse", "HEAD"})), external);
    EXPECT_EQ(trimmed(repo.git({"show", ":other.txt"})), "staged");
    EXPECT_EQ(repo.git({"ls-files", "--error-unmatch", "new.txt"}, 1).exitCode, 1);
    const auto recovery = TempRepo::join(TempRepo::join(repo.info().gitDir, "index.lock"), "frost-index");
    ASSERT_TRUE(repo.fs().exists(recovery).value());
    GitInvocation inspect;
    inspect.workingDirectory = repo.work();
    inspect.arguments = {"show", ":new.txt"};
    inspect.environment = {{"GIT_INDEX_FILE", recovery}};
    auto saved = repo.runner().run(inspect);
    ASSERT_TRUE(saved.has_value());
    ASSERT_TRUE(saved->succeeded()) << saved->stderrBytes;
    EXPECT_EQ(trimmed(*saved), "untracked");
}

TEST(GitE2E, CommitRetainsRecoveryIndexWhenPublicationFails) {
    TempRepo repo("vcs-index-publish-failure");
    repo.init();
    repo.write("a.txt", "old\n");
    repo.write("other.txt", "old other\n");
    repo.commitAll("base");
    repo.write("a.txt", "new\n");
    repo.write("other.txt", "staged other\n");
    repo.git({"add", "other.txt"});
    const auto index = TempRepo::join(repo.info().gitDir, "index");
    const auto recovery = TempRepo::join(TempRepo::join(repo.info().gitDir, "index.lock"), "frost-index");
    FailIndexPublication failFs(repo.fs(), index);
    GitRepository repository(repo.runner(), failFs, repo.info());
    auto result =
        repository.commit(CommitRequest{.paths = {repo.path("a.txt")}, .message = "committed", .amend = false});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("recovery index retained"), std::string::npos) << result.error().describe();
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:a.txt"})), "new");
    ASSERT_TRUE(repo.fs().exists(recovery).value());
    // Copying the retained file back under the reservation restores exactly the selected and
    // unrelated staged entries; this simulates explicit recovery after the reported failure.
    ASSERT_TRUE(repo.fs().writeFileAtomic(index, repo.fs().readFile(recovery).value()).has_value());
    EXPECT_EQ(trimmed(repo.git({"show", ":a.txt"})), "new");
    EXPECT_EQ(trimmed(repo.git({"show", ":other.txt"})), "staged other");
    EXPECT_EQ(trimmed(repo.git({"diff", "--cached", "--name-only"})), "other.txt");
}

TEST(GitE2E, CommitRetainsRecoveryIndexWhenReadingItFails) {
    TempRepo repo("vcs-index-read-failure");
    repo.init();
    repo.write("a.txt", "old\n");
    repo.commitAll("base");
    repo.write("a.txt", "new\n");
    const auto index = TempRepo::join(repo.info().gitDir, "index");
    const auto recovery = TempRepo::join(TempRepo::join(repo.info().gitDir, "index.lock"), "frost-index");
    FailIndexPublication failFs(repo.fs(), index, true);
    GitRepository repository(repo.runner(), failFs, repo.info());
    auto result =
        repository.commit(CommitRequest{.paths = {repo.path("a.txt")}, .message = "committed", .amend = false});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("reading its recovery index failed"), std::string::npos)
        << result.error().describe();
    EXPECT_EQ(trimmed(repo.git({"show", "HEAD:a.txt"})), "new");
    ASSERT_TRUE(repo.fs().exists(recovery).value());
    ASSERT_TRUE(repo.fs().writeFileAtomic(index, repo.fs().readFile(recovery).value()).has_value());
    EXPECT_EQ(trimmed(repo.git({"show", ":a.txt"})), "new");
}

TEST(GitE2E, RollbackRestoresModifiedAddedAndRenamedFiles) {
    TempRepo repo("vcs-rollback");
    repo.init();
    repo.write("modified.txt", "one\n");
    repo.write("staged.txt", "one\n");
    repo.write("old.txt", "renamed file content that is long enough\n");
    repo.write("deleted.txt", "one\n");
    repo.commitAll("base");
    repo.write("modified.txt", "two\n");
    repo.write("staged.txt", "two\n");
    repo.git({"add", "staged.txt"});
    repo.git({"mv", "old.txt", "new.txt"});
    repo.write("added.txt", "added\n");
    repo.git({"add", "added.txt"});
    ASSERT_TRUE(repo.fs().removeFile(repo.path("deleted.txt")).has_value());
    repo.write("untracked.txt", "keep me\n");

    const std::vector<std::string> paths = {repo.path("modified.txt"), repo.path("staged.txt"),
                                            repo.path("new.txt"),      repo.path("added.txt"),
                                            repo.path("deleted.txt"),  repo.path("untracked.txt")};
    auto rolled = repo.repository().rollback(paths, {});
    ASSERT_TRUE(rolled.has_value()) << rolled.error().describe();
    EXPECT_EQ(repo.read("modified.txt"), "one\n");
    EXPECT_EQ(repo.read("staged.txt"), "one\n");
    EXPECT_EQ(repo.read("deleted.txt"), "one\n");
    EXPECT_TRUE(repo.exists("old.txt"));
    EXPECT_FALSE(repo.exists("new.txt"));
    EXPECT_EQ(repo.read("untracked.txt"), "keep me\n");
    const GitStatus status = repo.status();
    // The added file is unversioned again (its content is kept); the untracked one is untouched.
    ASSERT_EQ(status.entries.size(), 2U);
    EXPECT_EQ(status.find("added.txt")->fileStatus(), VcsFileStatus::Untracked);
    EXPECT_EQ(status.find("untracked.txt")->fileStatus(), VcsFileStatus::Untracked);
}

TEST(GitE2E, BranchesCreateCheckoutAndARefusedCheckout) {
    TempRepo repo("vcs-branches");
    repo.init();
    repo.write("a.txt", "main\n");
    repo.commitAll("base");
    auto created = repo.repository().createBranch("feature/x", {});
    ASSERT_TRUE(created.has_value()) << created.error().describe();
    EXPECT_EQ(repo.status().branch.head, "feature/x");
    EXPECT_EQ(repo.repository().createBranch("feature/x", {}).error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(repo.repository().createBranch("bad..name", {}).error().code(), ErrorCode::InvalidArgument);
    repo.write("a.txt", "feature\n");
    repo.commitAll("feature");

    auto branches = repo.repository().branches();
    ASSERT_TRUE(branches.has_value()) << branches.error().describe();
    ASSERT_EQ(branches->size(), 2U);
    EXPECT_EQ((*branches)[0].name, "feature/x");
    EXPECT_TRUE((*branches)[0].current);
    EXPECT_EQ((*branches)[1].name, "main");
    EXPECT_FALSE((*branches)[1].remote);

    auto switched = repo.repository().checkout("main", {});
    ASSERT_TRUE(switched.has_value()) << switched.error().describe();
    EXPECT_EQ(repo.status().branch.head, "main");
    EXPECT_EQ(repo.read("a.txt"), "main\n");

    // A local change to a file that differs between the branches blocks the checkout.
    repo.write("a.txt", "local edit\n");
    auto refused = repo.repository().checkout("feature/x", {});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Busy);
    EXPECT_NE(refused.error().message().find("a.txt"), std::string::npos) << refused.error().message();
    EXPECT_NE(refused.error().message().find("refused"), std::string::npos) << refused.error().message();
    EXPECT_EQ(repo.status().branch.head, "main");
    EXPECT_EQ(repo.read("a.txt"), "local edit\n");
}

TEST(GitE2E, FetchPullAndPushAgainstALocalBareRepository) {
    TempRepo alice("vcs-remote");
    const std::string bare = TempRepo::join(alice.root(), "origin.git");
    alice.git({"init", "-q", "--bare", bare}, 0, alice.root());
    alice.init();
    alice.git({"remote", "add", "origin", bare});
    alice.write("a.txt", "one\n");
    alice.commitAll("first");

    // The first push has no upstream: it pushes to origin and sets one.
    std::vector<std::string> output;
    RunControl control;
    control.onLine = [&](GitStream stream, std::string_view line) {
        if (stream == GitStream::Command) {
            output.emplace_back(line);
        }
    };
    auto pushed = alice.repository().push({}, control);
    ASSERT_TRUE(pushed.has_value()) << pushed.error().describe();
    ASSERT_FALSE(output.empty());
    EXPECT_TRUE(output.back().starts_with("git push --set-upstream origin main")) << output.back();
    EXPECT_EQ(alice.status().branch.upstream, std::optional<std::string>("origin/main"));

    // Bob clones, commits and pushes; Alice fetches and pulls.
    TempRepo bob("vcs-remote-bob");
    const std::string bobWork = TempRepo::join(bob.root(), "clone");
    bob.git({"clone", "-q", bare, bobWork}, 0, bob.root());
    bob.open(bobWork);
    bob.write("b.txt", "from bob\n");
    bob.commitAll("bob");
    auto bobPushed = bob.repository().push({}, {});
    ASSERT_TRUE(bobPushed.has_value()) << bobPushed.error().describe();

    auto fetched = alice.repository().fetch({});
    ASSERT_TRUE(fetched.has_value()) << fetched.error().describe();
    EXPECT_EQ(alice.status().branch.behind, 1);
    auto branches = alice.repository().branches().value();
    EXPECT_TRUE(std::ranges::any_of(branches, [](const GitBranch& b) { return b.remote && b.name == "origin/main"; }));

    // Alice has a local commit too: her push is rejected with a clear message until she pulls.
    alice.write("a.txt", "two\n");
    alice.commitAll("alice");
    alice.git({"fetch", "-q"});
    bob.write("b.txt", "bob again\n");
    bob.commitAll("bob 2");
    ASSERT_TRUE(bob.repository().push({}, {}).has_value());
    auto rejected = alice.repository().push({}, {});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message().find("Update the project"), std::string::npos) << rejected.error().message();
    EXPECT_EQ(alice.repository().push(PushOptions{.forceWithLease = true, .confirmed = false}, {}).error().code(),
              ErrorCode::InvalidArgument);

    auto pulled = alice.repository().pull(PullStrategy::Merge, {});
    ASSERT_TRUE(pulled.has_value()) << pulled.error().describe();
    EXPECT_EQ(alice.read("b.txt"), "bob again\n");
    auto pushedAgain = alice.repository().push({}, {});
    ASSERT_TRUE(pushedAgain.has_value()) << pushedAgain.error().describe();
    EXPECT_EQ(trimmed(alice.git({"rev-parse", "HEAD"})), trimmed(alice.git({"rev-parse", "main"}, 0, bare)));

    // Pull with rebase on Bob's side.
    bob.write("c.txt", "bob local\n");
    bob.commitAll("bob 3");
    auto rebased = bob.repository().pull(PullStrategy::Rebase, {});
    ASSERT_TRUE(rebased.has_value()) << rebased.error().describe();
    EXPECT_EQ(trimmed(bob.git({"rev-list", "--merges", "--count", "HEAD", "^origin/main"})), "0");
    EXPECT_EQ(bob.read("a.txt"), "two\n");

    // A confirmed force push with lease goes through.
    bob.git({"commit", "-q", "--amend", "-m", "bob 3 reworded"});
    auto forced = bob.repository().push(PushOptions{.forceWithLease = true, .confirmed = true}, {});
    ASSERT_TRUE(forced.has_value()) << forced.error().describe();
    EXPECT_EQ(trimmed(bob.git({"log", "-1", "--format=%s", "main"}, 0, bare)), "bob 3 reworded");
}

TEST(GitE2E, RestrictedModeReadsButRefusesToRunHooks) {
    TempRepo setup("vcs-restricted", true);
    setup.init();
    setup.write("a.txt", "one\n");
    setup.commitAll("base");
    setup.write("a.txt", "two\n");
    GitRunnerOptions options;
    options.trusted = false;
    options.environment = {{"GIT_CONFIG_NOSYSTEM", "1"},
                           {"GIT_CONFIG_GLOBAL", TempRepo::join(setup.root(), "gitconfig")}};
    const GitRunner restricted(setup.runner().launcher(), setup.runner().gitPath(), options);
    const GitRepository repository(restricted, setup.fs(), setup.info());
    auto status = repository.status();
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_EQ(status->find("a.txt")->fileStatus(), VcsFileStatus::Modified);
    EXPECT_EQ(repository.headContent(setup.path("a.txt")).value(), std::optional<std::string>("one\n"));
    EXPECT_EQ(
        repository.commit(CommitRequest{.paths = {setup.path("a.txt")}, .message = "x", .amend = false}).error().code(),
        ErrorCode::Untrusted);
    EXPECT_EQ(repository.fetch({}).error().code(), ErrorCode::Untrusted);
}

TEST(GitE2E, RestrictedStatusNeverRunsCleanOrProcessFiltersInNestedPaths) {
    TempRepo setup("vcs-restricted-filters", true);
    setup.init();
    setup.write("nested/.gitattributes", "*.txt filter=review\n");
    setup.write("nested/file.txt", "one\n");
    setup.commitAll("base");
    setup.write("nested/file.txt", "two\n"); // Same-size change triggers the ordinary status filter.
    setup.git({"config", "filter.review.clean", "echo clean > clean-ran; cat"});
    setup.git({"config", "filter.review.process", "echo process > process-ran; exit 1"});
    GitRunnerOptions options;
    options.trusted = false;
    options.environment = {{"GIT_CONFIG_NOSYSTEM", "1"},
                           {"GIT_CONFIG_GLOBAL", TempRepo::join(setup.root(), "gitconfig")}};
    const GitRunner restricted(setup.runner().launcher(), setup.runner().gitPath(), options);
    const GitRepository repository(restricted, setup.fs(), setup.info());
    auto status = repository.status();
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_TRUE(status->restrictedRawComparison);
    ASSERT_NE(status->find("nested/file.txt"), nullptr);
    EXPECT_EQ(status->find("nested/file.txt")->fileStatus(), VcsFileStatus::Modified);
    EXPECT_FALSE(setup.exists("process-ran"));
    EXPECT_FALSE(setup.exists("clean-ran"));
    setup.git({"config", "--unset", "filter.review.process"});
    status = repository.status();
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_FALSE(setup.exists("clean-ran"));
    setup.git({"status", "--porcelain"}); // Positive control: ordinary status invokes the configured filter.
    EXPECT_TRUE(setup.exists("clean-ran"));
}

TEST(GitE2E, RestrictedStatusDoesNotRunFiltersInAnEnclosedRepository) {
    TempRepo setup("vcs-restricted-nested-repo", true);
    setup.init();
    setup.git({"init", "-q", "nested"});
    setup.git({"-C", "nested", "config", "user.name", "Frost Test"});
    setup.git({"-C", "nested", "config", "user.email", "frost@example.invalid"});
    setup.write("nested/.gitattributes", "*.txt filter=review\n");
    setup.write("nested/file.txt", "one\n");
    setup.git({"-C", "nested", "add", "."});
    setup.git({"-C", "nested", "commit", "-qm", "initial"});
    setup.write("nested/file.txt", "two\n");
    setup.git({"-C", "nested", "config", "filter.review.clean", "echo clean > clean-ran; cat"});
    setup.git({"-C", "nested", "config", "filter.review.process", "echo process > process-ran; exit 1"});
    GitRunnerOptions options;
    options.trusted = false;
    options.environment = {{"GIT_CONFIG_NOSYSTEM", "1"},
                           {"GIT_CONFIG_GLOBAL", TempRepo::join(setup.root(), "gitconfig")}};
    const GitRunner restricted(setup.runner().launcher(), setup.runner().gitPath(), options);
    auto info = discoverRepository(restricted, setup.path("nested/file.txt"));
    ASSERT_TRUE(info.has_value()) << info.error().describe();
    ASSERT_TRUE(info->has_value());
    GitRepository nested(restricted, setup.fs(), **info);
    auto status = nested.status();
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    ASSERT_NE(status->find("file.txt"), nullptr);
    EXPECT_EQ(status->find("file.txt")->fileStatus(), VcsFileStatus::Modified);
    EXPECT_FALSE(setup.exists("nested/clean-ran"));
    EXPECT_FALSE(setup.exists("nested/process-ran"));
    setup.git({"-C", "nested", "config", "--unset", "filter.review.process"});
    setup.git({"-C", "nested", "status", "--porcelain"});
    EXPECT_TRUE(setup.exists("nested/clean-ran"));
}

// `git diff --no-index --histogram -U0 a b`'s hunk headers as Hunks.
std::vector<frost::diff::Hunk> gitHunks(const TempRepo& repo, const std::string& a, const std::string& b) {
    const GitOutput output = repo.git({"diff", "--no-index", "--histogram", "-U0", "--no-color", a, b}, 1, repo.root());
    std::vector<frost::diff::Hunk> hunks;
    std::string_view rest = output.stdoutBytes;
    const auto number = [](std::string_view& text) {
        std::uint32_t value = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        EXPECT_EQ(ec, std::errc());
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        return value;
    };
    // "-start[,count]": a count of 0 means the range is empty *after* line `start`.
    const auto range = [&](std::string_view& text) {
        text.remove_prefix(1);
        const std::uint32_t start = number(text);
        std::uint32_t count = 1;
        if (!text.empty() && text.front() == ',') {
            text.remove_prefix(1);
            count = number(text);
        }
        const std::uint32_t begin = count == 0 ? start : start - 1;
        return frost::diff::LineRange{LineIndex{begin}, LineIndex{begin + count}};
    };
    for (std::size_t at = rest.find("\n@@ "); at != std::string_view::npos; at = rest.find("\n@@ ")) {
        rest.remove_prefix(at + 4);
        frost::diff::Hunk hunk;
        hunk.a = range(rest);
        rest.remove_prefix(1);
        hunk.b = range(rest);
        hunks.push_back(hunk);
    }
    return hunks;
}

std::uint64_t changedLines(const std::vector<frost::diff::Hunk>& hunks) {
    std::uint64_t total = 0;
    for (const frost::diff::Hunk& hunk : hunks) {
        total += hunk.a.size() + hunk.b.size();
    }
    return total;
}

/*
 * frost::diff gives the classic histogram dialect (ADR 0019, D6): on a classic git every fixture's
 * hunks are git's exactly. git 2.54 changed its histogram output (the new dialect, which the
 * canary probe of tests/diff detects); there a pair is exact or git changes fewer lines (the D6
 * rule; matching the new dialect waits for ADR 0019's question Q3), and an equal-cost difference
 * still fails. The "code" pair is one where the dialects differ.
 */
TEST(GitE2E, SideBySideMatchesGitDiffHistogramOnTheFixtures) {
    TempRepo repo("vcs-fixtures");
    frost::diff::test::GitRunner probe(frost::io::realProcessLauncher(), repo.runner().gitPath(), repo.root());
    ASSERT_TRUE(probe.prepare().has_value());
    auto canaries = frost::diff::test::loadDialectGoldens(frost::diff::test::testDataPath("goldens/histogram/dialect"));
    ASSERT_TRUE(canaries.has_value()) << canaries.error().describe();
    std::string report;
    auto dialect = probe.probeDialect(*canaries, report);
    ASSERT_TRUE(dialect.has_value()) << dialect.error().describe() << '\n' << report;
    std::size_t gitFewer = 0;
    for (const std::string name : {"simple", "moved", "code", "noeol", "crlf", "deleted"}) {
        SCOPED_TRACE(name);
        const std::string a = fixturePath("diff/" + name + ".a");
        const std::string b = fixturePath("diff/" + name + ".b");
        auto left = repo.fs().readFile(a);
        auto right = repo.fs().readFile(b);
        ASSERT_TRUE(left.has_value() && right.has_value()) << a;
        auto diff = SideBySideDiff::compute(*left, *right);
        ASSERT_TRUE(diff.has_value()) << diff.error().describe();
        const std::vector<frost::diff::Hunk> expected = gitHunks(repo, a, b);
        ASSERT_FALSE(expected.empty());
        if (*dialect == frost::diff::test::GitDialect::Classic || diff->hunks() == expected) {
            EXPECT_EQ(diff->hunks(), expected);
        } else {
            EXPECT_LT(changedLines(expected), changedLines(diff->hunks())) << "an equal-cost dialect difference";
            ++gitFewer;
        }
    }
    EXPECT_LE(gitFewer, 1U) << "only the code pair differs on the new dialect";
}

} // namespace
