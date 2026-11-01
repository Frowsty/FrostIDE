# ADR 0025: The editor model and its command set (m1/editor-model)

- Status: accepted
- Date: 2026-11-02
- Plan reference: PLAN.md section 3.2 (`src/text`), section 7 (M1 editor), Appendix A rows 39, 44, 45, 85, 94
  and I6 (grapheme navigation); ADR 0017 (Document, undo tree, selections), ADR 0026 (display map), ADR 0031
  (feature-first phase)

## Context
M1 needs the editing behaviour of a Rider-class editor (smart keys, line operations, comments, find/replace,
Go to Line, word and grapheme navigation) before the Qt view exists, testable without Qt, and every edit has to
reach the undo tree with selections and sensible grouping. Grapheme clusters and regex search need Unicode data
and a regex engine that `src/text` does not have.

## Decision
1. **Commands are pure functions** (`frost/text/editor/commands.hpp`, `search.hpp`):
   `CommandResult f(const CommandInput&, args...)`. The input is a `Snapshot`, an `EditorState` (the
   `SelectionSet` plus one `CaretState` per selection: bias and vertical goal x), `EditorSettings`, the
   language's comment tokens and, optionally, the `DisplayMap` for vertical movement. The result is a batch of
   non-overlapping `TextEdit`s, the state after them, an `EditKind` and a `breakCoalescing` flag. Every command
   acts on every caret; multi-caret and column-mode creation stays in M15.
2. **One command, one Document batch.** `frost::text::Editor` (`editor.hpp`) applies a result as one
   `Document::edit` with `EditContext{kind, selections before, selections after}` and feeds the ChangeSet to its
   `DisplayMap`; undo and redo restore the recorded selections; a second editor on the same Document follows
   ChangeSets with `follow()`. So a line operation or a replace all is one undo step, and typing coalesces
   through Document's rules (ADR 0017). Commands set `breakCoalescing` at word starts (a non-space after a space
   or line start), at line breaks, for paste and when typing replaces a selection: typing "hello world" is two
   undo steps.
3. **Unicode:** grapheme clusters by utf8proc's UAX #29 rules (`utf8proc_grapheme_break_stateful`), scanned
   forwards from the line start (at most 4 KiB back) for backward steps; CR LF is one cluster. Word classes come
   from the general category (letters, marks, numbers and connector punctuation are word characters).
4. **Search:** a compiled `SearchPattern`. Plain case-sensitive patterns are a streamed byte search over
   `chunks()`/`reversedChunks()` (new: reverse chunk iteration on `Snapshot`); everything else is RE2 (UTF-8,
   literal mode for plain case-insensitive patterns) run line by line, or over the whole text when the pattern
   can match a line break. Whole word filters the matches: no word character on both sides of either end (the
   classes of decision 3). Replacement templates take `$0`-`$99`, `$&`, `$$` and `\n \r \t \\`.
5. **Comments** come from a small built-in language table (`language.hpp`): C, C++, C#, HLSL/GLSL use `//` and
   `/* */`; languages without line comments (XML, HTML, Markdown) fall back to block tokens per line.
6. **Next/previous member** reads member ranges through the `IOutlineProvider` seam; the syntax and index
   slices provide implementations later.
7. **New dependencies (vcpkg, linked PRIVATE to `frost::text_editor`, not built into fuzz trees):** RE2
   (BSD-3-Clause) with its dependency Abseil (Apache-2.0), and utf8proc (MIT). All three pass
   `tools/licensecheck.py`'s allowlist; no exception is needed. They live in a separate library,
   `frost::text_editor` (layer text, `src/text/editor`), so `frost::text` keeps no third-party dependency.

## Consequences
- m1/editor-view binds keys to these commands and supplies the Qt shaper; it only calls `Editor::run`.
- Deferred to the hardening phase: the QTextBoundaryFinder cross-check and Unicode's GraphemeBreakTest.txt,
  fuzzing and sanitizer runs, B5 timing of the commands, regex search without copying long lines, and display-
  row Home/End under soft wrap.

## Alternatives considered
- **std::regex or PCRE2:** std::regex is slow, recursive and not UTF-8 aware; PCRE2 backtracks. RE2 is linear.
- **ICU:** complete, but tens of megabytes for grapheme breaks and categories; utf8proc is one small C file.
- **Commands as methods on a stateful editor:** harder to test and to reuse for multi-view and macro replay than
  pure functions over snapshots.
