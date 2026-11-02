<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0042: Shell navigation, sessions and the merge view (m1/shell-nav)

- Status: accepted
- Date: 2026-11-02
- Plan reference: PLAN.md section 7 M1 (shell navigation, external-change handling, hot exit); ADR 0038 (shell
  frame), ADR 0028 (save manager), ADR 0026 (display map), ADR 0021 (yyjson), ADR 0031 (feature-first)

## Context
The shell frame (ADR 0038) has tabs, tool windows and popups but no way to find a file, return to a place or
restore a session, and the SaveManager (ADR 0028) has external-change merges, a large-file guard and a hot-exit
journal with no UI. W1 needs one place that maps paths to open documents.

## Decision
1. **Libraries.** `frost::search` (layer search) holds the path matcher: subsequence scoring with word-start
   (CamelHump, `.`/`_`/`-`/`/`) and consecutive bonuses, path segments matching folders in order, and
   `file:line[:col]` / `file(line)` parsing. `frost::ui_nav` (layer ui, Qt-free, tested by a non-Qt executable)
   holds the models: navigation history, recent files and locations, the session JSON (yyjson) and its store,
   `DocumentRegistry`, `ProjectFiles` and `MergePanes`. `frost::ui_workbench` (Qt Widgets) holds the `Workbench`,
   the File System view, the document editor factory with banners, and the merge view; `frost::app` composes it.
2. **One document per file.** `IFileSystem::canonicalPath` returns one spelling per file (Windows: the real case
   of every component from `GetFinalPathNameByHandleW`; elsewhere the lexical normal form). Every open entry point
   goes through `EditorTabs::open`, which keys tabs by `IEditorFactory::canonicalPath`; the document factory
   answers with `DocumentRegistry::canonicalize` and opens through `SaveManager::open` (the LoadService guard).
   The SaveManager's own normalisation stays lexical, so its tests are untouched.
3. **Project folder scan.** `scanTree` reads `.ignore` files above `.gitignore`, with default excludes (`build/`,
   `out/`, `bin/`, `obj/`, `ipch/`, `cmake-build-*/`, `[Dd]ebug/`, `[Rr]elease/`, `x64/`, `x86/`, `ARM64/`) at the
   bottom of the stack (so a project file can re-include them) and `.git`, `.vs`, `.svn`, `.hg` skipped.
4. **Popups.** `ListPopup` gained an external filter mode (the owner re-ranks on `filterTextChanged`), disabled
   rows and section headers. Find Action uses `input::FindActionModel` with the context captured before the popup;
   Search Everywhere shows Files and Actions sections. Popups opened from a popup close it first.
5. **Navigation history.** Places are (path, line, column). Jumps (opening, switching tabs) and caret moves of 10
   lines or more push the place left; back/forward move between stacks, and places within 3 lines replace each
   other. Mouse back/forward buttons map to the actions.
6. **Sessions.** One JSON file per project under `<data>/sessions/project-<fnv1a64(root)>.json` plus `index.json`
   with the last project: tabs (order, pinned, active, caret, first visible line), tool-window visibility and
   size, recent files and locations. Saved when the project closes or the window goes; restored on reopening the
   project and for the last project at start-up. Tabs that fail to open are skipped with their notification.
7. **Hot exit.** After the first frame the app scans for recoverable journals; the workbench offers them in a
   sticky notification ("FrostIDE closed unexpectedly. Restore N unsaved files?") whose Restore runs
   `SaveManager::restore` and opens the restored files.
8. **External changes.** The SaveManager's listener rebuilds the editor banners (long line, out of date with
   Reload/Overwrite, merge pending with Compare..., deleted) and posts a notification with Compare... for a merge.
   The merge view shows yours, base and disk in three panes aligned by filler block decorations (each changed
   chunk is padded to its tallest side), with per-chunk Accept Left/Right and Apply, Keep My Version, Take Disk
   Version through `resolveExternalChange`.
9. **Saving.** Save All (Ctrl+S) and Save Document are actions; autosave is fed by window deactivation, input
   events and a 500 ms poll of `AutosaveController`.

## Consequences
- W1 binds editors to documents through `DocumentRegistry` (acquire/release per view, change listeners).
- M2's index replaces `ProjectFiles` as the Go to File source; the folder scan stays for the File System view.
- Deferred (feature-first): large-tree performance of the File System view model, watcher exclusions for
  `.git`/build folders, untitled-buffer tabs after restore, session writes on a timer, and the hardening items.
