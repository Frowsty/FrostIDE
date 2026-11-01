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
   for large batches (`m · 16 · ⌈log2(n + 2)⌉ > n`, the measured cost model of `anchor_paths.hpp`; "Path
   thresholds" below); `AnchorSet.SpliceAndSweepAgree` and `SpliceAndSweepAgreeAlongChains` check both paths give
   identical sets. Document publishes `DocumentSnapshot {Snapshot text; AnchorSet anchors;}`, so an anchor created at
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
   composition's base). Removed slices are cut from X's own piece tree, the *compose base*, which UndoTree keeps
   beside the current node only while that node accepts edits (open, or the open group's node) and drops when it
   is sealed (U8 checks both directions); it retains only the nodes X no longer shares with the live text. So a
   removed slice holds X's pieces over `before`, at most two of them cut, and a run of k coalesced backspaces keeps
   about k / maxPieceBytes + 2 pieces, not k one-byte pieces (`DocumentUndo.DeleteRunsKeepWholePieces`).
   Coalesced edits compose into the list (closed-interval runs, below); undo and redo replay
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
     removed  = X[before]                            // from the compose base, in one cut
     inserted = Z[after)
     drop C if before and after are both empty, else emit it
     dE += ΔE; dR += ΔR
   ```
   X[before] has the bytes of the spec's `Y[minY, r1.after.begin) + r1.removed + ... + rk.removed +
   Y[rk.after.end, maxY)` (Y's kept bytes are X's bytes). The first version built it that way, which left one
   one-byte piece per coalesced backspace: a review measured 1000 pieces for 1000 backspaces, about 149 KB of
   history per such transaction, and 1000 extra pieces in the live tree after its undo.
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
    proof) and skips moves to an anchor's current offset: O(regions × (B log n + P) + k log k + r × B log n) for
    the k anchors in those ranges and the r moved ones, or a bulk rebuild in place of the r reinsertions
    (deviations). Of the k, those created after the restored set was taken (for example inside a paste whose undo
    collapses them) are rejected in O(1) by id (`AnchorSet::find`, which never allocates).
11. **History cap.** `maxTransactions` (default 1000, nodes excluding the root). Right after a node is created,
    while the cap is exceeded: if the root has two or more children, drop its oldest child subtree (by sequence)
    that does not hold the current node and repair the root's `redoChild` (its remaining child with the largest
    sequence); otherwise re-root: the only child becomes the root, keeps its id, stateId, children and
    `redoChild`, and drops its transaction data. The current node and its ancestors are never removed. A dropped
    subtree leaves the tree at once (an O(1) extraction per node) and is freed `kReleasedPerOp` = 4 nodes per
    later recorded change, undo and redo, so the keystroke that drops an old redo branch of up to 999 nodes no
    longer frees it all (1.2-1.3 ms before, 57-100 us now; measurements). After a drop of s nodes the cap is
    reached again only after s more new nodes, so at most one dropped subtree (at most `maxTransactions` nodes)
    waits at any time (U3), and B19's history estimate counts it (`historyBytes` includes it).
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
- **Heavy restores:** an undo that restores r anchors among n reinserts them one by one (about 13-15 us each at
  n = 101k on REF-W1) while r · 8 · ⌈log2(n + 2)⌉ <= n, else rebuilds the registry in bulk (about 0.1 us per
  anchor), so it costs at most about one bulk rebuild: 8.3-12.5 ms for 1000 restored anchors among 101k
  (measurements). m1/editor-view should not undo inside a frame budget it cannot exceed.

### Deviations from the slice spec
- **Left closed form.** The spec's batch closed form maps Left to `a_F + D_F` (F the first edit whose closed range
  contains x). That is wrong when edits touch: for [2, 4) → 3 bytes and [4, 6) → 0 bytes, x = 6 Left maps to 2
  sequentially (6 → 4 through the second edit, 4 → 2 through the first), but `a_F + D_F` gives 4 + 1 = 5. The
  implementation uses `a_K + D_K`, K the first edit of the run of touching edits (b_{i-1} == a_i) that ends at F;
  the Right form is the spec's. Found by `TextAnchor.BatchClosedFormMatchesSequential`, documented in anchor.hpp.
- **`withMoved` bulk path.** Besides per-move reinsertion (O(r B log n)), `withMoved` rebuilds all three trees in
  O(n log n) when `r · 8 · ⌈log2(n + 2)⌉ > n` (the index sorted by id once). Restores of thousands of anchors (a
  big deletion's undo) would otherwise cost milliseconds more.
- **Path thresholds with measured factors.** The spec's switches `m · ⌈log2(n + 2)⌉ > n` (sweep) and
  `r · ⌈log2(n + 2)⌉ > n` (bulk move) assume a splice step or a reinsertion costs about log2 n sweep entries; a
  review measured 100-250. `anchor_paths.hpp` holds the cost model with the fitted factors 16 (sweep) and 8 (bulk
  move); see "Path thresholds" under the measurements. Both paths give equal results, so only time changes.
- **Restore scan matching.** The scan matches the mapped set's anchors in each closed range with `restoreFrom`'s
  anchors in the same range by id; an anchor not found there is looked up with `AnchorSet::find`. Section 4.6
  argues the lookup never *finds* an anchor (a review's probe made that branch abort and it never fired in
  `text_tests` or the stress suite), but it does run: for every anchor in a closed range that `restoreFrom` lacks,
  which are exactly the anchors created after `restoreFrom` was taken (an older live anchor is in it, since ids
  are never reused). `find` rejects those in O(1) by comparing with `restoreFrom`'s largest id and never
  allocates; the first version used `resolve`, an O(B log n) seek plus a `std::format`-built NotFound error each
  (about 1-1.6 ms for 10k anchors inside an undone paste).
- **Interpretations.** Re-rooting seals the node that becomes the root (its data is dropped; the current node,
  never the root, stays open). Edits inside a group set no `selectionsAfter` (only the outermost `endGroup` does).
  `UndoState.canUndo` means "the current node is not the root" (undo may still be Busy). An empty commit is a
  cancel whenever the net range is empty, with or without an explicit empty `replaceInBase`.
- **Not unit-tested:** the kMaxAnchors (2^24) exhaustion error; creating 16.7M anchors per test run costs too much.
  Its check is one comparison in `Document::createAnchor`, ordered as in the table above.
- **Test-only accessors.** `AnchorSet::classTreeForTesting`/`indexForTesting` expose the trees for
  `DocumentPerf.UndoRedoLatency`'s memory report (unique nodes the history retains), and
  `AnchorSet::withMovedForTesting` forces either move path, like `transformedForTesting`; not public API.
- **Deferred freeing of dropped branches** (decision 11) is not in the spec; it keeps the cap's subtree drop off
  the keystroke that triggers it.
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
Two standalone runs of each case on the final sources (after the review fixes, 2026-11-01), the first while
nothing else was building; "Review follow-ups" below has the first version's figures where they changed.
- `DocumentPerf.TypingLatency` (16 MiB, half CRLF, 10k anchors, 100k ops, 85 % Typing / 15 % DeleteBackward, 50 ms
  per op; history at its 1000-transaction cap): **p50 19.1 / 18.7 us, p99 48.2 / 42.8 us, max 864 / 243 us** (B5
  sub-budget p99 250 us). For comparison `TextPerf.EditLatency` (bare TextBuffer) gave p50 9.7 us, p99 22.2 us in
  ADR 0013.
- `DocumentPerf.UndoRedoLatency` (200 runs of 1000 keystrokes, 50 runs of 1000 backspaces, a 64 KiB paste, a 4 MiB
  deletion; 504 undos and redos): **p50 20.5 / 12.7 us, p99 100.1 / 54.4 us, max 2144 / 1692 us.** Heavy ops:
  deletion undo 2144 / 1692 us (2450 anchors restored, bulk path), deletion redo 823 / 574 us (0 restored: the
  anchors collapse to the deletion point, which is where `anchorsAfter` has them), paste undo 28.8 / 16.7 us and
  redo 26.6 / 14.5 us (0 restored).
- Memory (unique SumTree nodes of slices and anchor sets, plus node and region records, the compose base and
  dropped nodes, beyond the live text and anchors; identical in both runs): as recorded, the 252 transactions
  retain 2,603,952 bytes, **10.3 KB per transaction**, so 1000 would hold 10.3 MB, 12.9 % of B19's 80 MB; a
  typing run 9.35 KB and a backspace run 9.42 KB. Every node also gains `anchorsAfter` at its first undo and a
  replaced `anchorsBefore` at each redo, so after undoing and redoing all 252 the history retains 5,299,432
  bytes, **21.0 KB per transaction**: 21.0 MB, 26.3 % of B19, for 1000. Most of it is anchor-set path copies
  (10k anchors).
- `DocumentPerf.AnchorsAndCarets` (report only): resolve with 100k anchors p50 0.2 us, p99 0.4 / 0.6 us; one edit
  collapsing 1000 anchors at distinct offsets 89 / 88 us, its undo (restoring them among 101k, bulk path)
  12.5 / 10.7 ms; the undo of a 64 KiB paste holding 10k anchors created after the pasted text (among 20k)
  715 / 797 us; after undoing 999 sealed transactions, six keystrokes of 17-26 us except the second, which drops
  the 999-node branch, at 77 / 100 us (979 dropped nodes are still waiting to be freed after the sixth); a
  backspace next to 10k anchors at one offset 36 / 41 us (first-gap shortcut); 100k adversarial inserts (each
  between the previous one and a fixed neighbour) 1.70 / 1.28 s with 13,011 relabels of 1,049,910 entries (10.5
  per insert, amortised O(log n)); a 10k-caret Typing batch 229 / 202 ms (edit, anchors,
  `SelectionSet::transformed`), its undo 81 / 79 ms.
- Debug (1 MiB, 20 typing and 5 backspace runs of 100, report only; `verify -Quick`): Windows typing p50 87 us, p99
  155 us, deletion undo 16.3 ms (2402 anchors), deletion redo 12.6 ms, history 13.7 KB per transaction as
  recorded (54 KB after undoing and redoing everything: among only 27 transactions the 4096-byte paste and the
  256 KiB deletion dominate), the branch-dropping keystroke 734 us.
- `AnchorSet.LabelExhaustionPatterns` (10k anchors): one tie position 1199 relabels of 85,935 entries,
  alternating head and tail 598 of 36,920, between two anchors 1199 of 85,936; each within the asserted bound
  64 · n · ⌈log2 n⌉.

### Test times and scales
| Configuration | `UndoDifferential.Quick` | `AnchorSet.MatchesNaiveModel` | `UndoProperty.*` (3) | `AnchorConcurrency` | all new cases | stress: UndoStress / AnchorStress |
| --- | --- | --- | --- | --- | --- | --- |
| Windows Debug | 6.8 / 11.4 s | 10.8 / 17.6 s | 11.0 / 16.8 s | 0.6 / 1.1 s | 37.8 / 60.3 s (75 unit cases 33.4 / 55.6 s) | not run (`-LE stress` in Debug) |
| Windows Release | 0.5 s | 1.2 s | 2.3 s | 0.1 s | 26.1 s (unit 4.9 s) | 1.4 s (500k) + ManyAnchors 2.1 s (100k) / 7.5 s (500k) |
| Windows ASan (RelWithDebInfo) | 10.6 s | 19.0 s | 37.6 s | 1.0 s | 161.1 s | 12.0 s (200k) + 1.9 s (25k) / 28.3 s (100k) |
| WSL Debug | 2.8 s | 9.6 s | 9.9 s | 0.7 s | 33.9 s (unit 27.2 s) | not run |
| WSL ASan + UBSan (Debug) | 11.6 s | 27.0 s | 44.8 s | 2.6 s | 233.7 s | 26.8 s (200k) + 39.6 s (25k) / 47.7 s (100k) |
| WSL TSan (RelWithDebInfo) | 3.4 s | 6.3 s | 14.6 s | 0.6 s | 53.4 s | 3.7 s (100k) + 3.1 s (20k) / 6.1 s (50k) |

"All new cases" sums this slice's 81 ctest cases (75 unit, 3 stress, 3 perf) where they run, on the final sources
(after the review fixes): Windows and WSL Debug from `verify -Quick`'s `LastTest.log` (ctest runs serially, but
other slices were building; Windows Debug gives the last run and a more contended one before it), Windows Release
from hidden gtest runs of the three test executables, and the sanitizer trees from `ctest` over `text_tests`,
`text_stress_tests` and `text_perf_tests`. The first version (68 unit cases) took 37.5 s in Windows Debug, from
`verify -Full`'s hidden gtest runs; the review fixes added seven unit cases and made
`AnchorSet.MatchesNaiveModel` cycle through the forced paths. Contention moves these figures more than the
changes do.
`UndoDifferential.Quick` stays inside its 15 s Windows Debug ceiling at the spec's settings (a full check every
32 ops, initial texts up to 4 KiB), so neither fallback of section 7.3 was needed. Its documents stay small: in
200k ops the largest text reached is 4168 bytes, and only 779 redos succeed (most redo attempts follow an edit
and find nothing to redo). Redo is checked against the oracle mainly by `UndoProperty` (which redoes through the
driver's oracle-checked `redoOnce`) and by the stress cases; `DocumentGroup.RollbackRestoresEarlierRedoChild`
covers the redo-child repair at rollback that only `UndoProperty` caught before. `AnchorSet.MatchesNaiveModel`
(200 seeds × 2000 ops) runs its seeds on up to four threads, comparing sizes after every op, `all()` with the
naive model every 10 ops, and `resolve` and `checkInvariants` every 100 ops and at the end. Every stress case
stays under its 120 s ceiling, and `AnchorConcurrency` and `AnchorStress` are clean under TSan.

**B28** (`verify -Quick` p50 5 min). Every `-Quick` run of this slice had other slices building beside it, so
these are upper bounds: 357.8 s for the first version, 256.8 s after the review fixes (with their rebuild;
Windows Debug ctest 142.8 s for 516 cases) and 184.2 s for the final tree with nothing to rebuild (Windows Debug
ctest 91.2 s, of which the slice's 75 unit cases 33.4 s; WSL Debug ctest 57.0 s, the slice's 27 s). B28 should be
re-measured on a quiet machine before M1 exit. If it is over 5 min, the candidates for the `stress`
label are `AnchorSet.MatchesNaiveModel` and the three `UndoProperty` cases; moving them out of `-Quick` weakens
`-Quick`, so it needs the user's approval.

Scales: `UndoDifferential.Quick` runs 200 seeds × 1000 ops on up to 4 threads (piece caps {4, 5, 7, 16, 4096},
history caps {1000, 8, 1}, a small buffer limit for one seed in seven, initial texts up to 4 KiB, a full check every
32 ops); `UndoStress.Differential` 500k ops optimised, 200k under ASan, 100k under TSan and in Debug (seeds of 20k
ops from 2,000,000, a full check every 64); `UndoStress.ManyAnchors` 100k ops optimised, 25k under ASan, 20k under
TSan and in Debug (seeds of 2500 ops from 3,000,000, 300 or 1500 tracked anchors, up to 200 or 2000 churn anchors
and 40 or 400 carets, a full check every 64); `AnchorStress.ConcurrentReaders` 500k writer ops optimised, 100k
under ASan, 50k under TSan and in Debug. Failing `UndoStress.Differential` seeds print a command that re-runs them
through `UndoDifferential.Quick`; failing `ManyAnchors` seeds print their seed and anchor settings.

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

### Path thresholds (REF-W1, Windows Release, forced paths)
A probe timed `transformedForTesting` (m one-byte inserts spread over the text) and `withMovedForTesting` (r
random anchors to random offsets) with both paths forced, at sets of n random anchors, stepping m and r by 1.25x,
and reports the first m or r where the per-step path is slower, as the factor n / (m · ⌈log2(n + 2)⌉). Two runs:

| n | transform crossover m (factor) | move crossover r (factor) |
| --- | --- | --- |
| 1,000 | 5 / 5 (20.0 / 20.0) | 8 / 10 (12.5 / 10.0) |
| 10,000 | 41 / 51 (17.4 / 14.0) | 78 / 78 (9.2 / 9.2) |
| 100,000 | 366 / 293 (16.1 / 20.1) | 713 / 713 (8.3 / 8.3) |
| 1,000,000 | 2715 / 3393 (18.4 / 14.7) | 6626 / 5301 (7.5 / 9.4) |

So the factors are 16 (sweep) and 8 (bulk move) across three decades; the first version's factor 1 kept the
per-step paths up to 15x (transforms) and 8x (moves) past the crossover (at n = 101k: m and r up to 5941).
`AnchorSet.PathThresholdsFollowMeasuredCrossovers` pins the model to these crossovers. The index update after a
relabel (`updateIndexLabels`) keeps factor 1; relabels touch few entries (amortised O(log n)).

With the factors, sets of up to about a hundred anchors take the sweep and bulk paths for almost every batch, so
the unit differential (at most 64 churn anchors) rarely splices. Both paths stay covered: `AnchorSet.MatchesNaiveModel`
cycles through the default choice and both forced paths, `SpliceAndSweepAgreeAlongChains` and
`PerMoveAndBulkAgree` compare forced paths along chains of operations, and the stress case `UndoStress.ManyAnchors`
runs the Document differential with 300 or 1500 tracked anchors plus up to 200 or 2000 churn anchors (about 1600
live at most) and up to 400 carets, where splices and per-move restores are the common case.

### Review follow-ups (2026-11-01)
Two independent reviews of the first version (81afd3a) found no correctness defect; their performance and
memory findings were fixed as follows (before / after, REF-W1 Windows Release, the reviews' own probes rebuilt on
the fixed sources):
- **Delete runs fragmented the history** (medium). 1000 coalesced backspaces kept a removed slice of 1000
  pieces, now 2; 10,000 backspaces 10,000 pieces, now 4; the document after undoing 1000 backspaces had 1025
  pieces, now 27 (25 before the run). History per transaction of 1000 backspaces 148.6 KB, now 1.6 KB, like
  typing. Fix: decision 5's compose base. `DocumentUndo.DeleteRunsKeepWholePieces`;
  `DocumentPerf.UndoRedoLatency` now records 50 backspace runs and reports their history separately.
- **Path thresholds** (medium). The undo restoring 1000 scattered anchors among 101k took 25.6-29.3 ms on the
  per-move path, now 8.3-8.8 ms in bulk (`DocumentPerf.AnchorsAndCarets`: 21.0 / 30.6 ms, now 12.5 / 10.7 ms).
  Fix: "Path thresholds" above; `AnchorSet.PathThresholdsFollowMeasuredCrossovers`.
- **The cap's subtree drop** (low). The keystroke that drops a 999-node redo branch took 1164-1340 us, now
  57-100 us. Fix: decision 11's deferred freeing. `DocumentUndo.HistoryCapDefersDroppedBranch`; reported in
  `DocumentPerf.AnchorsAndCarets`.
- **The restore scan's fallback** (low). Undoing a 64 KiB paste with 1000 anchors created inside it took
  193-249 us, now 89-97 us; with 10,000, 2.1-3.3 ms, now 1.16-1.18 ms. Fix: `AnchorSet::find` (deviations).
- **History memory after navigation** (low): reported now. Every node also holds `anchorsAfter` once undone and a
  replaced `anchorsBefore` once redone; after undoing and redoing all 252 transactions the history retains about
  twice its recorded size (measurements).
- **Documentation** (low): the Sources' threshold formula, `anchor_set.cpp`'s provenance wording, the restore
  scan's cost, the `TODO(M11)` for chronological traversal and the undo-tree view (document.hpp), and the
  differential's scale below.
- **Redo-child restoration at rollback** (low, test gap): `DocumentGroup.RollbackRestoresEarlierRedoChild`
  catches the review's mutation "rollback ignores the previous redoChild", which only `UndoProperty` caught.
- **B28** (low): reported under the test times; to be re-measured on a quiet machine before M1 exit.

Mutation checks of the fixes (Windows Release `text_tests`, each reverted):

| Mutation | Caught by |
| --- | --- |
| build removed slices by concatenating the regions' pieces (the first version, in X coordinates) | `DocumentUndo.DeleteRunsKeepWholePieces` |
| coalesce against Y instead of the compose base | an assertion abort (`before.end <= xTree bytes`) in `DocumentCoalesce.BackspaceAndDeleteRuns` |
| keep the compose base at a coalescing break, at endGroup or for a new node | U8 in 5 cases each, e.g. `UndoDifferential.Quick`, `UndoProperty.*`, `AnchorConcurrency.ReadersResolveDuringEdits` |
| keep the compose base at undo | 13 cases, e.g. `DocumentGroup.Rollback`, `DocumentIme.AnchorsAroundPreedit`, `UndoCompose.Cases` |
| keep the compose base at redo | none: an equivalent mutation. Redo is refused while a group is open and needs a child of the current node, and a node that accepts edits never has one, so the base is already dropped; the call is defensive |
| path factors back to 1 | `AnchorSet.PathThresholdsFollowMeasuredCrossovers` |
| free dropped subtrees at once | `DocumentUndo.HistoryCapDefersDroppedBranch` |
| rollback ignores the previous redoChild | `DocumentGroup.RollbackRestoresEarlierRedoChild`, `UndoProperty.RedoThenUndoIsIdentity`, `ChangesReplayAsLsp` |
| `find` never finds | 16 cases, e.g. `AnchorSet.FindIsResolveWithoutAnError`, `DocumentUndo.AnchorsRestoredOnUndoOfDeletion` |

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
  t_i = ⌊(2/T)^i⌋ = ⌊(4/3)^i⌋ with T = 1.5 on the count, even spreading). The paper itself was not opened during
  this slice; it is cited as the algorithm's origin, and `anchor_set.cpp` says the code follows the spec's
  description.
- The existing frost::text `SumTree` (ADR 0013 and its sources): the anchor trees and the index are
  `SumTree<.., 8>` instances with monotone seeks.
- No editor's undo or anchor implementation was consulted: not Zed's text, rope or editor crates, Qt Creator,
  KDDockWidgets, JetBrains or Visual Studio sources, Emacs `undo-tree.el` or Vim's `undo.c`. Vim's `:help
  undo-tree` and CodeMirror 6's reference manual (allowed) were not read either; the undo tree of states is a
  generic concept and the design follows the slice spec.
- No function is a port of upstream code, so no NOTICE entry is needed. No dependency was added.
