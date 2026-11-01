# ADR 0012: The text buffer (frost::text piece tree)

- Status: accepted for M1's first slice (`m1/piece-tree`). No budget, test or warning is weakened;
  `tests/perf/budgets.json` is unchanged. A text-side sub-budget of B5 is recorded here (decision 9).
- Date: 2026-11-01
- Plan reference: PLAN.md section 3 ("Text buffer"), the module map and the edit data flow, the testing section
  ("piece tree vs std::string", fuzz targets), sections 6.1 and 6.3 (the fuzz smoke in `verify -Full`), section 7
  M1 (exit: "Piece-tree differential: 10M ops, 0 mismatches; clean 10-minute fuzz"); budgets B5 and B15; feature
  matrix row 35. The number 0011 is taken by the unmerged `m0/decisions`.

## Context
Every M1 feature after this slice (the editor model, undo and anchors, encodings, large files, syntax) reads and
edits text through one buffer. It has to give:
- O(1) immutable snapshots that any thread reads without locks (PLAN.md's edit data flow hands snapshots to the
  syntax host, the indexer and the renderer);
- edits in O(log n) with a reported change that tree-sitter (`TSInputEdit`, byte offsets) and LSP 3.17
  (`didChange`, line/UTF-16 positions) both accept;
- conversions between byte offsets, (line, byte column), (line, UTF-16 column) and UTF-16 offsets in O(log n);
- a path to F1 (a 2 GB file) inside B15 (first screen p50 150 ms, private bytes +64 MB, line index within 2 s),
  which `m1/large-file` gates;
- a keystroke cost far inside B5 (keystroke to paint end p50 3 ms, goal 2 ms; p99 8 ms).

The design follows two published, permissively licensed descriptions, read under the clean-room rule (sources
below): VS Code's piece table (original text plus an append-only add buffer, chunked) and Zed's `sum_tree` (a
B+tree whose nodes cache monoid summaries, searched by a monotone predicate over prefix summaries).

## Decision
1. **D1, content is well-formed UTF-8.** Loads and inserted text are validated with `io::validUtf8Prefix` (Unicode
   table 3-7, the rule frost::io already uses) before anything is appended to storage, and edit offsets must be
   char boundaries. So every piece boundary is a char boundary and every chunk handed out is valid UTF-8 on its
   own, and summaries are exact and split-invariant with per-byte counting: chars are the bytes outside
   0x80..0xBF, UTF-16 units are chars plus the bytes >= 0xF0. `summarize` counts bytes, breaks, line feeds and
   line byte lengths exactly for any input; only chars and UTF-16 assume well-formed input. Queries that land
   inside a char floor to its start before counting chars or units. Files that are not valid UTF-8 are
   `m1/encodings`' job.
2. **D2, the LSP line model.** LF, CR and CRLF each end a line once; columns exclude the break. A CRLF may
   straddle two pieces, and `combine` corrects for that (`a.endsWithCr && b.startsWithLf` merges two breaks into
   one). An offset strictly between the CR and LF of a CRLF reports the CR's point (the end of its line);
   inserting there is allowed and splits the break.
3. **The `lineFeeds` field.** `TextSummary` also counts LF bytes, so `lineBreaks - lineFeeds` is the number of
   lone CRs in O(1). tree-sitter counts rows by LF only, and clangd may as well; the field lets later slices tell
   in O(1) whether a document's LF rows and LSP lines agree (interop notes below).
4. **D3, structure.** `SumTree<Traits, B>` is a persistent, reference-counted B+tree with B..2B entries per node
   (the root 1..2B, an internal root at least 2). Internal nodes keep their children's summaries in an array
   beside the child pointers, so a descent scans contiguous 64-byte summaries. Nodes are immutable once built;
   the reference count is their only mutable field. Every edit is split + split + concat + concat with path
   copying, and split and concat normalise roots (zero items is the empty tree, an internal root with one child
   is replaced by that child). `PieceTree = SumTree<PieceTraits, 8>`. The display map reuses `SumTree`.
5. **Fan-out B = 8** (8..16 entries), against Zed's 6..12 (`TREE_BASE` 6). Wider nodes give a lower tree and
   longer sequential summary scans, and a path copy still costs about 1.2 KiB per level: a leaf is 16 pieces of
   72 B plus a 72 B header, an internal node 16 summaries of 64 B plus 16 references. Measured: a 256 MiB text in
   4 KiB pieces has 4097 leaves and 277 internal nodes, height 3, 5.4 MB of nodes (0.020 bytes per text byte).
   The generic tests instantiate B = 2, 3 and 8.
6. **D4, the piece cap.** Pieces hold 1..`maxPieceBytes` bytes (default 4096, at least 4), which bounds every
   in-piece scan. Loads cut the original at char boundaries into runs of at most P bytes (VS Code's chunked
   buffers). **B15 risk:** at P = 4 KiB, F1 needs about 512k pieces; the measured projection is 39.9 MB of tree
   nodes for 2 GB, more than half of B15's +64 MB private-bytes budget, and the measured Release load rate
   (0.98 GB/s, single-threaded, validation plus summaries) projects 2.0 s for F1 against B15's 2 s line-index
   limit. `m1/large-file` may give mapped originals a larger cap with an in-piece line index, and parallelise
   `summarizePieces` (already a separate, pure step).
7. **D5, storage.** One append-only, reference-counted `Storage` per buffer lineage: the original text (a
   `std::unique_ptr<std::string>`, whose `data()` is taken only once the string sits in its final heap place,
   since moving a short string would move SSO bytes) plus add chunks of 64 KiB. An insert of 16 KiB or more gets
   a dedicated exact-size chunk and leaves the write cursor where it was. Chunks never reallocate, so pieces hold
   raw pointers. The TextBuffer and every Snapshot own the storage; cursors and ranges that hand out
   `string_view`s hold a Snapshot. Deleted text is not reclaimed during a buffer's life (`TODO(M13)`:
   compaction, which would also invalidate storage-position anchors).
8. **D6, size limit.** At most 4 GiB - 2 bytes per buffer in M1 (`maxBufferBytes <= 0xFFFF'FFFE`), so line
   counts, `LineIndex` and `ByteColumn` fit in 32 bits and tree-sitter's 32-bit byte offsets cover every byte and
   the end. Larger buffers are M13's.
9. **B5 sub-budget for the text side: p99 <= 0.25 ms per typing op.** `TextPerf.EditLatency` asserts it in
   optimised, unsanitised builds (`kAssertBudgets`); one op is an insert or backspace at the caret, `snapshot()`,
   `pointAt(caret)` and a `LineCursor` over 60 lines (a screenful), on a 16 MiB code-like document, half CRLF.
   Derivation: 0.25 ms is 1/32 of B5's p99 limit (8 ms) and 1/8 of its p50 goal (2 ms), which leaves the rest of
   the keystroke (input, editor model, syntax, layout, paint) the remaining 31/32 at p99. Measured on REF-W1 in
   Release: p50 9.7 us, p99 22.2 us, max 111 us, so the sub-budget has an order of magnitude of headroom.
   `budgets.json` is unchanged: B5 itself is measured end to end (ETW) once the editor paints (later M1 slices).
10. **D7, concurrency.** One writer (the TextBuffer, owned by one thread at a time) and any number of readers
    (Snapshots). Reference counts are the only shared mutable state, and storage bytes are written only past every
    published byte. Increments are relaxed; decrements are `fetch_sub(acq_rel)` (the Boost intrusive_ptr scheme),
    with no standalone fences because ThreadSanitizer does not model `atomic_thread_fence`. Queries take no
    references: only copying a Snapshot touches counters, so readers that query one snapshot never contend on its
    root's counter. Allocation failure terminates; edits, loads and text members are `noexcept`, so `bad_alloc`
    never leaves frost::text (as in frost::sched).
11. **D8, TextChange normalisation.** Every change an edit step reports is widened so that no bound lies strictly
    inside a CRLF in the old or the new text, and the line structure outside the change is identical in both.
    With O the old text, N the new, s the start and eo/en the old/new ends: if s > 0, O[s-1] is CR and O[s] or
    N[s] is LF, then s -= 1; if O[eo] is LF (equivalently N[en]) and O[eo-1] or N[en-1] is CR (tested with the
    bounds before the start rule ran), then eo and en grow by one. A change is at most one byte wider on each side
    than the edit asked for. **Rationale:** LSP cannot express a position between a CR and its LF, and an edit
    that turns a lone CR into a CRLF (or splits one) changes the line structure one byte outside the edited range;
    widening makes the points LSP-representable and makes "apply the change to the old text as an LSP edit"
    reproduce the new text, which the tests check after every op. `applyEdits` reports one change per step, in
    application order (last edit first), each in its own step's coordinates, ready for sequential `didChange`
    entries.
12. **Edits.** `insert`, `erase`, `replace`, `applyEdits` (atomic batch) and `restore(earlier snapshot)` are
    all-or-nothing: on error the text, version and storage are unchanged. Checks run in a fixed order shared with
    the test oracle: begin > end (InvalidArgument), end > size (OutOfRange), a bound inside a UTF-8 sequence
    (InvalidArgument), ill-formed text (InvalidArgument with the byte offset), result above `maxBufferBytes`
    (OutOfRange). `applyEdits` requires `edits[i-1].range.end <= edits[i].range.begin`, validates everything
    first, then applies last to first. Typing coalescing is stateless: an insert whose left neighbour piece ends
    exactly at the write cursor (compared as chunk index and offset) and still fits the cap and the chunk extends
    that piece into a new leaf; nothing is tracked, so restore and reload need no invalidation.
13. **The first fuzz target and its build.** `text_piece_tree` drives the same `OpDriver` as the differential and
    stress tests, with choices read from the input instead of a seed. Fuzz trees are Qt-free and gtest-free
    (`FROST_FUZZ`, presets `win-clangcl-fuzz` and `linux-clang-fuzz`, RelWithDebInfo); normal trees build a
    replay executable that runs the committed corpus as the unit test `fuzz_replay.text_piece_tree` on both OSes.
    `verify -Full` runs each fuzz target for 60 s (`FROST_FUZZ_SECONDS`) on both OSes. Windows specifics, probed
    on 2026-11-01 with LLVM 23.1.4 and MSVC 14.44.35207:
    - **/MT is required.** The pinned `clang_rt.fuzzer-x86_64.lib` records, in each of its 15 members,
      `/FAILIFMISMATCH` values `RuntimeLibrary=MT_StaticRelease`, `_ITERATOR_DEBUG_LEVEL=0`, `annotate_string=0`,
      `annotate_vector=0`, `_MSC_VER=1900` and `_CRT_STDIO_ISO_WIDE_SPECIFIERS=0`. Configure fails unless
      `CMAKE_MSVC_RUNTIME_LIBRARY` is exactly `MultiThreaded`, ASan is on, and UI and tests are off (gtest from the
      /MD triplet cannot link into /MT objects).
    - **The STL annotations are off in fuzz trees.** ASan TUs record `annotate_string=1` and `annotate_vector=1`,
      which fails lld-link's `/failifmismatch` against the library, so every fuzz TU defines
      `_DISABLE_STRING_ANNOTATION` and `_DISABLE_VECTOR_ANNOTATION`. The fuzz tree therefore loses std::string and
      std::vector container-overflow checks; the Windows ASan test tree keeps them.
    - **libcpmt check.** The library imports five STL vectorised helpers (`__std_find_trivial_1`,
      `__std_find_last_trivial_1`, `__std_find_last_of_trivial_pos_1`, `__std_reverse_trivially_swappable_1`,
      `__std_search_1`); the pinned 14.44.35207 `libcpmt.lib` defines all five (`llvm-nm --defined-only`).
    - **Link inputs** mirror `clang-cl -fsanitize=address,fuzzer -MT -###` (run with `MSYS_NO_PATHCONV=1`):
      `-wholearchive:clang_rt.fuzzer-x86_64.lib`, `clang_rt.asan_dynamic-x86_64.lib` and
      `-wholearchive:clang_rt.asan_static_runtime_thunk-x86_64.lib`, and no `/INCLUDE:__asan_seh_interceptor`
      (the driver passes it only for /MD; the static thunk defines the symbol). CMake links with lld-link directly,
      so the toolchain file spells these out as `FROST_FUZZER_LINK_OPTIONS` and `FROST_ASAN_STATIC_LINK_OPTIONS`.
      Compiles add `-fsanitize=fuzzer-no-link`.
    - **No vcpkg install.** The fuzz presets set `VCPKG_MANIFEST_INSTALL=OFF`: fuzz targets link no ports, and the
      shared installed tree is left alone. `TODO(M2)` in `cmake/FrostFuzz.cmake`: add the
      `x64-windows-clangcl-fuzz` triplet (PLAN.md) when a fuzz target first links a port such as pugixml.

### Interop notes for later slices
- **tree-sitter.** `TextChange`'s byte fields have `TSInputEdit`'s shape. Its points follow LSP lines; tree-sitter
  counts rows by LF only, so the syntax host computes `TSPoint`s itself (by LF rows). They equal the LSP points
  whenever the document has no lone CR (`lineBreaks == lineFeeds`, O(1)). `chunkAt(offset)` has the shape of
  tree-sitter's read callback (the rest of the piece, starting with a whole char at a char boundary).
- **clangd and other servers.** LSP 3.17 defines CR, LF and CRLF as line ends; a server that splits on LF only
  disagrees with us exactly on lone CRs. Plan for M3: the LSP client falls back to full-document sync for a
  document with lone CRs when its server is known to split on LF only.
- **Anchors** (`m1/undo-anchors`): either TextChange-driven offsets, or storage positions, in which case
  `TODO(M13)` compaction must remap them. That slice chooses before `Piece`'s layout is frozen.
- **Undo** needs node identity for a structural diff (precise `restore` changes instead of today's whole-text
  change). `SumTree` keeps detail access to the root pointer and children for that; it is not public API.
- **The display map** reuses `SumTree` and needs reverse cursors and seek-forward (`m1/editor-model`).
- **Releasing big trees:** dropping the last reference to a 2 GB document's tree frees about 40k nodes on the
  releasing thread, in the millisecond range. `TODO(M1)`: `m1/large-file` moves that release to a background lane.

### Deviations from the slice spec
- **CR/LF cuts:** `appendPieces` does not avoid cutting between a CR and an LF. The spec allows this ("where
  cheap"; not needed for correctness), and the tests run caps of 4, 5 and 7 bytes precisely to exercise straddling
  CRLFs.
- **Node layout:** `NodeBase` is templated on `<Traits, B>` instead of `<Summary>` only, which keeps `destroy`
  (dispatch on height to the concrete `Leaf` or `Internal`) free of casts between unrelated instantiations.
- **Worktree path:** `E:\dev\FrostIDE\.claude\worktrees\m1-piece-tree` (the task's command), not
  `...\worktrees\piece-tree` (spec section 0).
- **Differential threads:** `TextDifferential.Quick` runs its 200 seeds on up to 4 threads
  (`clamp(hardware_concurrency / 2, 1, 4)`, `FROST_TEXT_THREADS` overrides; 1 when `FROST_TEXT_SEED` is set). Each
  seed stays deterministic and the report names the lowest failing seed, so results do not depend on the thread
  count. One thread took 21.6 s in Windows Debug against the 15 s ceiling; four take 8.2 s.
- **Length distribution:** inserted text is 1-8 bytes 97.8 % of the time, up to 4 KiB 2 %, up to 64 KiB 0.2 %
  (spec: "mostly 1-8 bytes, sometimes 4 KiB, rarely 64 KiB"). With the first draft (90/9/1) documents sat at the
  64 KiB cap and every full check cost O(64 KiB); now the mean text at a full check is about 4.9 KiB, while
  `TextStress.Differential` still reaches the cap (largest text 65538 bytes).
- **The document cap is soft:** an accepted raw-byte insert (an "invalid" op whose random bytes happen to be valid)
  may exceed `maxDocBytes` by up to 8 bytes.
- **One invariant call per full check:** `verifyFull` calls `TextBuffer::checkInvariants`, which already runs the
  snapshot's checks, instead of calling both.
- **No-op changes are unnormalised:** a no-op edit at a CRLF-interior offset reports the empty change at that
  offset (spec 3.5), whose point is the CR's point; D8's widening applies to changes that change the text.

## Measurements
Machine: REF-W1 (AMD Ryzen 9 5900X, 12 cores / 24 threads, 32 GB, Windows 11 10.0.26300; WSL Ubuntu 24.04 with
5 GB). LLVM 23.1.4; MSVC STL 14.44.35207 on Windows, libstdc++ 14 on Linux. The user was at the PC (light
desktop use) during every run. All Windows runs went through `run_hidden_nobox.py` (a private desktop).

### Test times and scales
| Configuration | `TextDifferential.Quick` | all text unit cases | stress: Differential / F2SingleLine / ConcurrentReaders |
| --- | --- | --- | --- |
| Windows Debug | 8.2 s | 11.7 s | not run (`-LE stress` in Debug) |
| Windows Release | 1.3 s | 2.1 s | 21.6 s (1M ops) / 2.5 s (50 MB line) / 11.8 s (1M edits) |
| Windows ASan (RelWithDebInfo) | 8.9 s | 15.4 s | 55.8 s (500k) / 0.8 s (8 MB) / 45.1 s (200k) |
| WSL Debug | 6.1 s | 8.3 s | not run |
| WSL ASan + UBSan (Debug) | 21.1 s | 29.2 s | 66.7 s (200k) / 1.7 s (8 MB) / 35.9 s (200k) |
| WSL TSan (RelWithDebInfo) | 11.6 s | 16.4 s | 52.6 s (300k) / 2.7 s (8 MB) / 13.4 s (100k) |

"All text unit cases" is the sum over `text_tests` (62 cases), the text perf cases and `fuzz_replay.text_piece_tree`.
Every unit case other than the differential takes at most 0.9 s in Windows Debug (`TextConcurrency` 0.84 s), and
every stress case stays under its 120 s ceiling in each `-Full` configuration. `TextConcurrency.ReadersDuringEdits`
is clean under TSan.

Scales (`tests/text/stress_test.cpp`, chosen to keep each case under 120 s): `TextStress.Differential` runs 1M ops
in optimised unsanitised builds, 500k in optimised ASan, 300k under TSan, 200k in sanitised Debug and in
unsanitised Debug, and 10M with `FROST_STRESS_FULL=1` in optimised unsanitised builds (the M1 exit scale). Seeds of
20k ops, a full check every 16 ops with 64 queries. `TextStress.F2SingleLine` uses a 50 MB line, 8 MB in Debug or
sanitised builds. `TextStress.ConcurrentReaders` runs 1M edits optimised, 100k under TSan, 200k otherwise.

M1 exit scale (`FROST_STRESS_FULL=1`, Release, 500 seeds of 20k ops on 4 threads, a full check every 16 ops): **10M
ops, 0 mismatches on both OSes**. Windows 217.5 s (1,125,973 full checks; ctest through `run_hidden_nobox.py`),
WSL 166.1 s (1,126,685 full checks; the binary run directly in `verify.sh`'s ext4 mirror). The other two stress
cases ran at their default scale beside it: F2SingleLine 2.5 s and 2.2 s, ConcurrentReaders 11.8 s and 11.4 s.

### Perf (REF-W1, Windows Release)
- `TextPerf.EditLatency` (16 MiB, 100k typing ops): p50 9.7 us, p99 22.2 us, max 111 us. Debug, 1 MiB, 2k ops,
  report only: p50 35.9 us, p99 76.9 us.
- `TextPerf.LoadThroughput`: 256 MiB in 0.273 s = 0.98 GB/s; 65537 pieces (4096 bytes each), 4097 leaves, 277
  internal nodes, 5.4 MB of nodes. F1 projection: load 2.03 s, 39.9 MB of tree nodes.
- `TextPerf.SeekLatency` (16 MiB, 419144 lines, height 3): p99 `pointAt` 4.3 us, `offsetAt(Point)` 3.7 us,
  `offsetAt(Utf16Point)` + `utf16PointAt` 15.8 us, `lineInfo` 3.0 us.

### Fuzzing
Target: at least 200 exec/s in the ASan fuzz build on each OS. Both builds exceed it several times over, so the
harness caps stay at the spec's values (512 ops, documents of 64 KiB, a full check every 32 ops with 16 queries).
- Windows (`win-clangcl-fuzz`, /MT, ASan), the 60 s `fuzz.text_piece_tree` smoke: 1484 exec/s on the first run
  (90533 runs from the 6 hand seeds; peak RSS 397 MB), 940 exec/s in `verify -Full` (57371 runs, starting from the
  first run's 1465-file work corpus; peak RSS 413 MB).
- Linux (`linux-clang-fuzz`, ASan + UBSan), the same smoke in `verify -Full`: 1079 exec/s (68038 runs; peak RSS
  365 MB).
- Corpus: 6 hand seeds plus 24 inputs picked from a 60 s run's 1470. `-set_cover_merge=1` with `-use_counters=0
  -use_cmp=0` reduced them to 71 files covering 2208 edges; a greedy cover over that merge's control file picked
  the 24, keeping the corpus inside the spec's 10-30 seeds. The committed 30 files (615 bytes) cover 2044 of the
  2208 edges (`-runs=0`); the fuzz smoke regrows the rest within seconds. Every merge and replay ran through
  `run_hidden_nobox.py`.
- M1 exit, a clean 10-minute fuzz on each OS, in separate trees configured with `-DFROST_FUZZ_SECONDS=600`
  (`build\win-clangcl-fuzz-exit` and the mirror's `build/linux-clang-fuzz-exit`; the `-Full` trees keep 60), each
  starting from the committed corpus: **no crash, leak or timeout on either OS.** Windows 325219 runs in 601 s =
  541 exec/s, 2313 edges, peak RSS 624 MB (ctest through `run_hidden_nobox.py`); Linux 346013 runs in 601 s = 575
  exec/s, peak RSS 414 MB. The rate halves over the long runs as libFuzzer's inputs grow (the length limit reached
  252 bytes on Windows).

## Consequences
- Every later M1 slice builds on immutable snapshots: the syntax host, indexer and renderer read without locks, and
  undo can keep old versions for the cost of their unshared nodes.
- Edits report LSP-ready changes; the LSP client never has to re-derive line structure around a CR.
- Memory is append-only until `TODO(M13)` compaction: a long editing session keeps every inserted byte.
- F1 is tight at P = 4 KiB (decision 6); `m1/large-file` owns the mitigation and B15's measurement.
- The fuzz build has its own CMake path and presets. `verify -Full` grows by about 1.5 minutes per OS: on Windows
  the fuzz leg's configure and rebuild took about 10 s and its test 61 s; in WSL the first configure and build of
  the fuzz tree took about 26 s and its test 59 s (file times in the build trees).
- Text, core and io changes need an independent review before merge (CLAUDE.md); not part of this slice.

## Alternatives considered
- **VS Code's red-black piece tree.** Mutable in place, so a snapshot needs a copy or a lock; rejected for a
  persistent B+tree whose snapshot is one reference-count increment.
- **A rope that copies text into its leaves** (no separate storage). Every edit would copy up to a leaf's text,
  and a 2 GB load would copy the file; the piece table keeps the original in place (and lets `m1/large-file` map
  it).
- **A gap buffer or a plain line array.** No O(1) snapshots and O(n) worst-case edits.
- **Fan-out B = 6 (Zed) or 16.** 6 makes the tree taller with shorter scans; 16 doubles the path-copy cost per
  level (about 2.3 KiB) for little height gain at these sizes.
- **LF-only lines (tree-sitter's rows).** Simpler, but LSP 3.17 positions count CR and CRLF too, so every change
  would need translation for every server; the LSP model plus `lineFeeds` keeps both available.
- **Lossy decoding inside the buffer.** It would make byte offsets disagree with the file on disk; validation stays
  in the buffer and lossy policies go to `m1/encodings`.
- **Release decrement plus an acquire fence** (Boost's exact form). Equivalent on x64, but ThreadSanitizer does not
  model standalone fences, and the TSan leg is part of the gate.
- **A fuzz vcpkg triplet now.** Nothing fuzzed links a port yet; deferred to M2 (`TODO(M2)`).
- **Fuzzing in the /MD ASan tree.** The pinned libFuzzer library is /MT only (`/FAILIFMISMATCH`), so a /MD fuzz
  build would need a rebuilt libFuzzer outside the pinned SDK.

## Sources
- VS Code: "Text Buffer Reimplementation" (Peng Lyu, 2018-03-23,
  https://code.visualstudio.com/blogs/2018/03/23/text-buffer-reimplementation); the source tree
  `src/vs/editor/common/model/pieceTreeTextBuffer/` in microsoft/vscode (MIT, latest commit a4ba39c) was allowed
  but not read: the post sufficed. Borrowed: the original-plus-add-buffer piece table and bounded chunks
  (`src/text/src/storage.hpp`, `piece_tree.hpp`).
- Zed: `crates/sum_tree` at commit d544470 (2026-10-05), whose `Cargo.toml` declares `license = "Apache-2.0"`.
  Borrowed: cached child summaries beside the child pointers, the dimension seek by a monotone predicate, the
  height-aware append (`push_tree`) and small test fan-outs (`src/text/include/frost/text/sum_tree.hpp`). Zed's
  rope, text and editor crates (GPL) were not opened.
- Boost: intrusive_ptr and the Boost.Atomic "Reference counting" usage example (BSL-1.0), for the reference-count
  memory ordering (`src/text/include/frost/text/detail/ref.hpp`).
- LSP 3.17 (line ends and positions) and tree-sitter's public API documentation (`TSInputEdit`, `TSPoint`, the read
  callback).
- No function is a close port of any upstream code, so no NOTICE entry or upstream copyright line is needed.
