// SPDX-License-Identifier: Apache-2.0
//
// GitRepository (M5, ADR 0048): the synchronous git operations on one work tree that the service
// and the UI build on: discovery, status, the HEAD and index versions of a file, commit of selected
// files, rollback, branches, checkout, and fetch, pull and push. Every call runs git through a
// GitRunner and blocks the calling thread; GitService runs them on its own threads.
// Each call owns its cancellation snapshot across reentrant Git/launcher callbacks.
/*
 * Discovery (discoverRepository). `git rev-parse --show-toplevel --absolute-git-dir` in the opened
 * folder (or a solution file's directory): the enclosing work tree, whatever depth the folder has.
 * Submodules are listed from .gitmodules (their paths) but never descended: status runs with
 * --ignore-submodules=dirty, so a submodule shows as one entry when its commit changes.
 *
 * File versions (headContent, indexContent): `git cat-file --filters HEAD:<path>` (or :<path>), so
 * the bytes are what a checkout would write (line endings, smudge filters), comparable with the
 * working file. Restricted mode reads the raw blob instead, because filters are commands from the
 * repository's configuration. TODO(M5): a persistent `cat-file --batch` for many files.
 *
 * Commit (commit). Only the selected paths are committed, with `git commit --only -F <file> --
 * <paths>`: other files keep their index state, staged or not. Selected untracked files are added
 * first (git add); when the commit fails (a pre-commit hook said no) they are unstaged again, so
 * the index is as it was. The message goes through a file in the git directory (no quoting, any
 * length); hook output comes back in the result or the error. Amend without paths changes only the
 * message (and takes nothing else that is staged).
 *
 * Rollback (rollback). Modified, deleted and conflicted files: `git restore --source=HEAD --staged
 * --worktree`. Added files: `git rm --cached` (the file stays, unversioned). A rename: the source
 * is restored and the new path removed. Untracked files are left alone (Rider does the same).
 *
 * Mutating operations (commit, rollback, checkout, createBranch, pull, push, fetch) need a
 * trusted runner (Untrusted otherwise): they can run the repository's hooks.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/vcs/git_runner.hpp"
#include "frost/vcs/git_status.hpp"

#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::vcs {

// One work tree.
struct RepositoryInfo {
    std::string workTree;                // Absolute, normalised (native separators).
    std::string gitDir;                  // Absolute, normalised; for a linked worktree or submodule, its own git dir.
    std::vector<std::string> submodules; // Absolute paths of the submodules .gitmodules names.

    friend bool operator==(const RepositoryInfo&, const RepositoryInfo&) = default;
};

/*
 * The work tree that contains `path` (an absolute directory, or a file whose directory is used, such
 * as a .sln). nullopt when it is in no work tree. Errors: those of GitRunner::run, IoError when git
 * fails for another reason.
 */
[[nodiscard]] Result<std::optional<RepositoryInfo>> discoverRepository(const GitRunner& runner, std::string_view path,
                                                                       std::stop_token stop = {});

// A local or remote-tracking branch (for-each-ref refs/heads and refs/remotes; */HEAD is skipped).
struct GitBranch {
    std::string name;     // "main", "origin/main".
    std::string fullName; // "refs/heads/main".
    bool remote = false;
    bool current = false; // HEAD points at it.
    std::string upstream; // Local branches: "origin/main" or "".
    std::string commit;   // The abbreviated commit id.

    friend bool operator==(const GitBranch&, const GitBranch&) = default;
};

struct GitCommitEntry {
    std::string hash;
    std::string shortHash;
    std::vector<std::string> parents;
    std::string author;
    std::string email;
    std::string dateIso;
    std::string subject;
    std::vector<std::string> refs;

    friend bool operator==(const GitCommitEntry&, const GitCommitEntry&) = default;
};

struct GitCommitFileChange {
    std::string status;
    std::string path;

    friend bool operator==(const GitCommitFileChange&, const GitCommitFileChange&) = default;
};

struct GitStashEntry {
    int index = 0;
    std::string ref;
    std::string message;

    friend bool operator==(const GitStashEntry&, const GitStashEntry&) = default;
};

struct GitBlameLine {
    int line = 0;
    std::string shortHash;
    std::string author;
    std::string dateShort;
    std::string summary;

    friend bool operator==(const GitBlameLine&, const GitBlameLine&) = default;
};

struct CommitRequest {
    std::vector<std::string> paths; // Absolute paths of the selected files (a rename: either path).
    std::string message;
    bool amend = false;
};

struct CommitResult {
    std::string commit; // The new HEAD's full id.
    std::string output; // git's stdout and stderr (hook output included).
};

enum class PullStrategy : std::uint8_t { Merge, Rebase };

struct PushOptions {
    // --force-with-lease. Refused unless `confirmed` is set too: the UI asks first.
    bool forceWithLease = false;
    bool confirmed = false;
};

// Options shared by the long-running calls.
struct RunControl {
    std::stop_token stop;
    GitLineSink onLine; // Streams git's output (the 'git' console).
};

// The files a refused checkout names ("would be overwritten by checkout", tab-indented); empty
// when `stderrText` is no such refusal. Pure.
[[nodiscard]] std::vector<std::string> checkoutBlockers(std::string_view stderrText);

class GitRepository {
public:
    // The runner and the file system must outlive the repository object.
    GitRepository(const GitRunner& runner, io::IFileSystem& fs, RepositoryInfo info);

    [[nodiscard]] const RepositoryInfo& info() const noexcept { return m_info; }
    [[nodiscard]] const GitRunner& runner() const noexcept { return *m_runner; }

    // The '/' path of `absolutePath` relative to the work tree. Errors: InvalidArgument (outside it).
    [[nodiscard]] Result<std::string> relativePath(std::string_view absolutePath) const;
    [[nodiscard]] std::string absolutePath(std::string_view relativePath) const;

    // `git status` (statusArguments), as a background command. Errors: run()'s, IoError, Corrupt.
    [[nodiscard]] Result<GitStatus> status(std::stop_token stop = {}) const;

    // The file's bytes in HEAD (the index), filtered as a checkout writes them; nullopt when HEAD (the
    // index) has no such file or there is no commit yet. Errors: InvalidArgument (outside), run()'s, IoError.
    [[nodiscard]] Result<std::optional<std::string>> headContent(std::string_view absolutePath,
                                                                 std::stop_token stop = {}) const;
    [[nodiscard]] Result<std::optional<std::string>> indexContent(std::string_view absolutePath,
                                                                  std::stop_token stop = {}) const;

    // HEAD's full commit message; "" without commits. Errors: run()'s, IoError.
    [[nodiscard]] Result<std::string> lastCommitMessage(std::stop_token stop = {}) const;
    // The messages of the last `count` commits of HEAD, newest first. Errors: run()'s, IoError.
    [[nodiscard]] Result<std::vector<std::string>> recentCommitMessages(std::size_t count,
                                                                        std::stop_token stop = {}) const;

    /*
     * Commits the selected files (header comment). Errors: Untrusted; InvalidArgument (an empty
     * message, no paths without amend, a path outside the work tree); IoError with git's output when
     * git (or a hook) fails; run()'s.
     */
    [[nodiscard]] Result<CommitResult> commit(const CommitRequest& request, const RunControl& control = {}) const;

    // Rolls the files back (header comment). Errors: Untrusted, InvalidArgument (outside), IoError, run()'s.
    [[nodiscard]] Result<void> rollback(std::span<const std::string> absolutePaths,
                                        const RunControl& control = {}) const;

    // Local branches first (by name), then remote ones. Errors: run()'s, IoError.
    [[nodiscard]] Result<std::vector<GitBranch>> branches(std::stop_token stop = {}) const;

    /*
     * Checks a branch out: a local one with `git switch`, a remote one ("origin/x") as a new local
     * tracking branch "x" (or the existing local "x"). Errors: Untrusted; Busy when the work tree's
     * changes block it (the message names the files; checkoutBlockers); IoError; run()'s.
     */
    [[nodiscard]] Result<void> checkout(std::string_view branch, const RunControl& control = {}) const;
    // Creates `name` from HEAD and checks it out. Errors: Untrusted, InvalidArgument (not a valid
    // branch name), AlreadyExists, IoError, run()'s.
    [[nodiscard]] Result<void> createBranch(std::string_view name, const RunControl& control = {}) const;

    // `git fetch --all --prune`. Errors: Untrusted, IoError (gitFailure's message), run()'s.
    [[nodiscard]] Result<GitOutput> fetch(const RunControl& control = {}) const;
    // `git pull --no-edit --no-rebase` or `--rebase`. Errors: as fetch.
    [[nodiscard]] Result<GitOutput> pull(PullStrategy strategy, const RunControl& control = {}) const;
    /*
     * Pushes the current branch to its upstream (`git push`), or, without one, to "origin" (or the
     * only remote) with --set-upstream. Errors: Untrusted; InvalidArgument (force without
     * confirmation, a detached HEAD, no remote); IoError; run()'s.
     */
    [[nodiscard]] Result<GitOutput> push(const PushOptions& options, const RunControl& control = {}) const;

    // Commit history (`git log --topo-order --decorate=short ...`) and per-commit file list (`git show --name-status`).
    [[nodiscard]] Result<std::vector<GitCommitEntry>> log(std::size_t maxCount = 300,
                                                          std::string_view branchFilter = {},
                                                          std::string_view searchFilter = {},
                                                          std::stop_token stop = {}) const;
    [[nodiscard]] Result<std::vector<GitCommitFileChange>> commitFiles(std::string_view commitHash,
                                                                       std::stop_token stop = {}) const;

    // History operations (cherry-pick, revert, reset, tag, branch at revision).
    [[nodiscard]] Result<GitOutput> cherryPick(std::string_view commitHash, const RunControl& control = {}) const;
    [[nodiscard]] Result<GitOutput> revertCommit(std::string_view commitHash, const RunControl& control = {}) const;
    [[nodiscard]] Result<GitOutput> resetBranch(std::string_view commitHash, std::string_view mode = "mixed",
                                                const RunControl& control = {}) const;
    [[nodiscard]] Result<void> createTag(std::string_view tagName, std::string_view commitHash = {},
                                         const RunControl& control = {}) const;
    [[nodiscard]] Result<void> createBranchAt(std::string_view name, std::string_view startPoint,
                                              const RunControl& control = {}) const;

    // Branch operations (merge, rebase, delete, rename).
    [[nodiscard]] Result<GitOutput> mergeBranch(std::string_view branch, const RunControl& control = {}) const;
    [[nodiscard]] Result<GitOutput> rebaseBranch(std::string_view ontoBranch, const RunControl& control = {}) const;
    [[nodiscard]] Result<void> deleteBranch(std::string_view branch, bool force = false,
                                            const RunControl& control = {}) const;
    [[nodiscard]] Result<void> renameBranch(std::string_view oldName, std::string_view newName,
                                            const RunControl& control = {}) const;

    // Stash operations (`git stash push|list|pop|apply|drop`).
    [[nodiscard]] Result<GitOutput> stashPush(std::string_view message = {}, bool includeUntracked = true,
                                              const RunControl& control = {}) const;
    [[nodiscard]] Result<std::vector<GitStashEntry>> stashList(std::stop_token stop = {}) const;
    [[nodiscard]] Result<GitOutput> stashApply(int index = 0, bool pop = true, const RunControl& control = {}) const;
    [[nodiscard]] Result<void> stashDrop(int index = 0, const RunControl& control = {}) const;

    // Line-by-line blame (`git blame --porcelain <path>`).
    [[nodiscard]] Result<std::vector<GitBlameLine>> blame(std::string_view relativePath,
                                                          std::stop_token stop = {}) const;

private:
    [[nodiscard]] Result<GitOutput> git(std::vector<std::string> arguments, const RunControl& control,
                                        IClock::Duration timeout, bool background = false, bool requiresTrust = false,
                                        std::string_view indexFile = {}) const;
    [[nodiscard]] Result<void> requireTrust(std::string_view what) const;
    [[nodiscard]] Result<GitStatus> restrictedStatus(std::stop_token stop) const;
    [[nodiscard]] Result<std::optional<std::string>> fileContent(std::string_view absolutePath, std::string_view rev,
                                                                 std::stop_token stop) const;

    const GitRunner* m_runner;
    io::IFileSystem* m_fs;
    RepositoryInfo m_info;
};

} // namespace frost::vcs
