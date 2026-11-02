<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0028: SaveManager, safe writes and the document load service

- Status: accepted for `m1/save-manager` (feature-first phase, ADR 0031). No budget, test or warning is weakened.
- Date: 2026-11-02
- Plan reference: PLAN.md section 2.2 "Data safety" and feature row I2; ADRs 0014 (hot-exit journal, local
  history), 0017 decision 14 (`UndoState::stateId`), 0018 (encodings, save consent), 0019 (merge model; D1:
  a target linking storage and diff needs rank 5 or more), 0021 (save.* and localHistory.* keys), 0029
  decision 7 (the large-file guard), 0032 (W1: saving Visual Studio sources must be byte-exact). No new
  dependency.

## Decision
1. **Module.** `src/document` (frost::document, new layer `document`, rank 6), Qt-free. It links io, sched, text,
   diff, storage (rank 4) and settings (rank 5), so it cannot live in `src/text` as PLAN 3.2 sketched. Fuzz trees
   leave it out (it links storage and settings). The shell owns one `SaveManager` on its document thread.
2. **Safe writes.** `io::IFileSystem::safeWrite` is the save primitive; `writeFileAtomic` keeps its behaviour for
   settings and storage. Windows: temp file in the target's directory, flushed, then `ReplaceFileW` with flags 0
   (keeps attributes, ACL, alternate streams and creation time; strict ACL merge), retried three times on sharing
   errors, then `Busy`. POSIX: `writeFileAtomic` (owner, mode and ACL copied, `rename`). Both refuse a read-only
   target with `PermissionDenied` unless `overwriteReadOnly`, which makes the file writable first and restores the
   attribute if the write fails. A symlink is resolved and its target is written; the link stays. Every failure
   leaves the old file intact and removes the temp file, with one exception: when `ReplaceFileW` has already
   removed the target (`ERROR_UNABLE_TO_MOVE_REPLACEMENT`), the temp file is moved into place, and if that also
   fails it is kept and named in the error, since it then holds the only copy.
3. **Hard links** are rewritten in place (space reserved first, then write, truncate, flush), because a rename would
   split the link. A crash during that rewrite can tear the file; the hot-exit journal still holds the buffer.
   A backup copy before the rewrite is deferred to hardening.
4. **Stale temp files.** `.<name>.frost-tmp-<pid>-<n>` names are parsed by `parseAtomicTempName`;
   `removeStaleTempFiles` deletes other processes' temp files older than a cut-off (1 h by default). The SaveManager
   sweeps each directory once per session, at the first open or save there. This closes the two TODO(M1)s at
   `writeFileAtomic`.
5. **Dirty state** is `UndoState::stateId` against the id stored at load or save, plus a format flag ("Convert to",
   line separator). Undo back to the saved state makes a document clean. A restored or rebased buffer stores no
   saved id (`kNoSavedState`) until it is saved.
6. **External changes.** A file whose size and time, or failing that XXH3-128, equal the base is unchanged. A
   clean buffer takes the disk's text as one undo step (`EditKind::External`, the line diff of buffer and disk, a
   whole-text edit above the diff's limits). A dirty buffer gets a `PendingMerge` (base = last loaded or saved,
   ours = buffer, theirs = disk) that the merge view settles: apply the merge, keep the buffer (the disk becomes the
   base) or take the disk. Saving checks the disk first, so an unreported change is never overwritten silently.
   Deleted files are reported once; autosave never recreates them.
7. **Large-file guard.** All opens, reloads, merges and hot-exit restores go through `LoadService`.
   `kMaxOpenFileBytes` (128 MiB) and more is refused with `OutOfRange` from the file's status, before it is read.
   A text whose longest line has more than `kMaxEditableLineChars` characters (1 Mi, a placeholder that
   `m1/editor-view` sets) opens read-only. A reload or merge that would cross the threshold, or a merge that
   cannot be built, keeps the buffer and its journal entry and marks the document `DiskState::OutOfDate`; saving
   it then needs an explicit overwrite.
8. **Hot exit.** Journal generations come from one counter per manager and change with the committed state or the
   format. `journalDirty()` puts changed dirty (and out-of-date) buffers and clears entries of buffers that became
   clean. The encoding travels in the journal's opaque labels as nine `|` fields (name, loss, replacements, the two
   loss offsets, the detected guess), checked with `findEncoding` and friends. A label this build cannot read gives
   UTF-8 marked lossy, so saving it needs consent. `restore()` takes the newest entry per key across the recovered
   journals, rebases it on the disk file, writes it into this process's journal, and only then discards the old
   journals. Entries the guard refuses are carried over unchanged.
9. **Autosave.** `AutosaveController` is passive and clock-driven: Save All on frame deactivation, once per idle
   period after `save.autosave.idleSeconds`, and the journal 1 s after the last input. The shell feeds it input
   events and a timer. Autosave skips untitled, out-of-date, merge-pending, deleted and lossy documents.
10. **Threads.** The SaveManager is single-threaded and blocking for now; the encode, write and journal puts already
    work from snapshots taken on the owner thread. Moving them to a background lane is `TODO(M1)` in
    `save_manager.hpp`.

## Consequences
- Saves keep BOM, encoding and every line break (ADR 0018), attributes, ACLs, links and modes, and never leave a
  torn file except during the in-place rewrite of a hard-linked file (decision 3).
- `ReplaceFileW` flags 0 can fail where the user may write the file but not its ACL; that case surfaces as an
  `IoError` and the file is left unchanged. Whether to retry with `REPLACEFILE_IGNORE_ACL_ERRORS` is a hardening
  question.
- Deferred to hardening (ADR 0031): fault injection beyond MemFs and the kill test, sanitizers, fuzzing the label
  parser, a case-insensitive key normaliser for Windows paths (storage's TODO(M2)), and saving through a dangling
  symlink.
