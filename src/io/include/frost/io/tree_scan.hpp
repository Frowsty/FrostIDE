// SPDX-License-Identifier: Apache-2.0
//
// scanTree: the gitignore-aware parallel directory walk (PLAN.md section 3.3, "gitignore-aware parallel
// scan"; ADR 0020). It fills the project trees on a first open (Budget: B23) and is the polling file
// watcher's snapshot walk.
//
/*
 * Model. The walk lists directories through IFileSystem::listDirectoryWithDiagnostics as tasks spawned
 * on `scheduler` (ScanOptions::lane), at most ScanOptions::parallelism at a time, and joins them in the
 * order they were started. Each directory's ".gitignore" (a regular file; a symlinked one is not read,
 * as git does not follow those) is parsed and pushed on that directory's IgnoreStack, so it applies to
 * the directory's entries and everything below. An ignored directory is never descended. Directory
 * symlinks are reported as Symlink entries and never followed.
 *
 * Problems that do not end the walk are collected as ScanIssues: a directory that could not be listed
 * (NotFound: it vanished after its parent was listed), names the platform cannot represent, a
 * .gitignore that could not be read, and patterns that can never match.
 *
 * Result. Entries are sorted by path (byte-wise); paths are absolute in the file system's native
 * syntax. The root itself is not an entry.
 *
 * Thread safety: scanTree may run concurrently with itself; `fs`, `scheduler` and every reference in
 * ScanOptions must outlive the returned task. Callbacks (ScanOptions::descend) run on scheduler threads,
 * possibly concurrently. Complexity: O(entries x matching rules) plus one listing per directory.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/ignore_rules.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <cstddef>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {

struct ScanOptions {
    // Rules below the root's own .gitignore in precedence (info/exclude, core.excludesFile), with paths
    // relative to the root. Empty by default.
    IgnoreStack ignore{};
    bool readGitignore = true; // Read and apply each directory's ".gitignore".
    // More ignore files read in each directory after ".gitignore", in order, each taking precedence over
    // the ones before it (ripgrep's ".ignore" over ".gitignore"; the File System view passes {".ignore"}).
    // Read whatever readGitignore says; the same rules as for ".gitignore" (a symlinked one is not read).
    std::vector<std::string> extraIgnoreFiles{};
    IgnoreOptions ignoreOptions{}; // For the .gitignore files read (case folding).
    // Keep ignored entries (ScanEntry::ignored) instead of dropping them; they are still not descended.
    bool reportIgnored = false;
    // Entry names that are neither reported nor descended, at any depth (exact, case-sensitive).
    std::vector<std::string> skipNames{".git"};
    // Names containing writeFileAtomic's temp marker (".frost-tmp-") are skipped like skipNames: they are
    // the transient halves of atomic saves (IFileWatcher's reportFrostTempFiles = false matches this).
    bool skipFrostTempFiles = true;
    bool recursive = true; // False: only the root's own entries.
    // If set, a directory below the root is descended only when this returns true for its path (it is
    // reported either way). Called on scheduler threads, possibly concurrently.
    std::function<bool(std::string_view directory)> descend{};
    // Fill ScanEntry::status with status() of every regular file and every descended directory (taken
    // before the directory is listed); an entry whose status fails is dropped (it vanished).
    bool withStatus = false;
    unsigned parallelism = 4; // Listings in flight; at least 1.
    sched::Lane lane = sched::Lane::Background;
};

struct ScanEntry {
    std::string path;     // Absolute, native syntax.
    std::string relative; // Relative to the root, '/'-separated (what the ignore rules saw).
    FileType type = FileType::Regular;
    bool ignored = false; // Only with ScanOptions::reportIgnored.
    // status.type is always the entry's type; the rest is filled only with ScanOptions::withStatus, for
    // regular files and descended directories (else default).
    FileStatus status{};

    friend bool operator==(const ScanEntry&, const ScanEntry&) = default;
};

enum class ScanIssueKind : unsigned char {
    ListFailed,       // `path` is a directory that could not be listed (its subtree is missing).
    NameSkipped,      // listDirectoryWithDiagnostics reported a name (DirListingIssue); `path` is the directory.
    IgnoreFileFailed, // `path` is a .gitignore that could not be read; the walk continues without it.
    IgnoreRule,       // `path` is a .gitignore with a pattern that never matches.
};

struct ScanIssue {
    ScanIssueKind kind = ScanIssueKind::ListFailed;
    std::string path;
    Error error;
};

struct TreeScan {
    std::vector<ScanEntry> entries;
    std::vector<ScanIssue> issues;
    std::size_t directoriesListed = 0; // The root included.
};

/*
 * FrostIDE's default project excludes (ADR 0042): build output directories that a project tree
 * hides unless an ignore file re-includes them ("!build/"). Meant for the bottom of
 * ScanOptions::ignore (below every .gitignore), with paths relative to the walk root. Directory-only
 * patterns at any depth: build/, out/, bin/, obj/, ipch/, cmake-build-*, [Dd]ebug/, [Rr]elease/,
 * x64/, x86/, ARM64/ (the Visual Studio and CMake output layouts). O(1) after the first call.
 */
[[nodiscard]] const IgnoreRules& defaultProjectExcludes();

// The names a project tree never shows nor descends at any depth (ScanOptions::skipNames for the File
// System view): version-control and IDE state directories, ".git", ".vs", ".svn" and ".hg".
[[nodiscard]] std::vector<std::string> defaultProjectSkipNames();

/*
 * Walks the tree below `root` (absolute) as described in the header comment. Errors: InvalidArgument (a
 * relative root, parallelism 0), Cancelled (`token`), and the root's own listing error (NotFound,
 * InvalidArgument for a non-directory, PermissionDenied, IoError). Resumes on options.lane. Budget: B23
 * (the File System view of a first open).
 */
[[nodiscard]] sched::Task<Result<TreeScan>> scanTree(IFileSystem& fs, sched::IScheduler& scheduler, std::string root,
                                                     ScanOptions options, std::stop_token token = {});

} // namespace frost::io
