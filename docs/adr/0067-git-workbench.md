<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0067: Git in the workbench

- Status: accepted implementation slice (feature-first phase, ADR 0031; 2026-11-03)
- Date: 2026-11-02
- Plan reference: M5 Git, U1; ADR 0048

## Context

The Git CLI service and its widgets existed independently of the application workbench. The
composition must preserve unsaved editors, honor repository trust, and end child processes when the
project changes or the application shuts down.

## Decision

The Workbench owns a project-scoped coordinator. It finds the enclosing repository off the GUI
thread using restricted Git, excludes the project and discovered worktree from executable lookup,
and keeps the user's normal Git configuration in trusted mode. It registers a Git menu and actions,
Commit, diff and console views, a branch widget, status colors and per-editor change markers.
HEAD and index comparisons use the open buffer when present and decode file bytes through the
normal document loader. Dirty-only documents also appear in the Commit list.

A selected dirty document is saved before a commit; a save failure prevents that commit. Rollback
refuses selected dirty documents, and checkout and pull refuse while the repository has dirty open
documents. All completed disk operations, including cancelled ones, recheck open documents through the
existing external-change machinery. Operations are serialized through delivery of their result.
While rollback, checkout or pull is in flight, editing cancels it and saving or closed-file
rewrites inside the worktree wait until it finishes. A marker popup cannot roll back a hunk after
its document or base changed.

Trust covers the open project and its enclosing Git worktree. A user can review the worktree in the
explicit Trust Git Repository action. Controller operations and mutating process launches recheck
live trust; trust revocation, project switch and shutdown cancel and join the old controller. Git
reads accept cancellation as well. Shell actions resolve the active controller at invocation time,
not a controller from a previous project. Inherited GIT_DIR and related repository-redirection
variables are cleared; ordinary system, user and repository Git configuration remains enabled.
Before trust, ordinary Git status is unsafe because clean and process filters can execute project
commands. Restricted status instead enumerates the index and HEAD and compares raw file hashes with
`hash-object --no-filters`; it prevents lazy fetch. The Commit view identifies this conservative
comparison: CRLF or filtered files, symlinks and submodules can appear changed, and staged renames
can appear as a deletion and an addition. Trusted status uses Git porcelain and normal filters.

A commit reserves the real index lock and uses an isolated index inside the reservation while hooks
run. The private index receives a conservative modification time after copying and after any
intermediate Git add: advancing the index timestamp would otherwise let Git mistake a same-size,
same-time worktree edit for unchanged. Publication uses a timestamped atomic filesystem write,
setting that time before replacement so readers never observe new bytes with a newer timestamp.
If the Git command fails or is interrupted and HEAD moved, a concurrent `git update-ref` means the
operation cannot establish who moved it. The real index is left unchanged and the private index,
any pending lock and reservation are retained with their recovery paths; no automatic push occurs.
A successful Git command publishes its private index. If publication or outcome checks fail, the
recovery artifacts remain for inspection. On Windows, timestamped replacement names a backup so
both [documented exceptional ReplaceFileW failures](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew) report the original and replacement paths.
After confirming no Git writer is active, the user can inspect HEAD and the retained files before
recovering the index and removing the reservation. A process crash can also leave this reservation,
as with Git's own index lock.

The existing project watcher delivers events to the Git service. A 1.5-second background status
refresh covers a repository above the project and linked-worktree Git directories outside that
watch. It runs no remote operation. The status engine treats submodules as entries and does not
recurse into them. This polling is a functional fallback, not an implementation of a shared-refs
watcher or a performance-budget claim.

## Validation

The application acceptance tests use isolated temporary repositories and a local bare remote.
They cover dirty-buffer comparisons and selected commits, late edits, hook failures, trust changes,
project switches, a cancelled commit, external refresh and branch/push behavior. Component tests
cover Git parsing, read restrictions, marker rollback and Git CLI operations. Astra independently reviewed
the slice and its recovery corrections; combined Windows and native Linux functional tests pass. Broader
M5 workflows and deferred hardening remain required; see the checkpoint evidence in the progress log.
