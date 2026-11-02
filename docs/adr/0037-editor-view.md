<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0037: The editor view (renderer seam, shaper, highlight pipeline, key routing)

- Status: accepted
- Date: 2026-11-02
- Plan reference: PLAN.md section 2 ("Editor rendering"), 2.2 ("Keyboard and IME"), 3.4 (highlighting);
  ADR 0011 A1 (QRhi approved for Windows), ADR 0016 (keymap), ADR 0025 (editor model), ADR 0026 (display map),
  ADR 0030 (German keyboard). Built in the feature-first phase (ADR 0031); W1 critical path.

## Context
m1/editor-model, m1/syntax and m1/settings left a Qt-free editor (frost::text::Editor over a DisplayMap), a
tree-sitter SyntaxSession and a settings store. W1 needs a widget that puts them on screen with Rider's
behaviour, and the shell (m1/shell-frame) needs a factory, status signals and a QWidget.

## Decision
1. **Library `frost::ui_editor`** (src/ui/editor, layer ui). `EditorView` is a QAbstractScrollArea over one
   borrowed Document; `createEditorView(document, path, services, settings)` is the factory the shell's
   IEditorFactory wraps. Status (caret line:column, carets and selected characters, encoding, line separator,
   indentation, read-only, dirty) is `status()` plus one signal per field.
2. **Renderer seam.** Each paint builds an `EditorDisplayList` (fills, text runs with their highlight kind and
   source bytes, lines, carets, gutter text, markers; viewport pixels, backend-neutral) and hands it to an
   `IEditorRenderer`. The raster backend draws with QPainter and caches one QGlyphRun list per (run text,
   style), up to 8192 runs, cleared on font change. The QRhi glyph-atlas backend implements the same interface
   in a follow-up slice; tests read the display list instead of pixels.
3. **Shaper.** `QtTextShaper` implements ITextShaper in 1/64 px units: tabs to the next tab stop, Zero-class code
   points 0, Wide at least two cells, others QFontMetricsF::horizontalAdvance (cached per code point). The view
   disables kerning and, unless editor.fontLigatures is set, `liga` and `calt`; each non-ASCII cluster is drawn
   as its own run at the shaper's x, so carets and glyphs agree. The default font is JetBrains Mono 13 pt.
4. **Highlight pipeline.** The view owns a SyntaxSession (language from the file name). The session's wake-up
   only queues `pollSyntax()` on the GUI thread. Paint queries `SyntaxTree::highlights` for the visible lines
   (widened to whole lines) with a 4 ms budget and caches the spans per (tree, version, range); an incomplete
   result is queried again next paint. While a reparse runs, the session's stale tree (edited to the current
   text) keeps colouring. Kinds map to new theme keys `Syntax.*` (26, chained fallbacks ending at
   `Editor.foreground`) and editor colours to `Editor.*` (11); the four Glacier themes set them. Only
   Editor.foreground on Editor.background joins the contrast policy now; token-colour thresholds are decided
   in the hardening phase.
5. **Key routing.** Editor actions (`editor.*`, `navigate.nextMember/previousMember`; the IntelliJ keymap ids)
   are registered in frost::input's ActionRegistry with context EditorFocus (editing ones also need
   !EditorReadOnly). `EditorKeyRouting` translates each QKeyEvent with frost::ui's adapter, matches it in the
   KeymapService, fires through the registry, and types `typedText` on PassThrough; ShortcutOverride claims
   bound or typing keys. The shell passes its own registry and service; tools and tests use
   `createStandalone`. ADR 0030's positional reading gives Ctrl+- on de-DE for Comment Line.
6. **IME.** QInputMethodEvent preedit becomes a Document composition (Editor::follow keeps the display map);
   the commit is one Typing step that replaces the primary selection. Multi-caret composition is deferred.
7. **Model additions.** `commands::pasteLines` (a line copy pastes above the caret's line, once per line, as
   IntelliJ does); `deleteLines` now merges carets that land on the same line (primary's column wins).
   `frost::text::kLongLineCapBytes` (1,000,000 bytes, Qt-free, for the save manager's large-file mode): the
   view never shapes or paints more than that per row; the hardening phase measures it against B5.
8. **Settings.** `EditorViewSettings` reads the editor keys from a snapshot or from `settingsForFile` (EditorConfig
   overlay); `EditorSettingsBinding` re-applies them live after every store change (the listener only queues).

## Consequences
- The registry hash and the theme blob size change (604 body bytes); every compiled theme is rebuilt.
- Deferred to the hardening phase: B5 measurement of the InputToPaint marker, token contrast thresholds,
  .editorconfig reads off the GUI thread, multi-caret IME and paste distribution, drag auto-scroll timers,
  screenshot goldens of the editor, and the QRhi backend.
