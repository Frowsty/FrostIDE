# ADR 0026: The layered display map (m1/display-map)

- Status: accepted
- Date: 2026-11-01
- Plan reference: PLAN.md section 2 ("Editor rendering"), section 3.2 (`src/text`), section 7 (M1 soft wrap,
  feature row 250); ADR 0013 (SumTree), ADR 0017 (ChangeSets, anchors), ADR 0029 decision 9, ADR 0031
  (feature-first phase)

## Context
m1/editor-model and m1/editor-view need one structure that maps buffer offsets to display rows and back in
O(log n), follows Document edits without re-laying out the whole file, wraps softly, and inserts whole rows for
the compare and merge views' alignment fillers. ADR 0029 decision 9 fixes the layer order (folds -> soft wraps
-> inlays -> block decorations), keeps the fold and inlay layers as identities until M15, and requires their
interfaces to be final now. Qt stays out of `src/text`, so text measurement needs a seam.

## Decision
1. **Four layers, one class each** (`src/text/src/display_layers.hpp`, private), composed by the public
   `frost::text::DisplayMap` (`display_map.hpp`). An edit goes up the stack as a patch of replaced line or row
   ranges: buffer lines -> fold lines -> wrap rows -> inlay rows (equal to wrap rows: inlays never add rows) ->
   display rows. `apply(ChangeSet, Snapshot)` returns the display-row patch for repainting.
2. **Fold and inlay layers are identities with their final queries.** The wrap layer reads only the fold layer
   (line starts and ends, `lineAt`, chunks tagged with a `SegmentKind`, buffer <-> fold offsets with a bias);
   DisplayMap reads columns and row segments only through the inlay layer. M15 fills both without touching the
   wrap or block layers. No stub mutators are added.
3. **Wrap tree items end at soft wraps.** A `WrapItem` is a row starting at its first byte followed by whole
   unwrapped lines; items merge across any line break, so unwrapped text is one item and the tree has one item
   per soft wrap plus one. Rows inside an item are found through the fold layer's line index, so both mapping
   directions stay O(B log n).
4. **Line patches come from the D8 TextChanges** (ADR 0013), composed step by step (`composeLinePatch`); D8
   guarantees the line structure outside each change is unchanged, which raw edits do not (a CR followed by an
   inserted LF). Anchors still move by the raw edits (ADR 0017).
5. **Wrap rules:** greedy; break opportunities after a run of spaces or tabs and before or after a Wide
   character; spaces and tabs hang; Zero-width characters and the character after a ZWJ never start a row; a
   word longer than the row breaks before the overflowing character; every row holds a character. Every row lays
   out from x = 0, so tabs are measured from the row start. Full UAX #14 line breaking is not attempted.
6. **Units.** `TextWidth` is an `int64` in shaper units: the Qt shaper (m1/editor-view) will use 1/64 px
   (26.6 fixed point, like QFixed), so wrap decisions are exact integers. `ITextShaper` gives `cellWidth()` and
   `advance(cp, x)`; `MonospaceTextShaper` is the deterministic double (Normal 1 cell, Wide 2, Zero 0, tabs to
   multiples of the tab size). Display columns are byte offsets into a row's display text.
7. **Blocks** keep their anchors in a private `AnchorSet` transformed with each ChangeSet's raw edits: Above
   blocks use Bias::Right, Below blocks Bias::Left, so a block stays with its line's content. ADR 0017 suggested
   registry anchors for folds (undo restores them); fillers are rebuilt by their diff anyway, so blocks do not
   need undo restoration. At a boundary, Below blocks of the upper line precede Above blocks of the lower line,
   each in creation order. An edit re-places only the blocks attached to its region's lines.
8. **Soft-wrap boundaries** use `Bias`: Left is the end of the upper row, Right the start of the lower row (the
   anchor rule). Hit tests return the bias that keeps the caret on the hit row; vertical moves skip block rows.

## Measurements
None: feature-first phase (ADR 0031). B5 with soft wrap on is measured when m1/editor-view paints.

## Consequences
- m1/editor-view supplies the Qt `ITextShaper` and paints rows from `DisplayRowCursor` segments; m1/editor-model
  moves carets with `moveVertically` and keeps the returned goal x.
- Deferred to the hardening phase: the display-map property test against a brute-force oracle (M1 exit), fuzzing,
  B5 with soft wrap on, background wrapping of large files, tables generated from the UCD, and UAX #14/#29.
- `TODO(M1)` in text_shaper.hpp tracks the generated width tables.

## Alternatives considered
- **One item per display row:** simpler, but 16+ bytes per row even with wrapping off; rejected for memory.
- **Wrap breaks stored per line in a vector:** copies allocate on every node rebuild and a long line needs a
  linear scan; rejected.
- **Line patches from raw edits:** wrong when an edit changes a neighbouring line break (CR + LF); rejected.
- **Registry anchors for blocks:** undo would restore fillers that the diff recomputes anyway, and the
  Document would carry view-specific anchors; rejected.
