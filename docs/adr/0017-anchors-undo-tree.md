# ADR 0017: Anchors, selection sets and the transactional undo tree (frost::text Document)

- Status: accepted for the `m1/undo-anchors` slice. No budget, test or warning is weakened;
  `tests/perf/budgets.json` and `TextPerf.EditLatency` are unchanged. ADR 0013's text-side sub-budget of B5 now
  covers the whole text-side keystroke, undo and redo included (decision 12).
- Date: 2026-11-01
- Plan reference: PLAN.md section 3 ("Text buffer": "O(1) snapshots, anchors, a transactional undo tree excluding
  IME preedit") and the edit data flow; section 3.4 ("Keystroke → paint", "Save and external change"); section 4,
  B5 and B19; sections 6.1, 6.4 and 6.6; section 7 M1. ADR 0013's "Interop notes for later slices". Feature matrix
  rows 35 (undo part, row unchanged) and 38 (planned → in-progress).

## Context
ADR 0013 gave frost::text a persistent piece tree with O(1) snapshots and D8-normalised `TextChange`s, and left
three questions to this slice: how anchors survive edits (storage positions or TextChange-driven offsets), how undo
reports precise changes (it anticipated a structural diff over node identity), and how M1's editor model carries
multiple carets. The consumers fix the requirements:
- m1/editor-model needs caret sets that merge when they overlap and that map through edits as values;
- m1/display-map (folds), M3's DiagnosticsStore and bookmarks need anchors that resolve against any later snapshot,
  from any thread, in O(log n);
- tree-sitter and the M3 LSP client need precise, replayable changes for undo and redo as well as for edits;
- the editor needs an undo *tree* (PLAN.md section 3), typing coalescing, groups for multi-step commands, and IME
  preedit that is visible to every consumer but never becomes an undo state;
- B5: the text-side work of a keystroke, undo or redo has ADR 0013's sub-budget (p99 0.25 ms); B19 bounds what the
  history may retain (UI process 80 MB with no project).

## Decision
1. **Anchors are mapped through raw edits** (`RawEdit {range, newBytes}`), never through D8 TextChanges, which
   widen a change by one byte at a CR and would move an anchor that sits before the CR past the inserted text
   (`AnchorSet.CrlfInteriorUsesRawEdits`). The rule for a replace of [a, b) by n bytes ("attachment" semantics):

   | offset x | Bias::Left | Bias::Right |
   | --- | --- | --- |
   | x < a | x | x |
   | a <= x <= b (closed) | a | a + n |
   | x > b | x - (b - a) + n | x - (b - a) + n |

   A batch (sorted as `applyEdits` requires) is sequential application from the last edit to the first, each in
   pre-batch coordinates; no-op edits map every offset to itself and are dropped from every batch Document
   reports. `mapOffset(batch)` uses a closed form (anchor.hpp) that `TextAnchor.BatchClosedFormMatchesSequential`
   checks against sequential application (see the deviations for the corrected Left case).
2. **Per-snapshot anchor trees, not edit history.** `AnchorSet` is an immutable persistent value of three
   `SumTree<.., 8>`s: a Left-bias and a Right-bias class tree of `{gap, label, id}` (positions are prefix sums of
   the gaps, labels increase along each tree) and an id index of `{id, label, bias}`. Within one bias class the
   mapping is monotone and depends only on (offset, bias), so edits never reorder entries, ties never need an
   order, labels stay sorted under edits, and transforms never touch the index. `resolve(id)` is an index seek and
   a label seek, O(B log n). One edit step costs O(B log n + k) per class for the k anchors collapsed into the
   replaced range, O(B log n) when they already share an offset (the first-gap shortcut), or one O(n + m) sweep
   for large batches (`m · ⌈log2(n + 2)⌉ > n`); `AnchorSet.SpliceAndSweepAgree` checks both paths give identical
   sets. Document publishes `DocumentSnapshot {Snapshot text; AnchorSet anchors;}`, so an anchor created at
   version v resolves in every later DocumentSnapshot on any thread. `Piece` (72 bytes) and `Snapshot` are
   unchanged, and M13 compaction cannot invalidate anchors (storage.hpp's TODO now says so).
3. **Order labels.** u64 labels in [1, 2^64 - 2]; a new label between lo and hi is `lo + min((hi - lo) / 2, 2^32)`
   (the cap keeps appends cheap). With no free label, Bender et al.'s simplified relabelling with T = 1.5 spreads
   the smallest enclosing block of 2^i labels whose count n_i satisfies n_i + 1 <= t_i = ⌊(4/3)^i⌋, from a
   constexpr table (`static_assert` t_1 == 1, t_8 == 9, t_64 > kMaxAnchors). Ids are u64, never reused within a
   lineage; a set holds at most `kMaxAnchors = 2^24` anchors, so the top level always succeeds. Relabel counters
   are a value field (`Stats`), never a static.
4. **Selections are values on anchors.** `Selection {anchor, head}`; a caret maps both endpoints Right, a
   non-empty selection its lower endpoint Left and its upper endpoint Right. `SelectionSet` is an immutable,
   reference-counted sorted array (O(1) copies). Normalisation sorts by (start, end, input index) and merges s
   with the next t when `t.start < s.end`, or `t.start == s.end` and s or t is empty; the merge takes the direction
   of its first non-empty member, is primary if any member was, and reports its first member as its `origin`.
   Touching non-empty selections stay separate; carets inside or at the edge of a selection are absorbed.
   SelectionSets do not use registry anchors (carets change every keystroke; editor commands are pure functions).
5. **Undo stores regions with piece slices.** A transaction from X (the parent's text) to Y is a canonical list of
   regions `{before (X), after (Y), removed slice, inserted slice}`: the gaps of the X-to-Y alignment of kept bytes
   (bytes no edit replaced, even with identical content), so regions are separated by at least one kept byte in
   both texts. Slices are `PieceTree`s of existing pieces; nothing is copied and no Snapshot is retained (except a
   composition's base). Coalesced edits compose into the list (closed-interval runs, below); undo and redo replay
   the slices through the private `TextBuffer::replaySlices`, which runs the same D8 `normalisedChange`, so their
   TextChanges are precise and LSP-replayable by construction. The cost is O(regions × (B log n + P)),
   independent of the edited size. `TextBuffer::restore` keeps its whole-text change; no structural diff and no
   node identity are needed (sum_tree.hpp's comments now say so).

   Composition of a batch E (Y coordinates, no no-ops) into the regions R (undo_tree.cpp):
   ```
   items = regions ([after.begin, after.end]) merged with edits ([c, d]) in begin order
   dE = 0; dR = 0
   for each maximal run C where next.begin <= running max end (closed-interval touch):
     if C is one region: emit it with after += dE; dR += its delta; continue
     minY = C.begin; maxY = C.maxEnd; ΔE = Σ edit deltas in C; ΔR = Σ region deltas in C
     before   = [minY - dR, maxY - dR - ΔR)          // X
     after    = [minY + dE, maxY + dE + ΔE)          // Z, the text after the batch
     removed  = Y[minY, r1.after.begin) + r1.removed + ... + rk.removed + Y[rk.after.end, maxY)
     inserted = Z[after)
     drop C if before and after are both empty, else emit it
     dE += ΔE; dR += ΔR
   ```
6. **Undo tree.** Nodes are states and the edge into a node is its transaction; ids are u64, never reused within
   a Document. A new transaction is a new child of the current node, so old children survive as redo branches.
   `redo()` follows `redoChild` (the most recently created or undone-from child); `redo(branch)` picks a child.
   `redoChild` is none exactly when a node has no children.
7. **Coalescing.** An edit joins the current open transaction iff its kind is Typing, DeleteBackward or
   DeleteForward and equals the transaction's, `now - lastEdit <= coalesceWindow` (default 1000 ms, inclusive,
   from the IClock; lastEdit is the latest edit, so the window slides), and `context.selectionsBefore ==
   transaction.selectionsAfter` (the carets did not move; empty sets compare equal). It is decided after
   validation, so a rejected edit seals nothing. Undo, redo, rollback, group begin and end, `breakCoalescing()`,
   any recorded edit that does not coalesce and re-rooting seal the transaction.
8. **Groups.** `beginGroup`/`endGroup` nest; the outermost pair is one transaction, created lazily at the first
   text change, with the outermost begin's kind and `selectionsBefore` and the outermost end's `selectionsAfter`.
   Groups never coalesce; each edit inside stays all-or-nothing. `rollbackGroup()` undoes and deletes the group's
   node, leaving no redo branch, and restores the parent's previous `redoChild` if it still exists.
9. **IME.** Preedit is applied to the buffer (`ChangeOrigin::Composition`) so every consumer sees it, and is never
   recorded. A commit replaces `[rb, re + len)` of the current text and records the net step "replace
   `replaceInBase` (default the empty range at the start) of the base with the committed text" against the
   composition's base tree and AnchorSet; it may coalesce with earlier typing. Cancel records nothing; an empty
   commit with an empty net range is a cancel. While composing, edit, undo, redo and groups return Busy.
   `committedText()` is the text without the preedit; persistence must use it.
10. **Anchor restoration (the identity property).** Each transaction keeps `anchorsBefore` (taken at its first
    edit, or the composition base for a commit; replaced by the pre-redo set at each redo) and `anchorsAfter` (the
    pre-undo set at each undo). After an undo every anchor that `anchorsBefore` holds is at its offset there and
    every other anchor is mapped; redo is symmetric with `anchorsAfter`. So undo-then-redo and redo-then-undo are
    the identity on text and anchors, and a bookmark inside deleted text comes back. The implementation
    repositions only anchors in the closed X (or Y) ranges of the regions (document.cpp's header comment gives the
    proof) and skips moves to an anchor's current offset: O(regions × (B log n + P) + r × B log n) for r moved
    anchors, or a bulk rebuild (deviations).
11. **History cap.** `maxTransactions` (default 1000, nodes excluding the root). Right after a node is created,
    while the cap is exceeded: if the root has two or more children, drop its oldest child subtree (by sequence)
    that does not hold the current node and repair the root's `redoChild` (its remaining child with the largest
    sequence); otherwise re-root: the only child becomes the root, keeps its id, stateId, children and
    `redoChild`, and drops its transaction data. The current node and its ancestors are never removed.
12. **B5.** ADR 0013's text-side sub-budget (p99 <= 0.25 ms per op) applies to `Document::edit` (Typing and Delete)
    and `Document::undo`/`redo`, asserted by `DocumentPerf.TypingLatency` and `DocumentPerf.UndoRedoLatency` in
    optimised unsanitised builds. The heavy undo and redo ops (those restoring many anchors) are reported
    separately so a p99 cannot hide them.
13. **Concurrency and ownership.** Document has one writer thread. `DocumentSnapshot`, `AnchorSet` and
    `SelectionSet` are immutable values safe on any thread (ADR 0013 D7's reference scheme); empty trees and an
    empty SelectionSet are null references, never function-local statics. Undo slices hold piece pointers and no
    storage reference; Document owns the TextBuffer and destroys the undo tree first (declaration order is
    commented). The IClock must outlive the Document.
14. **State identity.** `UndoState.stateId` is a per-Document counter value stamped on the current node at every
    recorded change (a new node or a coalesced extension); undo, redo and rollback make the target node's value
    current; preedit, no-ops and rejections leave it. Equal values mean equal committed text. m1/save-manager
    stores it at save for the dirty flag; a pruned saved state never comes back, so the document stays dirty.

### Errors, checked in this order (shared with the UndoOracle)
| Operation | Checks, in order |
| --- | --- |
| `createAnchor` | OutOfRange (offset > size); InvalidArgument (inside a UTF-8 sequence); OutOfRange (kMaxAnchors) |
| `releaseAnchor`; `AnchorSet::resolve`, `anchor`, `without` | NotFound |
| `edit` / `replace` | Busy (composing); then TextBuffer's checks 1-5 (applyEdits' order for `edit`) |
| `undo` | Busy (composing); Busy (group open); NotFound |
| `redo(b)` | Busy; Busy; OutOfRange if b >= children, or NotFound without b and no child |
| `beginGroup` | Busy (composing) |
| `endGroup` / `rollbackGroup` | InvalidArgument (no group open) |
| `beginComposition` | Busy (composing); Busy (group open); OutOfRange; InvalidArgument (inside a sequence) |
| `updateComposition` / `cancelComposition` | InvalidArgument (not composing); TextBuffer checks 4-5 |
| `commitComposition` | InvalidArgument (not composing); with replaceInBase: InvalidArgument (begin > end), OutOfRange (end > base size), InvalidArgument (not containing the start), InvalidArgument (a bound inside a sequence); TextBuffer checks 4-5 |

Every rejection leaves the text, version, anchors, undo tree (coalescing state and stateId included), group and
composition state unchanged (`DocumentUndo.ErrorsAndOrder`, and the differential's invalid ops).

### ChangeSet contents
| Operation | origin | edits (ascending, no no-ops) | selections | recorded |
| --- | --- | --- | --- | --- |
| `edit`, `replace` | Edit | the requested edits | context.selectionsAfter | yes, unless nothing changed |
| `undo` | Undo | `[{r.after, \|r.before\|}]` | N.selectionsBefore | moves current |
| `redo` | Redo | `[{r.before, \|r.after\|}]` | C.selectionsAfter | moves current |
| `rollbackGroup` | Rollback | the group node's undo (empty if none) | the group's selectionsBefore | deletes the node |
| `updateComposition` | Composition | `[{[start, start + len), \|p\|}]` | empty | no |
| `commitComposition` | Edit | `[{[rb, re + len), \|t\|}]` | context.selectionsAfter | net `[{[rb, re), \|t\|}]` vs the base |
| `cancelComposition` | Composition | `[{[start, start + len), 0}]` | empty | no |

`changes` are the D8 changes in application order; `baseVersion` and `version` bracket the operation and are equal
when nothing changed. New-text extraction for undo, redo and rollback: change i's new bytes are the post-op
snapshot's bytes [start_i + s_i, newEnd_i + s_i), s_i the sum of (newEnd - oldEnd) over the changes applied after
it; this holds because regions are separated by kept bytes, which D8's one-byte widening never alters
(`UndoProperty.ChangesReplayAsLsp` replays every history ChangeSet this way).

### Interop notes for later slices
- **Composition changes** are for consumers that show the preedit; the M3 LSP client should defer them and flush
  or drop them at the next non-Composition ChangeSet (a cancel nets to no change from the base).
- **Edit batches** whose D8 widening reaches into an adjacent requested edit may not satisfy the extraction rule
  above; the M3 LSP client may need full sync for those (edits are fine: they carry their own text).
- **Heavy restores:** an undo that restores r anchors costs about 20-30 us per anchor on REF-W1 when r ·
  ⌈log2(n + 2)⌉ <= n, else a bulk rebuild of all n anchors (measurements). m1/editor-view should not undo inside
  a frame budget it cannot exceed.

### Deviations from the slice spec
- **Left closed form.** The spec's batch closed form maps Left to `a_F + D_F` (F the first edit whose closed range
  contains x). That is wrong when edits touch: for [2, 4) → 3 bytes and [4, 6) → 0 bytes, x = 6 Left maps to 2
  sequentially (6 → 4 through the second edit, 4 → 2 through the first), but `a_F + D_F` gives 4 + 1 = 5. The
  implementation uses `a_K + D_K`, K the first edit of the run of touching edits (b_{i-1} == a_i) that ends at F;
  the Right form is the spec's. Found by `TextAnchor.BatchClosedFormMatchesSequential`, documented in anchor.hpp.
- **`withMoved` bulk path.** Besides per-move reinsertion (O(r B log n)), `withMoved` rebuilds all three trees in
  O(n log n) when `r · ⌈log2(n + 2)⌉ > n` (the index sorted by id once). Restores of thousands of anchors (a big
  deletion's undo) would otherwise cost milliseconds more.
- **Restore scan matching.** The scan matches the mapped set's anchors in each closed range with `restoreFrom`'s
  anchors in the same range by id; an anchor not found there is looked up with `resolve` (section 4.6 argues this
  never happens; it costs nothing when it does not).
- **Interpretations.** Re-rooting seals the node that becomes the root (its data is dropped; the current node,
  never the root, stays open). Edits inside a group set no `selectionsAfter` (only the outermost `endGroup` does).
  `UndoState.canUndo` means "the current node is not the root" (undo may still be Busy). An empty commit is a
  cancel whenever the net range is empty, with or without an explicit empty `replaceInBase`.
- **Not unit-tested:** the kMaxAnchors (2^24) exhaustion error; creating 16.7M anchors per test run costs too much.
  Its check is one comparison in `Document::createAnchor`, ordered as in the table above.
- **Test-only accessors.** `AnchorSet::classTreeForTesting`/`indexForTesting` expose the trees for
  `DocumentPerf.UndoRedoLatency`'s memory report (unique nodes the history retains); not public API.
- **Property generator.** The UndoProperty cases draw their ops from the UndoDriver, which also compares every op
  with the UndoOracle (spec: "these properties do not use the oracle"). The identity checks compare Document
  states only; the oracle comparison is an extra check, not a dependency of the properties.
- **Driver selections while composing.** The differential driver keeps the selections of `beginComposition`
  (base coordinates, which a commit records and undo restores) while composing; selection moves become IME ops.
  Callers must likewise pass base-coordinate `selectionsBefore` to `commitComposition` (FROST_PRE).

## Measurements
Machine: REF-W1 (AMD Ryzen 9 5900X, 12 cores / 24 threads, 32 GB, Windows 11 10.0.26300; WSL Ubuntu 24.04). LLVM
23.1.4; MSVC STL 14.44 on Windows, libstdc++ 14 on Linux. Every Windows run went through `run_hidden_nobox.py` (a
private desktop). Other slices were building in parallel (`-j 6` each), so timings are upper bounds.

### Perf (REF-W1, Windows Release; `text_perf_tests`)
Two runs each: a standalone run, then `verify -Full`'s.
- `DocumentPerf.TypingLatency` (16 MiB, half CRLF, 10k anchors, 100k ops, 85 % Typing / 15 % DeleteBackward, 50 ms
  per op; history at its 1000-transaction cap): **p50 18.5 / 18.8 us, p99 42.6 / 44.2 us, max 324 / 155 us** (B5
  sub-budget p99 250 us). For comparison `TextPerf.EditLatency` (bare TextBuffer) gave p50 9.7 us, p99 22.2 us in
  ADR 0013.
- `DocumentPerf.UndoRedoLatency` (200 runs of 1000 keystrokes, a 64 KiB paste, a 4 MiB deletion; 404 undos and
  redos): **p50 13.0 / 12.6 us, p99 69.7 / 59.5 us, max 1626 / 1618 us.** Heavy ops: deletion undo 1626 / 1618 us
  (2395 anchors restored, bulk path), deletion redo 564 / 533 us (0 restored: the 2395 anchors collapse to the
  deletion point, which is where `anchorsAfter` has them), paste undo 15.9 / 14.4 us and redo 14.7 / 13.6 us (0
  restored).
- Memory: the 202 transactions retain 2,118,736 bytes of nodes beyond the live text and anchors (unique SumTree
  nodes of slices and anchor sets plus node and region records), **10.5 KB per transaction**; 1000 transactions
  would hold 10.5 MB, 13 % of B19's 80 MB. Most of it is anchor-set path copies (10k anchors).
- `DocumentPerf.AnchorsAndCarets` (report only): resolve with 100k anchors p50 0.2 us, p99 0.6 us; one edit
  collapsing 1000 anchors at distinct offsets 81 / 166 us, its undo (restoring them one by one among 101k)
  21.0 / 30.6 ms; a backspace next to 10k anchors at one offset 35 / 41 us (first-gap shortcut); 100k adversarial
  inserts (each between the previous one and a fixed neighbour) 1.33 / 1.38 s with 13,011 relabels of 1,049,910
  entries (10.5 per insert, amortised O(log n)); a 10k-caret Typing batch 204 / 212 ms (edit, anchors,
  `SelectionSet::transformed`), its undo 81 ms.
- Debug (1 MiB, report only): Windows typing p50 134 us, p99 289 us, deletion undo 25.6 ms (2470 anchors),
  deletion redo 21 ms, history 14.6 KB per transaction; WSL typing p50 95 us, p99 185 us, undo/redo p50 56 us,
  p99 11.9 ms, max 16.7 ms.
- `AnchorSet.LabelExhaustionPatterns` (10k anchors): one tie position 1199 relabels of 85,935 entries,
  alternating head and tail 598 of 36,920, between two anchors 1199 of 85,936; each within the asserted bound
  64 · n · ⌈log2 n⌉.

### Test times and scales
| Configuration | `UndoDifferential.Quick` | `AnchorSet.MatchesNaiveModel` | `UndoProperty.*` (3) | `AnchorConcurrency` | all new cases | stress: UndoStress / AnchorStress |
| --- | --- | --- | --- | --- | --- | --- |
| Windows Debug | 9.2 s | 13.8 s | 10.3 s | 0.8 s | 37.5 s (68 unit cases) | not run (`-LE stress` in Debug) |
| Windows Release | 0.3 s | 0.8 s | 1.4 s | 0.1 s | 19.6 s | 1.3 s (500k) / 7.1 s (500k) |
| Windows ASan (RelWithDebInfo) | 11.5 s | 30.1 s | 49.2 s | 1.2 s | 162.5 s | 9.0 s (200k) / 28.8 s (100k) |
| WSL Debug | 2.5 s | 9.7 s | 9.5 s | 0.6 s | 28.3 s | not run |
| WSL ASan + UBSan (Debug) | 10.5 s | 27.2 s | 42.7 s | 2.5 s | 180.5 s | 25.4 s (200k) / 48.2 s (100k) |
| WSL TSan (RelWithDebInfo) | 10.9 s | 10.8 s | 23.5 s | 1.0 s | 74.7 s | 4.2 s (100k) / 8.2 s (50k) |

"All new cases" sums this slice's 73 ctest cases (68 unit, 2 stress, 3 perf) where they run, from each tree's
`LastTest.log` in `verify -Full` (Windows Debug: the hidden gtest runs of `text_tests`, unit cases only).
`UndoDifferential.Quick` stays inside its 15 s Windows Debug ceiling at the spec's scale (full check every 32 ops,
16 KiB documents), so neither fallback of section 7.3 was needed. `AnchorSet.MatchesNaiveModel` (200 seeds × 2000
ops) runs its seeds on up to four threads, comparing sizes after every op, `all()` with the naive model every 10
ops, and `resolve` and `checkInvariants` every 100 ops and at the end. Every stress case stays under its 120 s
ceiling, and `AnchorConcurrency` and `AnchorStress` are clean under TSan.

Scales: `UndoDifferential.Quick` runs 200 seeds × 1000 ops on up to 4 threads (piece caps {4, 5, 7, 16, 4096},
history caps {1000, 8, 1}, a small buffer limit for one seed in seven, initial texts up to 4 KiB, a full check every
32 ops); `UndoStress.Differential` 500k ops optimised, 200k under ASan, 100k under TSan and in Debug (seeds of 20k
ops from 2,000,000, a full check every 64); `AnchorStress.ConcurrentReaders` 500k writer ops optimised, 100k under
ASan, 50k under TSan and in Debug. Failing seeds print a command that re-runs them through
`UndoDifferential.Quick`.

### Mutation checks (Windows Release, each reverted)
| Mutation | Caught by |
| --- | --- |
| drop the restore scan | 9 cases, e.g. `AnchorSet.DeletionBoundaries`, `DocumentUndo.AnchorsRestoredOnUndoOfDeletion`, `UndoDifferential.Quick`, `UndoProperty.*` |
| skip the anchorsBefore replacement at redo | `UndoProperty.UndoThenRedoIsIdentity`, `RedoThenUndoIsIdentity`, `ChangesReplayAsLsp` |
| merge touching regions as open intervals (regions, edits) | 8 and 12 cases, e.g. `UndoCompose.Cases`, `UndoCompose.RandomMatchesAlignment`, `UndoDifferential.Quick` |
| map anchors through the D8 changes | clamped to the text: `AnchorSet.CrlfInteriorUsesRawEdits`; unclamped, the run aborts on `applyEdits`' batch precondition |
| drop selection equality from coalescing | 6 cases, e.g. `DocumentCoalesce.CaretMoveBreaks` |
| record preedit edits | `DocumentUndo.ErrorsAndOrder`, `ChangeSetTable`, `StateIdTracksSavedState`, `UndoDifferential.Quick` (filtered run); unfiltered, the run aborts in `DocumentIme.PreeditNotUndoable` |
| restore the previous redoChild without an existence check | an assertion abort (`found != nodes.end()`) in `DocumentGroup.RollbackAfterPrune` |

## Consequences
- **m1/editor-model** builds commands as pure functions over `SelectionSet` values, uses `fromSelections`' and
  `transformed`'s `origins` to carry goal columns and per-caret clipboard text through merges, passes
  `EditContext` (kind, selections before and after) to every edit, and wraps multi-step commands in groups.
- **m1/syntax** consumes `ChangeSet.changes` with `baseVersion` (edits, undo and redo alike).
- **m1/display-map** keeps folds as registry anchors through `Document::createAnchor`, which undo restores. The
  alternative, its own AnchorSet transformed with `ChangeSet.edits` after checking `baseVersion`, gets no undo
  restoration.
- **m1/save-manager** reloads as `EditKind::External` (Busy while composing: commit or cancel first), takes the
  dirty flag from `UndoState.stateId`, and persists `committedText()`, never the preedit.
- **m1/editor-view** chooses preedit in the buffer or view-only, and maps QInputMethodEvent replacements to
  `commitComposition`'s `replaceInBase`.
- **M3 LSP client:** `baseVersion`; deferral of Composition changes; the new-text extraction rule for undo, redo
  and rollback; edit batches with adjacent ranges may need full sync.
- **M3 DiagnosticsStore** re-anchors through `ChangeSet.edits`.
- **M13 compaction** must keep or remap the bytes undo slices reference; anchors are unaffected.
- `TODO(M1)` in src/text/CMakeLists.txt keeps m1/encodings and m1/large-file only.

## Alternatives considered
- **Storage-position (insertion-id) anchors over a tombstone tree:** changes `Piece` and every query, risks B5, and
  M13 compaction would have to remap them. Not pursued (and Zed's GPL text crate, which uses that design, was not
  opened).
- **Edit-history anchors** (resolve by replaying the edits since the anchor's version): O(Δversions) per resolve.
- **One anchor tree with a canonical tie order:** edits collapse anchors of both biases into one offset, which can
  reorder entries and forces relabelling inside transforms; the two class trees never reorder.
- **TextChange-driven anchors:** the D8 widening misplaces anchors at a CR (decision 1).
- **Snapshot-restore undo plus a structural diff:** retains O(height) nodes per transaction and its changes are not
  D8 by construction.
- **Deleted text stored as strings:** memory proportional to deleted bytes; slices reference existing pieces.
- **An undo stack:** PLAN.md section 3 requires a tree.
- **Registry anchors for selections:** churn on every keystroke, and editor commands would stop being pure.
- **Version numbers for the dirty flag:** undo bumps the version, so equal text would look dirty.

## Sources
- Michael A. Bender, Richard Cole, Erik D. Demaine, Martin Farach-Colton, Jack Zito, "Two Simplified Algorithms for
  Maintaining Order in a List", ESA 2002, LNCS 2461. The relabelling (decision 3; `anchor_set.cpp`, `relabel`)
  follows that algorithm as the slice spec's section 4.3 describes it (blocks of 2^i labels, threshold
  t_i = ⌊T^i⌋ with T = 1.5 on the count, even spreading). The paper itself was not opened during this slice; it is
  cited as the algorithm's origin.
- The existing frost::text `SumTree` (ADR 0013 and its sources): the anchor trees and the index are
  `SumTree<.., 8>` instances with monotone seeks.
- No editor's undo or anchor implementation was consulted: not Zed's text, rope or editor crates, Qt Creator,
  KDDockWidgets, JetBrains or Visual Studio sources, Emacs `undo-tree.el` or Vim's `undo.c`. Vim's `:help
  undo-tree` and CodeMirror 6's reference manual (allowed) were not read either; the undo tree of states is a
  generic concept and the design follows the slice spec.
- No function is a port of upstream code, so no NOTICE entry is needed. No dependency was added.
