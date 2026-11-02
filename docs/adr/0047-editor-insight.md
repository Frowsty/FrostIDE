<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0047: Editor insight (semantic highlighting, inactive code, squiggles, error stripe, F2)

- Status: accepted for `m3/editor-insight` (feature-first phase, ADR 0031). No test, budget or warning is weakened.
  The registry hash and the theme blob sizes in `tests/ui/model` change with the 28 new colour keys, as those tests
  require for a deliberate registry change.
- Date: 2026-11-02
- Plan reference: PLAN.md section 2 ("Editor rendering", "Semantic engine"), M3; ADR 0036 (ClangdService and the
  diagnostics store), ADR 0037 (EditorView and the display list), ADR 0041 (the language features).

## Context
The editor shows tree-sitter colours only, and clangd's and the build's diagnostics reach no editor. Rider users
expect semantic colours (locals, parameters, fields, macros, ...), dimmed `#if` branches, squiggles, an error stripe
beside the scroll bar, the inspection widget in the corner and F2 between problems. m1/editor-rhi draws the same
EditorDisplayList on the GPU, so nothing may paint outside the display list.

## Decision
1. **Protocol (frost::lang, Qt-free).** `semantic_tokens.hpp` reads the server's legend once into enums, decodes the
   relative 5-integer stream into byte ranges of the snapshot the request saw, and asks for
   `semanticTokens/full/delta` with the previous result id when the server offers deltas (an edit list that does not
   fit falls back to one full request). The client announces every token type and modifier and clangd's
   `inactiveRegionsCapabilities`. ClangdService keeps the last `textDocument/inactiveRegions` per open file, mapped
   against the snapshot of the last diagnosed version, with listeners.
2. **Editor vocabulary (frost::ui_editor, no LSP).** `editor_insight.hpp` defines SemanticKind (16 kinds), three
   modifiers that change painting (readonly, static, deprecated), InsightSeverity (error, warning, weak warning,
   info), EditorDiagnostic with a source (build, live) and the generic EditorDecoration (a range, a style, a colour
   or severity, a tooltip).
3. **Seams on EditorView.** `setSemanticTokens(version, spans)`, `setInactiveRanges(version, ranges)`,
   `setDiagnostics(source, diagnostics)`, `setDecorations(owner, decorations)`, `setUsageHighlights` and
   `setAnalyzing`. The view keeps the last 256 edit batches, so results computed for an older version are mapped
   through the edits since; every stored range follows each edit (start sticks right, end sticks left), so colours
   and squiggles stay on the stale text until fresh results arrive. Build diagnostics carry a line and column and
   are resolved to the identifier there; a build diagnostic that repeats a live one on the same line is shown once.
4. **Layering in the display list.** Text runs split at tree-sitter, semantic and inactive boundaries; inactive
   code wins, then the semantic colour, then tree-sitter's. New display-list parts:
   - `decorations` (backgrounds under the text; wavy, dotted, plain underlines and strike-outs over it). Errors and
     warnings are wavy, weak warnings and infos dotted, deprecated symbols struck out, usages background boxes;
   - the error stripe (`stripe`, `stripeMarks`, `stripeSummary`): 11 px at the right edge, rows scaled to the
     stripe when the document is taller than it, one mark per kind and pixel row, problems full width and usages
     and find matches in the left half;
   - `chrome` (rects, ellipses, polygons, polylines) and `chromeText` for the inspection widget (severity icons,
     counts, up/down arrows, a check when clean, an 8-dot spinner while analysing) and the F2 balloon.
   Every renderer draws these; the raster backend is the only QPainter user.
5. **Interaction.** Hovering a squiggle, a stripe mark or the widget shows the message; a click on a mark jumps to
   it (elsewhere in the stripe it scrolls there); the widget's arrows and F2 / Shift+F2 (`navigate.nextError`,
   `navigate.previousError`) visit the most serious severity present, wrapping, with the message in a balloon at the
   caret. InsightStatusIndicator is the status-bar item (a plain QToolButton).
6. **The bridge (frost::ui_lang).** EditorInsightBinding per view: tokens on attach and 300 ms after the last edit,
   cancelled by newer requests (LatestWins), decoded off the UI thread; live diagnostics and inactive regions from
   ClangdService's listeners, posted to the UI thread; clangd's "comment" tokens stand in for inactive regions until
   the extension's notification arrives; `setBuildDiagnostics` takes BuildService::diagnosticsForDocument. The view
   shows "analysing" from an edit until diagnostics of the current text arrive.

## For the W1 integrator
EditorDecoration is the one range-decoration API: give your own marks (spelling, run-to-cursor, breakpoints' line
tint) to `setDecorations("<owner>", ...)` in current-text offsets; they follow edits like the diagnostics and are
replaced per owner. A decoration with a severity takes that severity's theme colour, otherwise its own colour.

## Consequences
- 28 colour keys (`Editor.inactiveCodeForeground`, stripe, usage and widget colours, and `Semantic.*`) with
  fallbacks; all four Glacier themes set them, dark with a Rider-like palette.
- Deferred to the hardening phase: contrast thresholds for token and marker colours, `workspace/semanticTokens/refresh`,
  clangd's fileStatus for a finer analysing state, sharing tokens between two views of one document, fuzzing the
  token decoder, and perf of the per-edit range mapping (linear in ranges per edit).

## Alternatives considered
- **QWidget overlays for the stripe and the widget.** Rejected: the GPU backend would not see them.
- **Semantic tokens replacing tree-sitter.** Rejected: tokens arrive late and cover identifiers only; layering keeps
  keywords, strings and comments instantly coloured.
- **Range-mode token requests for the visible rows.** Rejected for now: clangd computes the whole file anyway and
  deltas keep the transfers small.
