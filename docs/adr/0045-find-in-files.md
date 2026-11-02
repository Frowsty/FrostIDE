<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0045: Find and Replace in Files (m2/find-in-files)

- Status: accepted
- Date: 2026-11-02
- Plan reference: PLAN.md M2 (Find in Files); ADR 0018 (encodings), ADR 0031 (feature-first), ADR 0038 (shell)

## Context
Users need Rider's Find in Path and Replace in Path across a solution. The search has to be fast enough on large
trees, it has to cover every encoding the editor can open, and it has to see what open editors hold even before
it is saved. Replace writes files the user may not have open, so it must never corrupt or truncate one.

## Decision
1. **Engine (`frost::search`, layer "search", rank 7, Qt-free).** `findInFiles` is a `sched::Task` that resumes
   on the request's lane and keeps up to `parallelism` file tasks in flight on the scheduler's workers. Results are
   joined in start order, so batches stream to the sink in file order. A `std::stop_token` is checked between
   files; a stop is not an error (the summary says cancelled). Matching reuses `text::SearchPattern` (plain, Match
   Case, Words and RE2 regex, with the in-file `$0`-`$99`/`$&` replacement templates). Matching is per line: a
   match that spans a line break is dropped unless the query is multiline (the UI sets that when a regex names
   `\n` or `\r`). The engine stops at 10,000 matches and the summary says "too many results".
2. **Files.** File masks are comma- or semicolon-separated globs on the file name, `!` excludes
   (`*.cpp, *.h, !*.generated.h`), ASCII case ignored. Closed files go through `LoadService` (the 128 MiB
   large-file guard, then detection and decoding as the editor does: UTF-8 with or without a BOM, UTF-16 and the
   legacy code page). A file detection calls binary, or one that does not decode, is skipped and counted. Open
   documents are searched in their committed text, read on the owner thread before the search starts through
   `IOpenDocuments` (`SaveManagerDocuments`).
3. **Scopes.** Solution (the default with a solution): every file node of the solution model. Project: the
   project that owns the active editor's file. Directory: recursive `io::scanTree` that skips `.git`, `.vs`, `bin`,
   `obj`, `x64`, `Debug`, `Release`, `build`, `out` and `node_modules` (TODO(M2): use m1/shell-nav's
   gitignore-aware scan). Open files: the open documents.
4. **Replace data safety.** A replace plan names byte ranges from the search, each file's XXH3-64 text hash and
   the pattern. Before a file changes, its text must hash the same and every range must still match the pattern;
   otherwise the file is skipped as changed. Open documents take all of their replacements as one undo
   transaction. They stay dirty and are saved by autosave or the user. Closed files go through
   `SaveManager::rewriteClosedFile`, which works in this order:
   - it refuses a file an editor has open, a lossy decode and a binary file;
   - it encodes with the file's own encoding, BOM and line separators and decodes the result again, which must give
     the edited text back;
   - it records the old bytes in local history ("Before Replace in Files");
   - it re-reads the file, which must still hold the bytes it read first;
   - it writes through `safeWrite` (temp file, flush, atomic rename), so a failure leaves the old file whole.

   Read-only files, locked files, changed files and replacements the encoding cannot hold are skipped and reported.
   The UI posts "N replacements in M files, K skipped".
5. **UI (`frost::ui_find`, layer ui).** `FindSession` runs the engine on the scheduler and posts its batches back to
   the GUI thread; a new search cancels and joins the old one. `FindPopup` is the popup for Ctrl+Shift+F and
   Ctrl+Shift+R. It has the query with Cc/W/.* toggles, the file mask, the scope buttons, a live result list with
   highlighted matches (check boxes in replace mode) and a preview with context lines. Enter opens a match and
   Ctrl+Enter or Open in Find Window moves the results to the Find tool window ("find", bottom, Alt+3). That window
   groups matches by file with counts and has Rerun and Stop. `FindFeature` registers `search.findInFiles`,
   `search.replaceInFiles`, `window.find` and two new keymap entries, `search.nextOccurrence` (Ctrl+Alt+Down) and
   `search.previousOccurrence` (Ctrl+Alt+Up); the IntelliJ goldens record them. The composition root (W1) wires
   the open-location callback and `documentEdited`, which lets the editor follow replace edits. Until then a match
   opens in a shell tab.

## Consequences
- Replace runs on the GUI thread for now (TODO(M2): a background lane with progress for closed files).
- Directory searches do not honour `.gitignore` until the shell-nav scan replaces `scanTree` here.
- The engine reads each closed file whole (bounded by the large-file guard); a trigram index (M13) can come later.
