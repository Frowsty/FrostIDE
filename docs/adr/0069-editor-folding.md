<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0069: Editor folding

## Context

The prior inventory described a display-fold layer as existing. The inspected baseline has its
interfaces, but they implement identity mapping only; only Lua supplies syntax fold candidates.
Interactive folding therefore also requires text projection, coordinate mapping and a C++ provider.

## Accepted design

Collapsed ranges belong to a view and use buffer byte coordinates. Nested ranges are retained
while an outer range is collapsed. Visible ranges project their contents to an ASCII placeholder;
wrap, hit testing and rendering consume the same projected segments. Document edits transform
the stored boundaries with the existing raw-edit anchor rules. A reveal operation removes collapsed
ancestors of a navigation target. Fold state is view state, not document text or undo data. The
project session records ranges with a content fingerprint and ignores them when the document has
changed. The editor factory also retains folds when a file is closed and reopened in the same run.
The current candidates come from local tree-sitter queries for C++, Lua, C# and CMake; no LSP
folding provider has been implemented. Queries run on the user-visible scheduler lane.

The projection now uses a persistent summary tree of identity runs and visible placeholder runs.
Its items cache buffer and projected text summaries, so coordinate seeks use tree paths and chunk
iteration retains the owning snapshot. A separate gap-based endpoint tree indexes nesting, and an
anchor registry carries stable endpoint identities across raw edit batches. Visible edits splice
changed runs; edits strictly inside a surviving placeholder do not request a wrap patch. Placeholder
endpoints map exactly; strict interiors use the requested bias. Blocks inside hidden text attach to
the placeholder line while retaining their source anchors.

The baseline requires logarithmic coordinate mapping, incremental projection patches and no
reshaping of unrelated lines. B5's latency targets are unchanged; they have not yet been measured
for this feature. Single projection seeks cost O(log r) in the number of runs, apart from source
snapshot operations. Arbitrary offset mapping through an m-edit batch costs O(m) setup and O(log m)
per lookup. Endpoint queries visit indexed paths and their results; resolving each returned anchor
costs O(log f). Consumed fold groups use one interval splice, and mixed edits batch hidden endpoint
changes once per coordinate group. Each affected anchor and record still uses tree operations;
visible fold restoration is individual. No stronger O(log f + k) metadata formula is asserted as a
baseline rule. Defensive recovery can materialize all folds; its remaining limits are recorded below.

The editor uses indexed endpoint queries for gutter, reveal and edits. After a text change it
revalidates grapheme boundaries on affected lines, removing a fold if its endpoint is no longer a
cluster boundary. Syntax jobs accept stop tokens and check cancellation while ordering candidates
and building the candidate index; current jobs are replaced on text or syntax changes. The local
C++, Lua, C# and CMake providers are syntax queries, not LSP folding. Folding commits active IME
composition before changing projection; selection replacements defer folding until fresh syntax
candidates arrive. Programmatic commits also clear keyboard composition routing. Persisted view
and history columns use a separate grapheme/display-column navigation API, preserving byte-column
semantics for existing external callers.

## Review and remaining limits

The defensive whole-projection recovery remains reachable in code. It is called explicitly if an
intermediate or final projection exceeds the 32-bit display-coordinate limit, or if an individual
removal fails. The general rebuild is reached if a surviving fold cannot be restored, or if an edit
cannot be represented by the local run path after touched endpoints have been removed. Astra review
found no ordinary valid-edit trigger for these non-overflow paths: monotone mapping preserves
nesting, duplicates are filtered, and removing touched endpoints leaves edits inside runs.
Exhaustive small replacements and seeded batches cover shared/coalesced endpoints, disjoint edits
and CRLF seams without materializing all folds; these tests are not a formal proof for all inputs.
A transient expansion during removal near the coordinate limit is a separate risk even
when the final projection would fit; the recovery may discard folds to remain representable.

The grouped update and survivor-anchor tests received independent review. Numerical B5 targets
(p50 3 ms and p99 8 ms) remain unchanged and unmeasured for this feature. Broader stress, fuzzing,
sanitizer and controlled performance validation remain deferred under the feature-first agreement.

## Verification

Astra approved worker tree `96fc0f4dbb02c98cbeccffe51ff32f046506d7b7`, including the final IME
correction. Focused Windows and native Linux tests cover projection, editing, IME and session/history
restoration; Windows also compares folded raster and WARP frames. The combined main tree passed
2738/2757 Windows tests (19 skipped, one additional disabled) and 2611/2635 native Linux tests
(24 skipped). The new Linux skip is the Windows renderer-parity case, not an omitted folding model
test. Formatting, lint, licence and feature-matrix checks passed. The staged Windows application
passed startup, actual Direct3D 11 rendering and bundled ConPTY input smoke tests. These are
functional acceptance results for this slice, not completion of M15 or the whole parity goal.
