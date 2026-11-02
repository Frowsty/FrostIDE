// SPDX-License-Identifier: Apache-2.0
//
// git status (M5, ADR 0048): the parser of `git status --porcelain=v2 -z --branch` and the
// per-file status the UI colours from (VcsFileStatus).
/*
 * Format (git-status(1), "Porcelain Format Version 2"), with -z every record ends in NUL:
 *   # branch.oid <commit> | (initial)        # branch.head <branch> | (detached)
 *   # branch.upstream <upstream>             # branch.ab +<ahead> -<behind>
 *   1 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <path>                       ordinary change
 *   2 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <X><score> <path> NUL <orig>  rename or copy
 *   u <XY> <sub> <m1> <m2> <m3> <mW> <h1> <h2> <h3> <path>              unmerged (conflict)
 *   ? <path>                                 ! <path>
 * X is the index's state against HEAD and Y the work tree's against the index; '.' means
 * unchanged. Paths are relative to the work tree's top, with '/', as git wrote the bytes. Other
 * header lines (# stash) are skipped, so newer gits stay readable.
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::vcs {

// What colour a file gets (IntelliJ's file status colours; theme keys Vcs.*).
enum class VcsFileStatus : std::uint8_t {
    Unmodified,
    Added,      // New in the index (staged).
    Modified,   // Content or type changed, staged or not.
    Deleted,    // Deleted in the index or the work tree.
    Renamed,    // Renamed or copied in the index.
    Untracked,  // Not known to git (an "Unversioned File").
    Conflicted, // Unmerged.
    Ignored,    // Ignored (only when status was asked for ignored files).
};

// "unmodified", "added", "modified", "deleted", "renamed", "untracked", "conflicted", "ignored".
[[nodiscard]] std::string_view toString(VcsFileStatus status) noexcept;

enum class GitEntryKind : std::uint8_t { Ordinary, RenamedOrCopied, Unmerged, Untracked, Ignored };

// One status record.
struct GitStatusEntry {
    GitEntryKind kind = GitEntryKind::Ordinary;
    char index = '.';         // X: the index against HEAD ('.', 'M', 'T', 'A', 'D', 'R', 'C', 'U').
    char worktree = '.';      // Y: the work tree against the index.
    std::string path;         // Relative to the work tree's top, '/' separated.
    std::string originalPath; // RenamedOrCopied: the source path; "" otherwise.
    bool submodule = false;   // <sub> starts with 'S'.

    // The colour this entry gives its file (VcsFileStatus).
    [[nodiscard]] VcsFileStatus fileStatus() const noexcept;
    // True when the index differs from HEAD for this path (X != '.'); false for untracked files.
    [[nodiscard]] bool staged() const noexcept;
    // True when the work tree differs from the index (Y != '.'), or for untracked files.
    [[nodiscard]] bool unstaged() const noexcept;

    friend bool operator==(const GitStatusEntry&, const GitStatusEntry&) = default;
};

// The # branch.* headers.
struct GitBranchStatus {
    std::string oid;                     // "" for (initial): a branch without commits.
    std::string head;                    // The branch name; "" when detached.
    std::optional<std::string> upstream; // "origin/main".
    int ahead = 0;
    int behind = 0;

    [[nodiscard]] bool detached() const noexcept { return head.empty(); }
    [[nodiscard]] bool unborn() const noexcept { return oid.empty(); }

    friend bool operator==(const GitBranchStatus&, const GitBranchStatus&) = default;
};

struct GitStatus {
    // Restricted mode compares raw bytes without project-configured filters; see ADR 0067.
    bool restrictedRawComparison = false;
    GitBranchStatus branch;
    std::vector<GitStatusEntry> entries; // In git's order.

    // The entry of `relativePath` (its path, or its originalPath for a rename), or nullptr. O(n).
    [[nodiscard]] const GitStatusEntry* find(std::string_view relativePath) const noexcept;

    friend bool operator==(const GitStatus&, const GitStatus&) = default;
};

// Parses `git status --porcelain=v2 -z --branch` output. Errors: Corrupt (a record that does not
// have its fields, an unknown record type, a rename without its source path). O(bytes).
[[nodiscard]] Result<GitStatus> parsePorcelainV2(std::string_view output);

// The arguments of the status command this module runs: porcelain v2, -z, --branch, every
// untracked file listed (-uall), ignored files off, submodules not descended (dirty ignored).
[[nodiscard]] std::vector<std::string> statusArguments();

} // namespace frost::vcs
