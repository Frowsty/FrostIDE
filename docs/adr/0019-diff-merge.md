# ADR 0019: Histogram diff and three-way merge cores (frost::diff)

- Status: accepted for the M1 slice `m1/diff-merge`. No PLAN budget is owned or changed; `tests/perf/budgets.json`
  is unchanged. Derived sub-budgets of B18 and B21 are recorded here (D15), as ADR 0013 did for B5.
- Date: 2026-11-01
- Plan reference: PLAN.md section 3.2 (layout: "git, diff, merge" in `src/vcs`; amended by D1), section 3.5
  (malicious repositories: bounded work), budgets B18 (openDiff visible) and B21 (no stall above 50 ms), M1's
  save-manager (external-change reload and 3-way merge) and shell-nav (compare view), M2's `frost diff` /
  `frost merge`, and M5's exit criterion "Diff output matches `git diff --histogram`".

## Context
Several M1 slices need a line diff and a three-way merge before M5 brings git: the save-manager reloads a clean
buffer as one minimal undoable edit and merges an external change into a dirty one, and the shell's compare view
shows hunks and intra-line changes. M5 then promises git-compatible histogram output. So the core has to:
- produce git's histogram hunks (at least for the git dialect that is installed in WSL, and with a documented rule
  for the newer dialect), with intra-line refinement and a unified renderer;
- merge three texts with diff3's chunking (Khanna, Kunal and Pierce), expose a resolution model, and turn a
  resolution into `text::Edit`s that `TextBuffer::applyEdits` takes as one undoable edit;
- stay bounded on hostile input (a work cap with degraded but valid results, cancellation), within B21;
- be Qt-free and usable from every layer at rank 4 or above.

git's xdiff (GPL-2.0) and libxdiff (LGPL-2.1) are off limits (CLAUDE.md clean room). The algorithm is JGit's
BSD-3-Clause `HistogramDiff`, cited and implemented in our own structure (D3); git's behaviour was established as a
black box.

## Decision
1. **D1, placement and layer.** A new Qt-free library `frost::diff` (target `frost_diff`, directory `src/diff`),
   declared `LAYER text` (rank 4) with `DEPS frost::core frost::text`; `FrostLayers.cmake` is unchanged (layers do
   not follow directory names). The consumers sit at ranks 4 to 7 (save-manager, shell-nav, M2's CLI, M5's vcs),
   so `src/vcs` (rank 7) was out of reach; a new "diff" layer at rank 4 could neither use frost::text nor be used by
   text-layer targets; code inside `frost_text` would grow the CMake file three parallel slices edit and make every
   text user link diff code. `src/vcs` keeps the git CLI in M5 and consumes frost::diff. PLAN 3.2's layout line
   needs the amendment "src/diff/ histogram diff, 3-way merge cores (ADR 0019)" (left to the integrator).
   Consequence for m1/save-manager: FrostLayers forbids edges between different layers of equal rank, so a target
   that links both frost::storage (storage, rank 4) and frost::diff (text, rank 4) needs rank 5 or more.
2. **D2, line model.** `LineSequence` is an immutable byte copy (heap storage behind `std::shared_ptr<const ...>`,
   so copies are O(1) and views survive moves) with a `uint32` line table and a 64-bit FNV-1a hash per line. Two
   splits: `Lsp` (LF, CR and CRLF each end a line once; line i equals `text::Snapshot` line i with its break) and
   `Lf` (only LF; what git and patch tools see). Lines compare by their bytes including the break, so CRLF and LF
   lines differ, as in git. Any bytes are allowed (invalid UTF-8, NUL). `SequenceLimits` (64 MiB and 2^21 lines by
   default, both checked before anything is allocated; `fromSnapshot` checks in O(1) from the summary) send larger
   texts to large-file mode. Under Lsp, `size()` is `Snapshot::lineCount() - 1` when the text is empty or ends with
   a break, else `lineCount()`; byte offsets equal Snapshot offsets. Memory: the bytes plus 12 bytes per line.
3. **D3, histogram.** The algorithm of JGit's `HistogramDiff`/`HistogramDiffIndex` (BSD-3-Clause, cited in
   `histogram.cpp` and NOTICE) in our own structure: `RegionIndex` (each region's A part indexed by one forward pass
   that appends positions to per-line chains through a tail per line, counts saturated at 255, a generation per id
   instead of clearing), `AnchorSearch` (walks B, grows and ranks candidate runs) and a driver over an explicit
   stack. Provenance: an early draft of this slice followed JGit's `tryLongestCommonSequence` in structure, local
   names and statement order (review 1). It was replaced by the structure above, and the slice's history was
   squashed before merge, so no commit carries that draft (Q2 (b)). The observable rules are unchanged, by design:
   every golden and every git count is identical (21/9/14 git-fewer cases in the differential, 213/1 in stress).
   **Reuse** (the feature-first revision): three shortcuts give exactly the plain rules' flags with less work, for
   edits at evenly spaced lines, where the best anchor is the first of many equal runs and the plain rules re-index
   and re-grow nearly the whole region once per edit (O(N x D); 10k lines with every 10th edited needed 2.54x the
   cap). (1) When the next region's A part is a suffix of the indexed one, the index drops the leading positions
   instead of being rebuilt; counts are kept exact and saturated only when read, so the narrowed index equals a
   rebuilt one. (2) A region of at most 64 line pairs is first checked for a common pair directly; with none it is a
   replace (what the search finds) and the index stays intact. (3) A run of rarity 1 grown from (i, j) is cached per
   B position; a later region that tries (i, j) and contains the whole run reuses it, because the regions whose B
   part holds j are nested, a run maximal in an enclosing region and inside the smaller one is maximal there, and
   its rarity stays 1. With reuse that shape costs about O(N + D^2). `Histogram.ReuseIsExact` compares both settings
   over every generator class. The rules: candidate counts start at 65, records with a count above the current one only mark that a
   common line exists, occurrences are tried from the earliest, matches are extended both ways within the region, a
   candidate replaces the best when it is longer on A's side or rarer, and a region with common lines but no
   candidate at a count of 64 or less goes to Myers (`fallbacks`). Unlike JGit's `DiffAlgorithm.diff`, the whole
   input is one region with no common prefix/suffix trimming: git does not trim, and trimming changed 30-57 of 1000
   results in the prototype. JGit's abort when 64 distinct elements share a hash bucket does not exist here (ids are
   exact, there are no buckets); the black-box parity shows no effect. State is allocated once per call; regions wait
   on an explicit stack ("before" popped first). A queued 1 x 1 region is always a replace: a queued region is the
   part before or after a common run that was grown backwards from (i, j) while `a0 < i && b0 < j && A[i-1] ==
   B[j-1]` (forwards symmetrically) within the same bounds; a 1 x 1 "before" region has `i = a0 + 1` and `j = b0 +
   1`, so the bounds held when growing stopped, which means `A[i-1] != B[j-1]`, its one pair; the "after" case is
   the mirror image. Work: one unit per scanned or dropped A element, B position, extension comparison, visited and
   skipped occurrence, and line pair of the small-region check; a reused run costs no extension steps.
4. **D4, Myers.** Myers 1986, section 4b: linear-space middle snake, divide and conquer on an explicit stack, the
   common prefix and suffix of each subproblem stripped first. It emits a minimal script (checked against an LCS
   DP on 20k cases). It serves null histogram regions, `Algorithm::Myers` and the token diff of refinement. The
   prototype's fallback was a DP LCS that drops from A first, not Myers; switching tie-breaks changed 0/300
   sparse-fallback and 21/150 dense-fallback results after compaction. With this Myers, sparse parity holds
   (300/300 on git 2.43); dense cases differ from git in equal-cost scripts only (17/150), so they are compared by
   totals (D6).
5. **D5, sliders (`Sliders::GitClassic`).** Our own behavioural formulation, verified against git as a black box:
   on change flags, one pass over side A against side B's flags, then one pass over side B. Each group slides up
   as far as equal lines allow, absorbing adjacent groups after every step; then down, absorbing and re-checking
   alignment after every step, repeating while it grows; it ends at the lowest placement that aligns with a change
   on the other side (by the other side's gap counts), else the lowest placement. The passes run once each:
   re-running is not a fixed point (A=011 B=001: git prints [(1,1,1,2),(2,3,3,3)], the single-pass result; a second
   run of both passes gives [(1,2,1,2)]), because pass A ran against side B's raw flags, which pass B then changed.
   Pass B is a fixed point of the final flags, which `checkDiff` checks as H4 (a consistency check; pass A is
   covered by goldens and the git differential). Each slide step and absorbed line costs one unit; on exhaustion
   the flags stay a valid script and the result is degraded. The indent heuristic and git 2.54's final clean-up
   are M5's (TODO(M5)).
6. **D6, git dialect policy.** git 2.54 changed histogram output (RelNotes 2.54.0, "yc/histogram-hunk-shift-fix
   later to maint"; maint releases may carry it, so the dialect is probed, never inferred from the version).
   `GitRunner::probeDialect` runs 12 canary pairs (`tests/diff/goldens/histogram/dialect`) at -U0: all equal to
   the classic answers means Classic, all equal to the new ones means New, anything else fails with "unknown git
   dialect". Classic (git 2.43 in WSL): every case's -U0 hunks equal ours, except DenseFallbackMinimal, where only
   the deleted + inserted totals must be equal. New (git 2.56 on Windows): each case is exact, or git's total is
   lower (git-fewer, printed with its seed). The rule is two-sided on both dialects (review 2): git's total above
   ours fails, an equal-cost case with different hunks fails outside DenseFallbackMinimal, and the git-fewer count
   of each class may be at most twice the count measured on git 2.56 for the same cases (21 of 2000 adversarial,
   9 of 300 mid-frequency, 14 of 300 sparse fallback, 0 elsewhere; in stress 213 of 20000 adversarial and 1 of
   5000 code-like, plus 2 because sanitised runs draw other cases), so a change that makes our hunks coarser
   fails on Windows too. Matching the new dialect is M5's (question Q3 asks which git defines it).
7. **D7, work cap and degraded results.** One `WorkBudget` per `diffLines` call (`DiffLimits::maxWork`, default
   `kDefaultMaxWork` = 2^22, D15) shared by interning, histogram, Myers and compaction; the stop token is checked at
   entry and every 4096 units (Cancelled is an error). On exhaustion the current and every queued region (or Myers
   subproblem) becomes a whole replace, compaction stops, and `degraded` is set; the result always passes H1-H3,
   and is deterministic for given inputs, options and budget. The interner counts `kInternUnitsPerLine` = 8
   units per line, one per extra probed slot and one per 64 bytes compared. The probes count because FNV-1a is not
   collision-resistant: crafted colliding lines (PLAN 3.5) cost probes, so the cap stays a real bound
   (`Limits.InternerFlooding`). The 8 units per line put interning on the time scale of the other phases (a line
   costs 12-27 ns to intern at 100k lines and about 46 ns at 2^21, a unit of the other phases 1.1-5.4 ns): at one
   unit per line, a pair of 2^21-line sides fit their whole interning into the cap and took 191-201 ms (review 1);
   now the budget runs out after 2^19 lines and the call takes 13-16 ms (`DiffPerf.MaxLinesBounded`). The
   interner's table and the id arrays are sized for the lines the remaining budget can pay for, so a huge input
   under a small cap neither allocates nor clears memory it never uses. The price was 16 units per line pair of a
   diff and 24 per line triple of a merge, which collapsed mid-size inputs that used to be exact into one
   whole-file hunk (200k-300k lines with 10 edits) or one whole-file conflict (a 150k-line merge with 5 edits per
   side). The **partner path** (feature-first revision) takes that back without cheating the time scale: each line
   of the later sequences (B; ours and theirs) is first compared with its expected partner in the first sequence
   (the line after the last one it matched, re-aligned after a table hit to the line after that id's first
   position there); equal hashes and bytes take the partner's id for 1 unit plus the compare units (two sequential
   reads, no table access), and everything else goes through the table as before. The ids are identical
   (`Interner.PartnerPathIsExact`), and a lightly edited diff now costs about 9 units per line pair and a merge 10
   per triple. The first sequence still costs 8 per line, so a pair at the line limit still runs out while
   interning A (`DiffPerf.MaxLinesBounded` is unaffected). Exhaustion while interning gives
   one whole-input hunk (none for byte-equal sides). `merge3` charges the interning of its three sequences to one
   budget and gives each of its two diffs `floor(R/2)` of the remainder R: equal budgets keep merge(O,A,A) = A and
   merge(O,O,B) = B under every cap, and the whole merge within one maxWork. Degraded merges are sound: a degraded
   diff only loses matches, so chunks only get coarser, and a coarse chunk is clean only if a side equals base or
   both sides are equal over the whole chunk; a true conflict never becomes clean (`Merge.DegradedSound`).
   `maxLines` (2^21) is checked first, in O(1), with OutOfRange.
8. **D8, refinement.** Word tokens are maximal runs of `[A-Za-z0-9_]` and bytes >= 0x80, maximal runs of blanks,
   one token per break (CRLF one token) and one per other byte; Char tokens are well-formed UTF-8 code points, each
   invalid byte alone, CRLF one token (grapheme clusters wait for utf8proc, TODO(M5)). A hunk side above
   `maxScanBytes` (16 MiB) is one coarse pair without tokenising; the common token prefix and suffix are trimmed;
   a middle above `maxBytes` (64 KiB) on either side, or a token diff that exhausts `maxWork` (2^20), is one coarse
   pair over the middle; otherwise token Myers runs on interned token ids and each pure deletion or insertion run
   slides down while the token after it equals its first token, keeping one unchanged token before the next run.
   Pairs are absolute byte ranges, sorted and disjoint, and applying them turns A's hunk bytes into B's. Only the
   bytes from a certain token start before the first difference to one after the last are tokenised (the window,
   `detail::RefineScan`): tokens outside it are common prefix/suffix tokens on both sides, so the result is the
   full scan's (`Refine.WindowEqualsFullScan`, which also caught a seeded mutation of the start rule), and an 8 MB
   line with one change costs a memcmp. A line that differs near both ends has a window as large as the line, so
   the size guard runs first, on bytes: the token middle contains the byte middle (common tokens consist of common
   bytes), so when the byte middle exceeds `maxBytes` on either side the result is the coarse pair (or a pure
   insertion or deletion when the other side's token middle is empty), and only the middle's two edges are
   tokenised to find its token bounds. With both byte middles non-empty the byte suffix bounds the token suffix;
   otherwise the full scan's clamp does. A 15 MiB line differing in its first and last bytes took 109-154 ms
   (16 MiB, review 1) and now takes microseconds (`RefinePerf.EdgesOnly`); `Refine.CoarseAtBothEnds`, the window
   test (half of its cases now with tiny limits) and the fuzz target check that the result equals the full
   scan's. A start is "certain" when it is judged from common bytes alone: for Char, any byte that is no
   continuation byte, and also a continuation byte none of whose three predecessors (all common) is a lead byte
   0xC2-0xF4, since only a sequence led from there can cover it. Without that second rule a run of continuation
   bytes (binary data, Latin-1 text in 0x80-0xBF; one token per byte) had no certain start, so the window grew to
   the line's ends and every byte was tokenised: Char refinement of 7 MiB runs around a one-byte change took
   177 ms, around a 128 KiB middle 139 ms, and takes about 1 ms with it (`RefinePerf.ContinuationRuns`, found
   while acting on review 1; `Refine.ContinuationRunsWindow` checks it against the full scan).
9. **D9, unified format.** Hunks join when the gap between them is at most 2 x context; a group spans the context
   clipped to the file; headers are `@@ -R +R @@` with R = `s+1` for one line, `s,0` for none, else `s+1,n`; lines
   are prefixed with space, `-` or `+`; a line without a break is followed by `\ No newline at end of file`.
   Identical inputs render nothing. With both headers set, `--- <old>` / `+++ <new>` come first, CR, LF and
   backslash escaped as `\r`, `\n`, `\\` (one header line always; git's quoting is TODO(M2)). Funcname context is
   out of scope (TODO(M5)). Patch producers use `LineSplit::Lf`: with Lsp, lone-CR lines are not lines to LF tools.
10. **D10, diff3 chunking.** Intern the three sequences, diff base against ours and theirs, and walk both match
    maps: maximal runs matched consecutively in all three are Stable; the region up to the next base line matched
    in both is an unstable chunk, classified by id equality as Stable (coalesced), Theirs (ours equals base), Ours
    (theirs equals base), Both (ours equals theirs) or Conflict. No conflict trimming (zealous diff3 is M11's).
11. **D11, markers and joining.** `renderWithMarkers` writes diff3-style markers (`<<<<<<<`, `|||||||`, `=======`,
    `>>>>>>>` with labels) for unresolved conflicts, ending marker lines with `eol()`: the option, else the first
    line break of ours, theirs or base, else LF. The marker size (`MergeModel::markerSize`) is the smallest n from
    minSize (clamped to 7-32) up to `kMaxMarkerSize` = 64 that is not the exact length of a run of one marker
    character starting a line of the three sides or of the text of a chunk taken as Custom (runs are maximal, so a
    longer run collides with nothing). A parser that takes a marker line to be exactly n marker characters followed
    by a space or the line end then never mistakes content for a marker, so marker-like content round-trips
    (`Merge.MarkersRoundTrip`, `Merge.MarkersAfterCustomResolution`, the fuzz target, also after custom
    resolutions). The cap bounds every marker line: the first version used one more than the longest run, so a
    1 MiB line of `=` made each of the four marker lines of every conflict 1 MiB long, and a custom text was not
    counted at all (review 1, `Merge.MarkerSizeBounded`). When every size up to 64 collides, `renderWithMarkers`
    fails with OutOfRange instead of writing ambiguous markers (`Merge.MarkerSizeSkipsTakenLengths`); the merge
    view still resolves such a merge, and `resolvedText` is unaffected. Labels are escaped like unified
    headers; an empty label gives the bare marker. Joining: when an emitted piece ends without a break and more
    output follows, `eol()` is inserted; only a side's last line lacks a break, so for line pieces this happens only
    in the final chunk (OursThenTheirs, TheirsThenOurs, markers); a custom text without a final break is joined to
    whatever follows it. Lsp caveat: a piece ending in a lone CR followed by a piece starting with LF fuses into one
    CRLF break in the output bytes; the bytes are still exactly the concatenation. Question Q5 asks whether diff3
    style is the right default and which labels the save-manager prompt shows.
12. **D12, resolution model.** `MergeModel` owns the three sequences and one `Take` per chunk (defaults: Ours, Theirs
    and Both take their change, Conflict is Unresolved, Stable reports Ours). `resolve` validates (Stable chunks
    cannot be resolved, Unresolved only for conflicts, custom text only with Custom; empty custom text deletes the
    chunk). `resolvedText` refuses while a conflict is unresolved; `MergeOutput` carries one output byte range per
    chunk for the merge view. `editsForOurs` returns a move-only `OursPatch`: O(1) copies of the sequences, a heap
    arena for custom texts and inserted breaks (complete before any view is taken) and sorted, non-adjacent edits in
    ours' byte coordinates, none for a chunk whose output equals ours, so the merge lands as one undoable
    `applyEdits` and the patch outlives the model. `toEdits` is the same for a two-way diff. Both document the apply
    precondition: the buffer's text must still be the sequence's bytes (compare `TextBuffer::version()`).
13. **D13, hashing.** FNV-1a 64 (public domain, draft-eastlake-fnv) per line, computed at construction; the
    interner compares hash, then length, then bytes, and counts probes (D7), so collisions cost work but never
    correctness. XXH3 can replace FNV once m1/storage brings the xxhash port.
14. **D14, concurrency.** `LineSequence` is immutable and every member is safe concurrently; `diffLines`, `merge3`,
    `refineHunk` and `renderUnified` are pure and reentrant over shared sequences (`Concurrency.SharedSequences`
    runs 8 threads, clean under TSan). `MergeModel`'s const members are safe concurrently; `resolve` needs exclusive
    access. `OursPatch` may move to another thread.
15. **D15, sub-budgets, the cap and memory.** Asserted only with `kAssertBudgets` (Release, unsanitised); Debug and
    sanitizer builds run a tenth of the scale and report. `DiffPerf.Typical`: a 10k-line code-like file (about 350
    KB), 1-100 edits, 200 samples; timed two LineSequence builds, `diffLines` and `renderUnified`; p99 <= 5 ms (1/20
    of B18). `MergePerf.Typical`: three builds and `MergeModel::create`; p99 <= 10 ms. `DiffPerf.LargeNotDegraded`:
    100k lines, 1000 edits; `diffLines` undegraded and <= 50 ms. `DiffPerf.Pathological`: P1-P7 (100k lines; P6 and
    P7 an 8 MiB line) <= 50 ms each with default limits, timed `diffLines` plus `refineHunk` for P6 (precise) and P7
    (coarse). `MergePerf.Pathological`: merge3 over P2 and P5 triples <= 50 ms. The cap: the largest power of two at
    which every P case stays within 50 ms and LargeNotDegraded is undegraded, measured directly by
    `DiffPerf.WorkCapSweep`: 2^23 on a quiet machine, 2^22 beside parallel builds; **2^22** is used so the
    assertion holds in every run (Measurements). Memory per call is printed by `DiffPerf.PeakMemory`. Added after
    review: `DiffPerf.MaxLinesBounded` (2^21 lines per side, every 1000th edited, <= 50 ms under the cap, degraded),
    `DiffPerf.EvenlySpaced` and `MergePerf.EvenlySpaced` (edits at evenly spaced lines, the histogram's O(N x D)
    case: <= 50 ms under the cap; whether they degrade and how much work they would need is printed, and the
    uncapped merge must be clean) and `RefinePerf.EdgesOnly` (a 15 MiB line differing at both ends, <= 50 ms per
    granularity). Statistic: each timed case except the Typical p99s asserts the best of its 3 or 5 runs and
    prints the worst beside it. B21 is a property of the code on REF-W1; verification runs beside other slices'
    builds, where one run's scheduler stall says nothing about the code. The worst runs are recorded in
    Measurements, and none exceeded 50 ms in the runs after review. What the cap means for real inputs, after the
    feature-first revision (D3's reuse and D7's partner path; work units, which do not depend on the build, from
    `DefaultCap.*`): a 10k-line source-like file needs 0.03x the cap with 10 scattered edits, 0.05x with 300 or with
    every 40th line edited, and 0.42x with every 10th (2.54x before); 10k-line merges with 200 scattered edits per
    side need 0.07x, every 40th line per side 0.09x (1.27x and one conflict before), every 20th 0.24x; a 200k-line
    diff with 10 edits needs 0.61x and a 150k-line merge with 5 edits per side 0.60x (both collapsed before). Very
    dense edits still degrade within the cap (every other line of a 10k-line file, `DefaultCap.BeyondTheCapStillDegrades`).
    These revisions only remove work, so the timed sub-budgets above should hold; they were not re-timed in the
    feature-first phase.
16. **D16, tests and oracles.** Independent oracles: an LCS DP (Myers minimality, refinement minimality), three
    appliers for the apply property (`TextBuffer::applyEdits`, a byte patcher over the hunks, a POSIX unified
    applier over `renderUnified` at -U0/1/3), a constructive merge generator whose truth is known by construction,
    git as a black box, and the goldens measured on git (histogram worked examples and boundaries, 12 dialect
    canaries, 39 fixture pairs with git 2.43's -U0/-U3 bodies). Not an oracle: the 17 merge goldens (chunks and
    markers) were written by frost::diff itself and reviewed by hand against diff3's rules; they are regression
    goldens that pin behaviour (review 2). Consistency checks are labelled as such (H4, `checkMerge` C1-C4). The
    git harness runs one absolute git path (`FROST_GIT`, else
    configure-time `FROST_GIT_EXECUTABLE`) through `io::IProcessLauncher` with a pinned environment
    (`GIT_CONFIG_NOSYSTEM=1`, an empty `GIT_CONFIG_GLOBAL`, HOME and XDG_CONFIG_HOME in the scratch directory,
    `GIT_CEILING_DIRECTORIES`, `GIT_ATTR_NOSYSTEM=1` because Git for Windows ships a system gitattributes,
    `GIT_TERMINAL_PROMPT=0`, `LC_ALL=C`, and GIT_DIR, GIT_WORK_TREE, GIT_EXTERNAL_DIFF, GIT_DIFF_OPTS and the
    GIT_CONFIG_* overrides removed) and pinned `-c` keys (core.autocrlf, core.safecrlf, core.quotepath,
    core.fsmonitor, core.hooksPath to an empty directory, diff.suppressBlankEmpty, diff.noprefix,
    diff.mnemonicPrefix, diff.relative). One `git diff --no-index --histogram --no-indent-heuristic
    --inter-hunk-context=0 -U<k> -a a b` covers a batch of up to 5000 cases written as `a/cNNNNNN` and
    `b/cNNNNNN`. Success means exit 0 or 1, no signal, empty stderr (git exits 1 with "error: Could not access" for
    a missing path) and a parse in which every differing case has exactly one section with hunks and no
    byte-identical case has one; anything else is an error (`DiffGit.RunnerFaults` injects 12 faults through
    `io::ScriptedProcessLauncher`). A launch failure says "git not found" and how to configure it. The fuzz target
    `diff_merge` checks diffs, rendering, refinement (and that the windowed scan equals the full scan), merges,
    identities, OursPatch application and the marker round trip, before and after custom resolutions with
    marker-like lines, with the support library's Result-returning checks (`tests/diff/support/checks.hpp`).
    Stress reproduction: `FROST_DIFF_SEED` replaces a stress test's master seed; `FROST_DIFF_CASE_SEED` runs only
    the one case whose seed a failure printed. Both are parsed with `std::from_chars`, and a malformed value fails
    the test. The configured git path is dropped and searched for again when it is not an existing absolute path,
    and goes into the test's C string in CMake's forward-slash form.

## Measurements
Machine: REF-W1 (AMD Ryzen 9 5900X, 12 cores / 24 threads, 32 GB, Windows 11 10.0.26300; WSL Ubuntu 24.04).
LLVM 23.1.4; MSVC STL 14.44.35207 on Windows, libstdc++ 14 on Linux. Other slices were building in parallel
(ninja -j 6 each), so absolute times carry up to about 2x noise; every Windows run went through
`run_hidden_nobox.py`. git: 2.56.0.windows.1 (`C:/Program Files/Git/ucrt64/bin/git.exe`, New dialect) and 2.43.0
in WSL (`/usr/bin/git`, Classic dialect), both classified by the canary probe.

### Black-box evidence before this slice (the prototype, section 2 of the slice spec)
A throwaway Python prototype of JGit's rules plus our slider formulation, compared with `git diff --no-index
--histogram --no-indent-heuristic -U0`:

| Set | git 2.43.0 (WSL) | git 2.56.0.windows.1 |
| --- | --- | --- |
| Adversarial (alphabets 2-12, <= 30 lines, <= 6 edits) | 1000/1000 | 988/1000 (12 dialect cases, git fewer in all) |
| Mid-frequency (90 % one line) | 1000/1000 | not measured |
| Sparse fallback (97 % one line; 20 reach the fallback) | 300/300 | not measured |
| -U0/-U1/-U3 bodies | 300/300 each | - |
| Real pairs from FrostIDE's history | - | 300/300 |
| Code-like (prototype generator) | - | 400/400 |

Further findings: the dense-fallback set (two symbols, 50/50, 140-200 lines) differed from git in 2/150 cases with
the DP stand-in, all equal-cost minimal scripts; trimming like JGit mismatches 30-57 in 1000; re-running both
compaction passes changed 34 of 3300 results, re-running pass B alone changed none (H4); the 64/65 boundary (A = x^n
y^(n+1), B = y^(n+1) x^n) is histogram at n = 64 and fallback at n = 65 on both gits; one batched `git diff
--no-index` over 1000 pairs took 0.38 s with hunks identical to per-file runs; git's default indent heuristic changed
0/200 real pairs.

### This slice: the git differential (`diff_git_tests`, Debug, quick counts)
| Class (cases) | git 2.43.0, Classic | git 2.56.0.windows.1, New | cases reaching the Myers fallback |
| --- | --- | --- | --- |
| Adversarial (2000) | 2000 exact | 1979 exact, 21 git-fewer | 0 |
| MidFrequency (300) | 300 exact | 291 exact, 9 git-fewer | 2 |
| SparseFallback (300) | 300 exact | 286 exact, 14 git-fewer | 12 (the prototype's DP: about 20) |
| LoneCr (300) | 300 exact | 300 exact | 0 |
| CodeLike (300, C++ generator) | 300 exact | 300 exact | 0 |
| DenseFallbackMinimal (150) | 133 exact, 17 equal-cost, totals equal | 133 exact, 17 equal-cost | 146 |
| Fixtures (39) | 39 exact, -U0 and -U3 bodies equal | 39 exact | 1 |

No case anywhere had git's total above ours, and every git-fewer case on 2.56 is exact on 2.43 (a dialect difference,
not a bug). `DiffGit.UnifiedBodies` compared the -U0, -U1 and -U3 bodies of every exact case (3333 per context on
2.43, 3289 on 2.56): all equal. `DiffGit.GitApplyAcceptsOurPatches`: `git apply` reproduced B for every case at -U3
and, with `--unidiff-zero`, for every case at -U0 except 33 of 1650 that delete an incomplete last line (Deviations
14). Stress (`DiffStress.Git`, Release, 20000 adversarial and 5000 code-like cases, the same on both OSes): on 2.56
19787 + 213 git-fewer adversarial and 4999 + 1 git-fewer code-like, 0 git-more; on 2.43 20000/20000 and 5000/5000
exact.

Time: `diff_git_tests` takes 17.0 s in Windows Debug on a quiet machine (26.7 s while the WSL ASan leg ran beside
it) and 6.9 s in WSL Debug; the longest case is `DiffGit.UnifiedBodies` (7.8 s on Windows). The file-writing share
(writing batch files over writing plus waiting for git) is 25-61 % per case on Windows (53 % for the 25000 cases of
`DiffStress.Git`, 14.7 s of 27.9 s) and 23-48 % in WSL.

### This slice: perf (REF-W1, Release, default cap 2^22)
The first table is the slice's first version's (Windows ranges span four runs, two of them beside other slices' builds; WSL
is one run). The second is after the review fixes (one run each, quiet machine, best and worst of the runs).

| Case | Windows Release | WSL Release | Sub-budget |
| --- | --- | --- | --- |
| DiffPerf.Typical (10k lines) | p50 1.5-2.1 ms, p99 2.1-3.3 ms | p50 1.4 ms, p99 2.3 ms | p99 5 ms |
| MergePerf.Typical | p50 2.0-3.2 ms, p99 2.4-4.4 ms | p50 1.8 ms, p99 3.0 ms | p99 10 ms |
| DiffPerf.LargeNotDegraded (100k lines, 1000 edits) | 11.8-19.1 ms, undegraded | 15.9 ms, undegraded | 50 ms |
| P1 all-unique vs reversed | 6.2-12.5 ms, exact | 6.0 ms, exact | 50 ms |
| P2 two-symbol random vs mutated | 24.0-34.4 ms, degraded | 22.9-24.7 ms, degraded | 50 ms |
| P3 no common lines | 5.4-13.1 ms, exact | 4.0 ms, exact | 50 ms |
| P4 identical lines plus 10 insertions | 3.6-4.1 ms, exact | 2.9 ms, exact | 50 ms |
| P5 shuffled repeated block | 15.1-29.1 ms, degraded | 14.0 ms, degraded | 50 ms |
| P6 8 MiB line, one word changed (diff + refine) | 0.5-0.6 ms, refinement precise | 0.3 ms, precise | 50 ms |
| P7 8 MiB line, two changes 1 MiB apart | 3.0-3.5 ms, refinement coarse | 3.1 ms, coarse | 50 ms |
| MergePerf.Pathological P2 / P5 | 24.7-34.8 / 17.6-29.6 ms, degraded | 22.4 / 14.1 ms, degraded | 50 ms |

After the review fixes (interning at 8 units per line, the rewritten histogram, the refinement edge path; one
run per OS on a quiet machine; best of the runs, worst in parentheses):

| Case | Windows Release | WSL Release | Sub-budget |
| --- | --- | --- | --- |
| DiffPerf.Typical (10k lines) | p50 1.5 ms, p99 1.6 ms | p50 1.3 ms, p99 1.9 ms | p99 5 ms |
| MergePerf.Typical | p50 2.0 ms, p99 2.4 ms | p50 1.9 ms, p99 3.1 ms | p99 10 ms |
| DiffPerf.LargeNotDegraded | 11.5 (11.9) ms, undegraded, 3616300 units | 10.1 (12.0) ms, undegraded | 50 ms |
| P1 / P3 / P4, exact | 5.7 (6.4) / 5.4 (6.9) / 3.5 (3.6) ms | 3.3 (4.0) / 2.7 (2.9) / 2.7 (2.8) ms | 50 ms |
| P2 / P5, degraded | 16.1 (16.2) / 11.2 (11.4) ms | 16.2 (16.3) / 9.4 (9.5) ms | 50 ms |
| P6 precise / P7 coarse (diff + refine) | 0.16 (0.23) / 0.14 (0.21) ms | 0.33 (0.60) / 0.34 (0.45) ms | 50 ms |
| DiffPerf.MaxLinesBounded (2^21 lines per side) | 13.9 (15.8) ms, degraded | 10.1 (21.0) ms, degraded | 50 ms |
| EvenlySpaced 10k lines, every 10th (needs 2.54x the cap) | 6.5 (7.1) ms, 216 of 1000 hunks | 6.2 (6.6) ms | 50 ms |
| EvenlySpaced 100k, every 1000th (2.77x) | 9.1 (9.7) ms, 14 of 100 hunks | 5.9 (6.1) ms | 50 ms |
| EvenlySpaced 100k, every 2000th (1.58x) | 8.6 (9.1) ms, 16 of 50 hunks | 6.0 (6.5) ms | 50 ms |
| EvenlySpaced 50k, every 500th (1.39x) | 7.2 (7.2) ms, 43 of 100 hunks | 5.7 (6.3) ms | 50 ms |
| MergePerf.Pathological P2 / P5 | 12.6 (12.9) / 11.0 (11.1) ms | 11.9 (13.2) / 8.7 (10.1) ms | 50 ms |
| MergePerf.EvenlySpaced (10k, 250 disjoint edits per side, 1.27x) | 6.4 (6.6) ms, 1 conflict | 6.1 (6.6) ms, 1 conflict | 50 ms |
| RefinePerf.EdgesOnly, 15 MiB, Word / Char | below 0.01 ms, coarse | below 0.02 ms, coarse | 50 ms |
| RefinePerf.ContinuationRuns, 14 MiB, precise / coarse (Char) | 0.87-0.94 / 0.83-0.97 ms (177 / 139 ms before) | 1.05 / 0.82 ms | 50 ms |

P7 dropped from about 3 ms to 0.14 ms because its middle (1 MiB) exceeds maxBytes, so it now takes the refinement
edge path (D8). The reviewer's probes, rebuilt against the fix: 2^21 lines per side 12.8-13.1 ms (191-201 ms
before), a 16 MiB line differing at both ends below 0.1 ms (109-154 ms), 100 conflicts beside a 1 MiB line of `=`
render 1.06 MB (the first version wrote four 1 MiB marker lines per conflict), and the custom text
"kept\n=======\nalso kept\n" next to an unresolved conflict gets size-8 markers and parses.

LargeNotDegraded needed 2216482 units before interning was charged 8 units per line (2354369 before the generators
were made platform-independent, Deviations 17); it needs 3616300 now. Before the refinement window (D8), P6 took
34-57 ms on Windows (tokenising both 8 MiB sides) and P7 34-40 ms.

`DiffPerf.WorkCapSweep` (the slowest P case at each cap, best of three, in ms; P2 from 2^20 up):

| Cap | 2^18 | 2^19 | 2^20 | 2^21 | 2^22 | 2^23 | 2^24 | 2^25 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Windows, quiet | 5.4 | 5.8 | 6.7 | 12.5 | 24.0 | 47.9 | 94.5 | 131.0 |
| Windows, beside parallel builds | 7.4 | 8.0 | 11.6 | 13.1 | 32.3 | 61.6 | 96.5 | 157.3 |
| WSL | 2.2 | 3.1 | 5.6 | 11.3 | 22.9 | 47.5 | 93.5 | 127.7 |
| LargeNotDegraded | degraded | degraded | degraded | degraded | undegraded | undegraded | undegraded | undegraded |

The rule's answer is 2^23 on a quiet machine (P2 at 47.5-47.9 ms, 4 % below the limit) and 2^22 beside parallel
builds (P2 at 53.6-66.7 ms at 2^23 in three runs). kDefaultMaxWork is **2^22**: the assertion of 50 ms has to hold
in every verification run, and 2^22 keeps P2 at 23-34 ms (about 1.5-2x headroom) while LargeNotDegraded keeps 1.9x
headroom in work. This degrades more, never relaxes a number; the notes ask the user whether 2^23 is preferred. After
the review fixes (Windows, quiet machine) the sweep reads 0.4, 1.2, 2.9, 5.7, 16.1, 39.4, 85.6 and 128.0 ms for
2^18 to 2^25 (P2 the slowest from 2^22), LargeNotDegraded is undegraded from 2^22, and the rule's answer is again
2^23 on a quiet machine; the 1.9x work headroom above is now 1.16x (D7).
The slice spec's starting value 2^24 took P2 to 94-118 ms. The unit costs (`DiffPerf.WorkUnitCost`, ns per unit in
the P cases): interning 12-25 per line at one unit per line (up to 84 under load), histogram 1.6-5.6, Myers
1.2-5.6, compaction 2.6; histogram and Myers on P2 are the costliest capped phases. After the review fixes
(interning at 8 units per line, quiet machine): interning 1.5-3.4 per unit (12-27 ns per line), histogram
1.2-5.4, Myers 1.1-5.4, compaction 2.6.

Memory (`DiffPerf.PeakMemory`, Windows Release, two code-like sides of 2097152 and 2097112 lines, 32.7 MB each,
20004 distinct lines): the two sequences hold 115.7 MB (the bytes plus 12.0 bytes per line: the line table and the
hashes); one `diffLines` call peaks at 184.5 MB above that, 44.0 bytes per line of both sides, reached while
interning: the id arrays (4 B per line, kept), the open-addressing table (two 8-byte slots per line, 16 B) and the
key array, which is reserved for one 24-byte key per line although only the distinct lines need one (24 B; a
smaller reservation could save most of it). Later phases stay below that peak: the change flags add 1 B per line,
the histogram with its Myers fallbacks 2 B per line (per-id state and the per-position chain), compaction nothing.
The result was degraded at the default cap. After the review fixes the interner is sized for the lines the
budget can pay for (2^19 at the default cap), and the same call peaks at 23.1 MB (5.5 bytes per line), still
degraded; an uncapped call of that size still peaks at about 44 bytes per line.

### This slice: tests, scales and sanitizers
`scaled(optimised, debug, sanitised)` picks counts per build type: the apply property 20000/6000/3000 pairs,
Myers vs DP 20000/20000/5000, the merge invariants 10000/3000/1500 triples, the refinement window 40000/8000/3000,
the marker round trip 3000/800/400 (each also after custom resolutions), Char refinement against the DP
5000/2000/800, the concurrency check 2000/300/200 iterations on 8 threads.

| Configuration | `diff_tests` (50 cases; 56 after review, 57 in the final run) | `diff_git_tests` (12) | stress: Property / Git / MergeStress |
| --- | --- | --- | --- |
| Windows Debug | 23-27 s | 17.0 s (26.7 s beside the WSL ASan leg) | not run (`-LE stress` in Debug) |
| Windows Release | - | - | 10.8-12.5 s (100k) / 33.7-34.3 s (25k) / 6.8-8.7 s (200k) |
| Windows ASan (RelWithDebInfo), ctest -j 4 | 30.6 s summed | 37.4 s summed | 43.4 s (20k) / 25.9 s (12k) / 26.9 s (40k) |
| WSL Debug | 22.8 s | 6.9 s | not run |
| WSL Release | 4.0 s | - | 7.3 s (100k) / 3.9 s (25k) / 6.6 s (200k) |
| WSL ASan + UBSan (Debug) | 62.5 s | 15.4 s | 86.2 s (20k) / 12.1 s (12k) / 84.4 s (40k) |
| WSL TSan (RelWithDebInfo) | 13.0 s | 3.4 s | 25.5 s (20k) / 2.9 s (12k) / 12.3 s (40k) |
| After review: Windows Debug | 21.5 s | 17.3 s | not run |
| After review: Windows Release | - | - | 8.7 s / 39.3 s / 5.9 s |
| After review: Windows ASan, one process each | 29 s | 41 s | 90 s for the three |
| After review: WSL Release | 3.4 s | 1.9 s | 17.1 s for the three |
| After review: WSL ASan + UBSan | 25.2 s | 10.1 s | 116.4 s for the three |
| After review: WSL TSan | 8.0 s | 3.7 s | 39.7 s for the three |
| Final run (D8's continuation rule): Windows Release | 4.0 s | 21.7 s | 56.0 s for the three |
| Final run: Windows ASan (RelWithDebInfo) | 24.2 s | 23.3 s | 88.4 s for the three |
| Final run: WSL Release | 3.8 s | 2.2 s | 20.2 s for the three |
| Final run: WSL ASan + UBSan (RelWithDebInfo) | 4.7 s | 3.9 s | 20.4 s for the three |
| Final run: WSL TSan (RelWithDebInfo) | 7.7 s | 4.1 s | 33.1 s for the three |

Every case passed in every configuration, with no sanitizer report; `Concurrency.SharedSequences` is clean under
TSan. `DiffStress.Git` on git 2.43 (WSL Release): 20000/20000 adversarial and 5000/5000 code-like exact. Each stress
test prints its master seed and each failing case its own (`FROST_DIFF_SEED`, `FROST_DIFF_CASE_SEED`, D16), and
`Generators.SameOnEveryPlatform` pins the generators' bytes (digest 0xCD2C092506455821 on both OSes), so a seed
reproduces on either.

### Fuzzing
Target `diff_merge` (D16). One 60 s run per OS from the 12 hand seeds: Linux (`linux-clang-fuzz`, ASan + UBSan)
71874 runs, 1178 exec/s, peak RSS 382 MB, 2517 edges in a 1212-file work corpus; Windows (`win-clangcl-fuzz`,
/MT, ASan) 57318 runs, 939 exec/s, peak RSS 411 MB, 2511 edges. No crash, leak or timeout. Both exceed the 200
exec/s target several times over, so the harness keeps the spec's limits (inputs of at most 16384 bytes, at most
32 hunks refined per input). Corpus: the 12 hand seeds plus 18 inputs picked greedily by new edges from a
`-set_cover_merge=1` control file over the Linux run (30 files, 927 bytes, covering 2278 of the 2517 edges). The
replay `fuzz_replay.diff_merge` runs in every -Quick; the 60 s `fuzz.diff_merge` smoke runs in -Full on both OSes
through the existing `ctest -L fuzz`.

After review the harness also compares every refinement with the full scan and round-trips the markers again
after custom resolutions with marker-like lines, which costs about two thirds of the rate: 60 s runs from the
30-file corpus gave 24533 runs at 402 exec/s, peak RSS 377 MB, 2566 edges (Linux) and 23935 runs at 392 exec/s,
435 MB, 2533 edges (Windows). No crash, leak or timeout; still about twice the 200 exec/s target. The replay of
the 30 corpus files passes under Windows ASan, WSL ASan + UBSan and WSL TSan. The corpus is unchanged. After
D8's continuation rule, 120 s runs from the corpus gave 54581 runs at 451 exec/s, peak RSS 398 MB, 2598 edges
(Linux) and 52703 runs at 435 exec/s, 448 MB, 2580 edges (Windows), again with no crash, leak or timeout.

## Consequences
- Every layer from rank 4 up can diff and merge without Qt, git or a new dependency; save-manager gets
  `toEdits`/`editsForOurs` patches that apply as one undoable edit, and the merge view gets chunk byte ranges.
- On classic git the hunks are git's; on git 2.54 and later they may differ (git changes fewer lines in about 1 %
  of adversarial cases). M5 decides which git defines parity (Q3).
- Pathological input degrades at 2^22 units (P2 and P5 in 11-34 ms) instead of stalling; so do inputs at the
  line limit (13-16 ms). Ordinary files (10k lines with scattered or evenly spaced edits, every 10th line
  included) diff and merge exactly, and so do 200k-line diffs and 150k-line merges with few edits (D15); very
  dense evenly spaced edits and larger files still degrade, merges somewhat sooner than diffs (Q4).
- The git differential depends on an installed git: WSL's 2.43 is the exact oracle; Windows runs the new-dialect
  rule unless a classic git is pinned (Q1).

## Alternatives considered
- **Trimming the common prefix and suffix like JGit's `DiffAlgorithm`:** cheaper on mostly-equal files, but git
  does not trim and trimming mismatched git in 3-6 % of cases. Rejected.
- **Myers only:** minimal, but not git's output (histogram anchors on rare lines) and quadratic-ish on large D.
  Kept as the fallback and as `Algorithm::Myers`.
- **Patience diff:** cited by JGit as the origin of rare-line anchoring; histogram generalises it and is what
  `--histogram` users expect.
- **libgit2 / git's xdiff / libxdiff:** GPL-2.0 (with libgit2's linking exception) or LGPL-2.1, and the clean-room
  rule forbids reading xdiff. Rejected.
- **Inside `frost_text`:** grows a CMake file three parallel slices edit and makes every text user link diff code.
- **In `src/vcs` (PLAN 3.2):** rank 7 is out of reach of the M1 consumers at ranks 4-6.
- **A version-number dialect rule:** "later to maint" means pre-2.54 maint releases can carry the change; the probe
  classifies the behaviour itself.
- **A code-level port of JGit's `HistogramDiffIndex`:** would carry the full BSD-3 notice and an "Apache-2.0 AND
  BSD-3-Clause" SPDX line, which CLAUDE.md's header rule does not allow for (Q2). An early draft came close to
  one by accident (D3); it was replaced by our own structure rather than relabelled, and squashed out of history.
- **A different tie-break or a degraded "finisher" for evenly spaced edits:** a tie-break other than JGit's breaks
  git parity; finishing exhausted regions with a cheap linear matcher would make degraded diffs claim matches the
  full algorithm might not make, which breaks D7's argument that degraded merges are sound. Not done: D3's reuse
  computes the same result with less work instead, which keeps both.
- **A larger cap, or interning outside the cap, for the mid-size regression:** either lets a call run past B21's
  50 ms on inputs the cap was tuned to stop (P2; a pair at the line limit, 191-201 ms when interning was nearly
  free). The partner path (D7) charges what a line costs instead.

## Sources
- JGit, https://github.com/eclipse-jgit/jgit at master c43e21c9e8c1110af253a8c1aa262f156591f87b (2026-11-01):
  `org.eclipse.jgit/src/org/eclipse/jgit/diff/HistogramDiff.java` and `HistogramDiffIndex.java` (last changed in
  5c5f7c6b, 2020-01-04), "Copyright (C) 2010, Google Inc. and others", EDL-1.0 (SPDX BSD-3-Clause). The algorithm
  only; no JGit code is included after the rewrite described in D3 (NOTICE has the acknowledgement).
  `DiffAlgorithm.java` was not used.
- E. W. Myers, "An O(ND) Difference Algorithm and Its Variations", Algorithmica 1(2), 1986, section 4b.
- S. Khanna, K. Kunal, B. C. Pierce, "A Formal Investigation of Diff3", FSTTCS 2007.
- Bram Cohen's patience-diff description (as cited by JGit).
- POSIX.1-2024 `diff -u` (the unified format).
- FNV-1a: Fowler, Noll, Vo, draft-eastlake-fnv (public domain).
- git's documentation only: git-diff(1) (`--no-index`, `--histogram`, `--no-indent-heuristic`, `-U`,
  `--inter-hunk-context`), git-config(1) for the pinned keys, git(1) for `GIT_CONFIG_*` and `GIT_ATTR_NOSYSTEM`,
  git-apply(1), and Documentation/RelNotes/2.54.0.adoc.
- Never read: git's xdiff, libxdiff, GNU diffutils/diff3, Qt Creator's, JetBrains' or Zed's GPL diff code, or any
  library deriving from xdiff. The slice's prototype Python files (hist.py, search4.py) were run as behavioural
  oracles, not ported.

## Deviations from the slice spec
1. `LineRange` is declared in `line_sequence.hpp` (the spec put it in `line_diff.hpp`) because
   `LineSequence::byteRange(LineRange)` needs it; `line_diff.hpp` includes it.
2. `LineSequence::hasBreak(LineIndex)` was added (the unified renderer, joining and tests need it).
3. A private header `src/diff/src/engine.hpp` shares the id-level engine (`diffIds`, `hunksFromFlags`,
   `diffLinesWith` with a hash override for `Limits.InternerFlooding`, `refineHunkWith`) between diffLines, merge3,
   checkDiff and the white-box tests.
4. `diffLines` returns no hunks without interning when the two sides are byte-equal (zero work).
5. The interner also charges one unit per 64 bytes compared, so long colliding lines cost work too.
6. A custom text without a final break is joined to whatever follows it (the spec only discusses line pieces).
7. `take()` of a Stable chunk reports `Take::Ours`.
8. merge3's diffs equal `diffLines(base, ours)` and `diffLines(base, theirs)` when neither is degraded; under a cap
   the budgets differ (merge3 charges three sequences' interning and halves the rest).
9. The test support library `frost_diff_test_support` also holds the file helpers, golden readers and the
   Result-returning property checks (`checks.hpp`) shared by the unit tests and the fuzz target; the git runner is
   a second library (`frost_diff_git_support`) that the fuzz target never links.
10. The constructive merge oracle lives in the generators and reports identical deletions on both sides as Both.
11. P6 and P7 are sized in KiB (`pathologicalPair(kind, rng, kib)`), the other P cases in lines.
12. `parseMarkers` takes the split, so the marker round trip parses Lf models by LF only.
13. The absorption-order golden (A=110200 B=100 and A=10100 B=020) was found with a prototype variant that absorbs
    after the loop in a single round, and verified on both gits.
14. `DiffGit.GitApplyAcceptsOurPatches` does not compare the -U0 result of cases whose last hunk is a pure
    deletion of an incomplete last line (33 of 1650 per OS): `git apply --unidiff-zero` (2.43 and 2.56) also drops
    the break of the line before, with git's own byte-identical -U0 patch (verified with a two-line probe). The
    POSIX applier checks these cases at -U0 in every apply property test.
15. The refinement window (D8) and its oracle switch `detail::RefineScan`; the spec tokenises whole hunk sides.
    After review the size guard also runs before tokenising, on the byte middle, and runs of continuation bytes
    have certain token starts (D8).
16. The cap is chosen by `DiffPerf.WorkCapSweep`, which times the P cases at each cap, instead of multiplying the
    most expensive ns per unit by the cap: unit costs vary several-fold between phases and inputs, and the sweep
    measures their product with the cap directly. (The first version's reason, that interning's work does not grow
    with the cap, was wrong: at the line limit it filled the whole cap, D7.) `DiffPerf.WorkUnitCost` still prints
    the per-phase costs, and `kInternUnitsPerLine` is set from them. kDefaultMaxWork is 2^22, not the spec's
    starting 2^24.
17. The generators sequence their random draws explicitly (function arguments have no evaluation order, and
    clang-cl and clang drew differently, so Windows and WSL generated different cases from one seed);
    `Generators.SameOnEveryPlatform` pins a digest of their output.
18. `DiffGit.UnifiedBodies` takes its exact cases from its own -U0 run, and `GitRunner::diffContexts` writes a
    batch once for several contexts, which brought `diff_git_tests` from 29 s to 17 s in Windows Debug.
19. 15 history fixtures instead of about 16 (39 fixtures in all); each gives identical -U0 and -U3 output on git
    2.43 and 2.56.
20. Stress seeds can be overridden with `FROST_DIFF_SEED` (the master seed) and `FROST_DIFF_CASE_SEED` (one
    case), parsed strictly (D16).
21. The marker size is the smallest free size from minSize up to 64 (D11), not the spec's uncapped
    `max(minSize, L + 1)`, and Custom texts count; `renderWithMarkers` returns `Result<MergeOutput>` (OutOfRange
    when no size is free), and `MergeModel` gains `markerSize()` and `customText()`.
22. Interning costs 8 units per line (D7); the spec counted only probe steps as interning work.
23. `Limits.CancelledMidway` diffs two independent random two-symbol texts of 100k lines instead of P2 (the same
    shape: every line repeats far more than 64 times, so the uncapped diff is one long Myers run); the P2 generator
    took 8.3 s to build its input in Debug.
24. The histogram reuses work across regions (D3: index narrowing, the small-region check, the run cache), with a
    test-only switch `detail::HistogramReuse` whose Off setting is the plain rules (the oracle of
    `Histogram.ReuseIsExact`). Counts are kept exact (4 bytes per id instead of 1) and the run cache adds 12 bytes
    per line of B.
25. The interner's partner path (D7), with the test-only switch `detail::PartnerPath` (Off interns every line
    through the table, the oracle of `Interner.PartnerPathIsExact`).
26. Feature-first phase (ADR 0031): the revision above was verified with functional tests and `verify -Quick`
    only. The sanitizer, fuzz, stress and perf runs and the git pins of the second review's finding (D6's
    New-dialect rule bounds the git-fewer count at twice the measured rate instead of pinning the measured cases)
    wait for the hardening phase.

## Questions for the user
- **Q1 (licence, SDK):** may a classic-dialect git (for example portable Git for Windows 2.43.x) be pinned in
  E:\sdk with its SHA-256 in sdk.lock.json as the Windows oracle? git is GPL-2.0, used only as an external test tool
  through IProcessLauncher, never linked or shipped. Until then Windows runs the new-dialect rule.
- **Q2 (licence):** an early draft of the histogram followed JGit's `HistogramDiffIndex.tryLongestCommonSequence`
  in structure, names and statement order; it was replaced by our own structure, which keeps the citation
  (algorithm only) and makes NOTICE's "no JGit code is included" true, and the slice was squashed into one commit
  so that no commit carries the draft (the conservative answer to the old part (b)). Remaining: is the rewrite
  acceptable as is, or would you rather allow a declared code-level port of `HistogramDiffIndex` with the full
  BSD-3 notice and an "Apache-2.0 AND BSD-3-Clause" SPDX line?
- **Q3 (scope, M5):** which git defines M5's "Diff output matches git diff --histogram": the installed git (2.54 or
  later on this machine, so M5 must implement the post-2.54 clean-up) or a pinned version?
- **Q4 (budget, re-posed after the feature-first revision):** 2^22 meets the tuning rule in every run. With D3's
  reuse and D7's partner path, ordinary files (10k lines, scattered or evenly spaced edits down to every 10th line)
  and the mid-size cases of review 2 (200k-line diffs, 150k-line merges with few edits) are exact under it (D15).
  What still degrades: very dense evenly spaced edits (every other line of a 10k-line file needs far more than the
  cap; the reuse leaves an O(D^2) walk) and large files (interning the first side costs 8 units per line, so a
  lightly edited diff exceeds the cap from roughly 300k lines per side and a merge from roughly 250k, extrapolated
  from the 200k and 150k cases at about 12.7 and 16.7 units per line); when the cap strikes
  while interning, the result is one whole-file hunk or conflict. Degraded results stay valid and merges sound.
  Options: (a) keep 2^22 everywhere; (b) a second, larger cap for callers off the UI thread (the merge on save,
  M5's openDiff after the first paint), for example 2^25 (about 130 ms for P2), since `DiffLimits::maxWork`
  already allows it; (c) 2^23 as the default (P2 at 39-48 ms on a quiet machine, over 50 ms beside parallel
  builds). The conservative (a) stays until you choose.
- **Q5 (product, low):** conflict markers default to diff3 style (with the base section) rather than git's "merge"
  style; confirm, and choose the save-manager prompt's labels (for example "editor" / "last saved" / "disk").
- **Q6 (process):** PLAN.md 3.2's layout line ("git, diff, merge" in `src/vcs`) is amended by D1 but not edited
  here (this slice may not edit PLAN.md's status paragraphs); the integrator should update it on merge.
