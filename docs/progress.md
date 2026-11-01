# FrostIDE progress log

Session hand-off log (CLAUDE.md, PLAN.md section 6.7). Newest entry first. Each entry lists what was done,
what is next, blockers and open questions for the user.

## 2026-11-01: `m1/piece-tree`, acting on two independent reviews

Two independent reviews of c8757f3: one approves, one asks for changes. Neither found a correctness,
memory-safety or concurrency defect in `src/text`. Together they report one medium and seven low findings, all in
test tooling, docs and the clean-room record. Every finding is fixed or recorded; one question for you is below.
Committed on `m1/piece-tree`, not merged or pushed. Every Windows test ran through `run_hidden_nobox.py`; no
window opened on your desktop.

### Done
- **Medium, the stress test's reproduction command.** `TextStress.Differential` runs 64 queries per full check,
  but the command it printed re-ran the seed through `TextDifferential.Quick` with its fixed 16. The verify RNG
  draws differently with 16 queries, so a failure found by the later draws could pass under the printed command.
  `text_ops` now has `quickSettings`, which parses `FROST_TEXT_SEED`, `_OPS`, `_CHECK_EVERY`, the new
  `FROST_TEXT_QUERIES` and `_THREADS` and rejects bad values (InvalidArgument; zero threads too, which used to run
  no seed), and `reproduceCommand(SeedRun)`, which sets every SeedRun field. Both tests print it. New tests:
  `TextDifferential.ReproduceCommandRoundTrips` and `TextDifferential.SettingsDefaultsAndBadValues`.
- **Low, the buffer limit was never fuzzed.** One differential seed in seven (`seedBufferLimit`, `seed % 7 == 3`)
  and 30 of the 256 values of a fuzz input's first byte now run with a small `maxBufferBytes` (`limitBuffer`).
  Edits, batches, loads and the invalid op's near-limit branch then meet check 5. `DriverStats::overLimit` counts
  these rejections, `TextDifferential.Quick` expects some, and the stress test prints the count. No committed
  corpus file changes meaning: none has a first byte in the new range. The first mapping, `(seed / 24) % 8 == 7`,
  chose no seed in the default-scale stress runs (seeds 1,000,000-1,000,049; the first `-Full` printed
  "0 rejections by the buffer limit"). Any 7 consecutive seeds include one of the new mapping's seeds.
- **Low, the fuzz smoke was not hermetic.** The new setup fixture `fuzz.<name>.reset`
  (`cmake/FrostFuzzResetWork.cmake`) empties `fuzz-work/<name>` before each run, so every run starts from the
  committed corpus. Fuzz trees now count two tests (`win fuzz 2/2`, `wsl fuzz 2/2`).
- **Low, docs:** `snapshot.hpp` and ADR 0013 D7 now say that `chunks()` and `linesFrom()` copy the snapshot (four
  counter operations per call). The ADR's tree height for 256 MiB is now 4, not 3: 16^3 = 4096 leaves is the most
  height 3 holds, and `TextPerf.LoadThroughput` now prints the height. D8 now says where the LSP-replay property is
  checked: at each full check, not after every op. Two comment fixes: `mergeSiblings` holds "at least B", and the
  harness takes bytes `[2, 2+L)`.
- **Low, clean-room record:** `text_summary.hpp` and ADR 0013's Sources now record where TextSummary's
  line-metric design came from, and the review's note that the line fields resemble those of Zed's GPL rope
  crate. That crate was not opened. See the question below.
- **Mutation checks** (Debug, each reverted afterwards; `git diff src/text/src` is empty). Each mutant made the new
  tests fail:
  - (A) the command without `FROST_TEXT_QUERIES`, and (B) Quick ignoring that variable: the round-trip test fails.
  - Removing the size check from (C) `applyEdits`, (D) `replace` or (E) `fromUtf8`: Quick fails 3, 29 and 12
    seeds, the first being seed 3, 3 and 10. All three were also killed under the first mapping, where the
    printed command reproduced C's first failing seed (171) on its own.
  - (F) no limited seeds: Quick fails its `overLimit > 0` expectation.
  The reviewer's surviving mutant was C. These runs used the final mapping: Quick had 486 limit rejections, and
  the 1M-op stress run in Release had 2218.
- **Verification** (`run_hidden_nobox.py`, `-Jobs 4`, `night.py build-gate` before every build and long run):
  - `verify.ps1 -Full`, twice: first on the first mapping (21 min 49 s), then on the final code (20 min 26 s).
    Both gave win debug 435/438 (1 skipped), release 441/444 (1 skipped), asan 442/445 (1 skipped), fuzz 2/2; wsl
    debug 404/404, asan 411/411, tsan 410/410, fuzz 2/2. format, frost_lint (348 files), licensecheck and
    featurecheck were ok. The only failures are the known `FrostProcess.RunsFromPathLongerThanMaxPath` and
    `FrostProcess.PlatformPluginPathBoundary`; the skip is `FrostProcess.ReportsLongPathLimitCleanly`.
  - The final `verify.ps1 -Quick`, after the long runs and the docs: win debug 435/438 (1 skipped; the same two
    known failures), wsl debug 404/404, all checks ok, 105 s wall. Only this log entry changed after it.
  - Every text stress case stays under its 120 s ceiling. The longest are WSL ASan Differential at 55.6 s, WSL
    TSan Differential at 48.9 s and Windows ASan Differential at 31.2 s. `TextDifferential.Quick` takes 6.0 s in
    Windows Debug and 5.7 s in WSL Debug.
- **M1 exit runs, repeated on the final code** (run beside each other; ADR 0013 has the details):
  - The 10M-op differential in Windows Release: **0 mismatches**, 1,126,249 full checks, 21,367 rejections by the
    buffer limit, 161.4 s.
  - A **clean 10-minute fuzz on each OS**, each starting from an empty work corpus. Windows: 381289 runs,
    634 exec/s. Linux: 386400 runs, 642 exec/s. No artifacts on either.
- `TextPerf.LoadThroughput` at 256 MiB now prints height 4 (the ADR's corrected value).

### Next
- Merge `m1/piece-tree` once M0 is signed off and you have answered the provenance question (CLAUDE.md: text,
  core and io had their independent review; these fixes touch only comments in `src/text`).
- Then `m1/undo-anchors`, `m1/encodings`, `m1/large-file` and the editor model, as in the entry below.

### Blockers
- None new. The two known FrostProcess failures still wait for `m0/longpath`.

### Questions for the user
- **TextSummary provenance (clean room).** The second review noted that TextSummary's line metrics
  (`firstLineChars`, `lastLineChars`, `lastLineUtf16`, `longestLineChars`, `longestLine`) and the non-CRLF combine
  rule match, field for field, the `TextSummary` of Zed's rope crate. That crate is GPL, and PLAN.md section 3
  lists it as off-limits. The review found no evidence of copying, and found the CR, CRLF and lone-CR rules,
  `lineFeeds`, per-byte counting and tie-breaking original. The slice's source record says the crate was not
  opened. The record now states this in ADR 0013 and in a provenance comment in `text_summary.hpp`. Is that record
  enough, or do you want something else? For example, a redesign of the line fields, or a citation of a public
  description such as Zed's "Rope & SumTree" blog post, which this session has not read.

## 2026-11-01: M1 first slice, the frost::text piece tree (`m1/piece-tree`)

M1 started while M0 waits for sign-off (your instruction on 2026-11-01). Branch `m1/piece-tree` from
`m0/foundation` (593e15f), worktree `.claude\worktrees\m1-piece-tree`; committed, not merged or pushed. Design,
sources and measurements: ADR 0013. Every Windows test ran through `run_hidden_nobox.py` (a private desktop with
error boxes off); no window opened on your desktop.

### Done
- **frost::text** (`src/text`, layer rank 4): the generic persistent `SumTree<Traits, B>` (ref-counted B+tree,
  B = 8), the piece tree over append-only UTF-8 storage, `TextSummary` (64 B), O(1) lock-free `Snapshot`s,
  `TextBuffer` with insert/erase/replace, atomic `applyEdits` and `restore`, each returning `TextChange`s normalised
  to line-break boundaries (D8), conversions between byte offsets, `Point`, `Utf16Point` and UTF-16 offsets with
  documented round-trip rules, chunk and line iteration, `summaryOf`, `checkInvariants`, and stateless typing
  coalescing. Nothing from the spec's cut list was cut.
- **Core and io:** `Utf16Column` in `strong_type.hpp`; `io::validUtf8Prefix`, now behind `isValidUtf8`, tested
  against an independent encoder over every scalar value (`ValidUtf8PrefixMatchesEncoder`).
- **Tests** (`tests/text`): a brute-force oracle with its own bit-pattern UTF-8 decoder and an `OpDriver` shared by
  the differential, stress and fuzz tests; 62 `text_tests` (summary monoid, `SumTree` at B = 2, 3 and 8, 30 edge
  cases, the 200-seed differential, the concurrent-reader test), `text_stress_tests` and `text_perf_tests`. The
  shared test environment (`stressFull`, `kSanitized`, ...) moved to `tests/support/test_env.hpp`.
- **Fuzzing:** `FROST_FUZZ`, `cmake/FrostFuzz.cmake`, presets `win-clangcl-fuzz` (/MT) and `linux-clang-fuzz`, the
  first target `text_piece_tree` with a 30-file corpus, the corpus replay `fuzz_replay.text_piece_tree` (label unit)
  in every normal tree, and the fuzz legs in `verify -Full` on both OSes (60 s per target).
- **Verification** (`run_hidden_nobox.py`, `-Jobs 4`, after `night.py build-gate`):
  - `verify.ps1 -Quick`: win debug 433/436 (1 skipped), wsl debug 402/402, format, frost_lint, licensecheck and
    featurecheck ok. Run twice: 136 s wall (rebuilding the reformatted text sources) and, on the final tree, 132 s
    (rebuilding everything that includes `snapshot.hpp` after a comment change).
  - `verify.ps1 -Full`: win debug 433/436 (1 skipped), release 439/442 (1 skipped), asan 440/443 (1 skipped), fuzz
    1/1; wsl debug 402/402, asan 409/409, tsan 408/408, fuzz 1/1; all checks ok. 27 min 9 s wall, most of it the
    first build of this worktree's ASan trees on both OSes. It ran before the last change, which only adds a comment
    to `snapshot.hpp` (the `TODO(M1)` on releasing big trees) and fills in the docs; the second `-Quick` covers it.
  - The only failures are the known `FrostProcess.RunsFromPathLongerThanMaxPath` and
    `FrostProcess.PlatformPluginPathBoundary` (they fail until `m0/longpath` lands); the skip is
    `FrostProcess.ReportsLongPathLimitCleanly`.
- **Text test times and scales** (ADR 0013 has the table): `TextDifferential.Quick` 8.2 s in Windows Debug (ceiling
  15 s; 4 threads), 6.1 s in WSL Debug; every other unit case at most 0.9 s in Windows Debug. All text unit cases
  together: 11.7 s Windows Debug, 8.3 s WSL Debug. Stress cases in `-Full`, longest first: WSL ASan Differential
  66.7 s (200k ops), Windows ASan Differential 55.8 s (500k), WSL TSan Differential 52.6 s (300k), Windows ASan
  ConcurrentReaders 45.1 s; every case is under its 120 s ceiling.
- **B28 effect:** the text tests add about 12 s (Windows) and 8 s (WSL) to `verify -Quick`'s test steps, inside the
  30 s per OS the slice allows; the test steps now take 58.6 s and 25.2 s. ADR 0010 measured `verify -Quick` at
  p50 82.3 s after a one-.cpp change; this slice's two runs took 136 s and 132 s, each after a change that rebuilt
  the text library and its four test executables on both OSes, so they are not B28 samples. Both stay far inside
  B28's p50 5 min. Re-measuring B28 with ADR 0010's harness is part of its M1 follow-up.
- **Perf** (Windows Release, REF-W1, you at the PC): `TextPerf.EditLatency` p50 9.7 us, p99 22.2 us against the
  0.25 ms text-side sub-budget of B5 (ADR 0013, decision 9; asserted in optimised builds); load 0.98 GB/s, F1
  projection 2.03 s and 39.9 MB of tree nodes (the B15 risk in ADR 0013, decision 6); seek p99 3-16 us.
- **Fuzz throughput** (60 s smoke, ASan): Windows 1484 exec/s on the first run, 940 exec/s in `-Full` (a larger
  work corpus); Linux (ASan + UBSan) 1079 exec/s; target 200. Caps unchanged.
- **M1 exit runs** (after `night.py build-gate`): the 10M-op differential in Release, **0 mismatches on both OSes**
  (Windows 217.5 s through `run_hidden_nobox.py`; WSL 166.1 s in the ext4 mirror), and a **clean 10-minute fuzz on
  each OS** in separate `FROST_FUZZ_SECONDS=600` trees (Windows 541 exec/s, Linux 575 exec/s; no artifacts). Both
  M1 exit criteria for the piece tree are met; the other M1 exit items wait for later slices.
- `tests/features/matrix.json` row 35 (text buffer, large files, undo) is `in-progress`.

### Next
- An independent review of `m1/piece-tree` (it touches core, io and text) before any merge (CLAUDE.md).
- `m1/undo-anchors` (anchors, selections, the undo tree, precise restore changes by node identity), then
  `m1/encodings` and `m1/large-file` (mapped originals, B15 at the buffer level, the background release of big
  trees: `TODO(M1)`), and the editor model (reverse cursors, seek-forward, graphemes).

### Blockers
- None for this slice. M0 sign-off (G1) and the M0 questions below are still open; `m0/longpath` decides the two
  known FrostProcess failures.

### Questions for the user
- None new. ADR 0013 records the slice's decisions; none weakens a test, budget or warning.

## 2026-11-01: M0 overnight run and exit verification (`m0/foundation`)

This entry consolidates the five entries that tonight's branches appended (S1 on 2026-10-31 and on 2026-11-01,
Tracy, S7 and B28) and adds the work that had none. Their facts are kept below. Their "Next" items are done or
carried into "Next": every branch is merged, and the integrated `verify -Quick` runs and tonight's `verify -Full`
cover the verification that the S7 entry still asked for before `m0/s7` merged. Their questions are in the
consolidated list at the end. Sources: `git log --oneline 8027101..HEAD`, ADRs 0003 and 0005–0010, and the merge
commits' messages.

### Done

#### Exit verification: one test fails; pushed under D23
- `tools\verify.ps1 -Full -Jobs 4` on 2c499a2, a clean tree, after `night.py build-gate` (04:50–04:59, 8 min
  21 s). **One test failed**; every other result is green.
  - Windows: debug 367/368 (1 skipped), release 370/371 (1 skipped), asan 370/372 (1 skipped, **1 failed**). The
    skip in all three is `FrostProcess.RunsFromPathLongerThanMaxPath` (LongPathsEnabled=0, question E1).
  - WSL (`verify.sh --full`): debug 334/334, asan (ASan+UBSan) 338/338, tsan 337/337, licensecheck ok.
  - Checks: format ok, frost_lint ok (279 files), licensecheck ok, featurecheck ok; tidy not-run (M1).
- **The failure.** `FrostProcess.PlatformPluginPathBoundary` in the Windows ASan tree (RelWithDebInfo): both of its
  launches of a copied frost.exe exit with 0xC0000106 (STATUS_NAME_TOO_LONG) before any frost code runs. Re-run
  alone it fails the same way, and the long-path tests beside it pass. It had never run in a Windows ASan tree: it
  came with d41f2a1, after the last `-Full`, and `m0/dll-copy` ran ASan for tests/sched and tests/io only.
- **Root cause, measured** on a hidden desktop with critical-error boxes off. frost::io's `launchPath` passes the
  plain path below MAX_PATH and the `\\?\` form from 260 units on. With LongPathsEnabled=0 and a plain launch, the
  loader refuses each DLL that frost.exe loads at start-up (its static imports beside it) and that embeds an
  isolation-aware manifest (RT_MANIFEST 2), once its path is longer than 250 UTF-16 units. That is the limit ADR
  0003 records for Qt's plugins, here hitting frost's static imports. The ASan runtime `clang_rt.asan_dynamic-x86_64.dll` (32 units, RT_MANIFEST 2) loads at 250 units
  and fails at 251, from a directory of 218 units. The test's two directories are 225 and 226 units (the plugin at
  250 and 251), so the test cannot pass in that tree as written.
- **The same gap in the product.** Release frost dies with 0xC0000106, and no message, from directories of 236 to
  249 units (`Qt6Widgets.dll` beyond 250 units); Debug does from 235 (`Qt6Widgetsd.dll`). Below that, the start-up
  check refuses cleanly (exit 1, from 226 units in Release). From 250 on, the `\\?\` launch starts frost and the
  check refuses cleanly again. No test covers 235–249, so only ASan showed it: its runtime has the longest name.
- No code or test changed on `m0/foundation`: e4f5756 records the finding in ADR 0003 (Consequences) and as a
  `TODO(M1)` at `kQtPluginPathLimitUnits`, docs and comments only. A pass in ASan was first thought to need either a
  skip, which weakens the gate, or a change to how frost starts. Option (d, revised) on the unmerged branch
  `m0/e4-asan-runtime` is a third way: a test-fixture change in the ASan tree only, with `-Full` green there and an
  approving review whose issues are handled, apart from one nit. All of it is question E4 (new).
- **Pushed at 05:24 under D23**, although `-Full` had failed. A push needs CLAUDE.md's definition of done
  (`verify -Quick` on Windows and `verify.sh` in WSL, both green) and an independent review of every branch, which
  were met; `-Full` gates the M0 sign-off (PLAN.md section 7, "Every milestone exit also requires"). 2741b57's footer
  shows the failure ("Branches" below).
- The `-Full` log, the measurement script, its results and the re-run are in `E:\FrostBuilds\perf\m0\exit-full\`
  (`README.txt` maps them).
- **sched's exit-scale TSan stress, re-run on 3563873: clean** (06:35–06:38, after `night.py build-gate`). The two
  sched exit criteria (TSan clean over 10k SimExecutor seeds; the real pool, 1M tasks × 100 iterations with
  cancellation) had been shown at the exit scale only by a run that ended on 2026-10-31 at 19:38. That was before
  a51c23b (`join()` on a ready handle, the shutdown flush, results taken once) and round 1 (23127ab) changed sched,
  and `-Full` runs the stress at its default 20 iterations. The re-run is
  `FROST_STRESS_FULL=1 TSAN_OPTIONS=halt_on_error=1 sched_stress_tests` in the WSL TSan tree (RelWithDebInfo) of
  verify.sh's mirror, synced as verify.sh does (0 paths differ). Ninja had nothing to rebuild in the sched targets,
  so the binary is `-Full`'s from 04:58. Exit 0, `[ PASSED ] 3 tests`, **no TSan report**:
  - SimExecutor: 10,000 seeds, 1,362,189 dispatches (as on 2026-10-31), 8 threads, 0.6 s.
  - ThreadPool: 100 × 1,000,000 tasks on 23 workers (low-lane cap 20), 8,461,798 skipped, 372,943 cut short,
    134.7 s; every invariant held in every iteration.
  - Deque: 20M items, 6 thieves, 37.9 s. Wall 178.5 s, peak RSS 427 MB.
  - Logs: `E:\FrostBuilds\perf\m0\sched-exit-scale-2026-11-01\` (`README.txt` maps them). ADR 0002's exit-scale
    table still shows the 2026-10-31 run.

#### Merged into `m0/foundation` tonight
Every M0 branch is merged, each with a verification footer on its merge commit (`m0/startup` came in as the single
commit d41f2a1, which carries its own).
- **frost::io** (`m0/io`, 008a2de; ADR 0003). Strict UTF-8 ⇄ UTF-16 conversion. Absolute-path rules for drive, UNC
  and `\\?\` forms, with normalisation and extended-length paths that work with LongPathsEnabled=0. One errno/Win32
  → ErrorCode mapping. `IFileSystem` with atomic same-directory writes that keep the target's mode, owner, group
  and access ACL on POSIX as far as allowed. `IProcessLauncher`: absolute executables, exact argv quoting, batch
  files refused, a kill-on-close job object or session, deadline-bounded communicate. The doubles MemFs and
  ScriptedProcessLauncher; `realProcessLauncher` replaces tests/support/process_runner. Independently reviewed in
  four rounds: the first found one high (the batch-file bypass), fixed in 6bbed4d; the review of 66dbabc passed.
- **Start-up hardening** (`m0/startup`, d41f2a1, one commit on 008a2de).
  - `hardenProcess()` is main()'s first statement: default DLL directories, `SetDllDirectoryW("")`, safe search mode
    and NoDefaultCurrentDirectoryInExePath=1, plus `/DEPENDENTLOADFLAG:0xA00` for frost.exe's static imports. The
    self-test checks both.
  - `suppressCrashDialogs()` for the command-line modes, so unattended runs never hang on a dialog.
  - `checkPluginPaths()` before QApplication exists: the platform plugin Qt is about to use must exist in
    `<exe dir>\platforms` and, while long paths are off, be within the 250-unit limit. Otherwise the self-test
    reports "FAIL plugin-paths" and exits 1, and the GUI shows "FrostIDE cannot start" and exits 4.
  - frost-lint rule `exe-search` bans SearchPathW, system, popen, the `_spawn*p`/`_exec*p` families, execvp and
    posix_spawnp.
  - The review's findings (verdict changes-needed) are handled in the same commit. `RunsFromPathLongerThanMaxPath`
    skips while the switch is off, so the M0 exit criterion stays visibly unmet (E1).
- **Comment-accuracy review, four rounds** (`m0/comments-1` to `m0/comments-4`), each fix pass checked
  independently. Comment fixes change no behaviour; each code defect found is fixed with a test that fails without
  the fix.
  - Round 1, compat, core, sched, tools and the build: 45 findings plus four corrections. Code defects: the `#embed`
    probe regex, STL hardening in sanitizer builds, `ThreadPool`'s noexcept constructor, featurecheck refusing to
    overwrite an invalid matrix.json, the layering check's regex, and frost-lint's exit status 2 for environment
    errors.
  - Round 2, frost::io and tests/support: 41 findings. Code defects: `createDirectories` through a dangling
    junction, MemFs verbatim roots, well-formed UTF-8 checks in `validateProcessSpec` under Windows rules, and waits
    with `Duration::max()` ending early.
  - Round 3, tests/perf (the probes and the S1 tools): 34 findings. Code defects: runs with `--inject-cpu-ms` no
    longer get a B6 verdict, and the run watchdog covers every key.
  - Round 4, src/app and tests/app: 19 findings. Code defects: Qt no longer sees the arguments after `--`, so
    `-platform` cannot bypass the plugin check, and `PlatformPluginPathBoundary` counts UTF-16 units. The 250-unit
    limit is the Windows loader's, for DLLs with RT_MANIFEST 2, not Qt's; ADR 0003 says so since 5e90b49. Whether
    LongPathsEnabled=1 lifts it is unmeasured.
- **sccache** (on `m0/comments-1`: b03609d, 8e1e46e). `cmake/FrostCompilerCache.cmake` sets the compiler launcher
  when the pinned sccache 0.19.0 is found, on Windows only (Linux has no pin, F2), and requires `SCCACHE_DIR` on E:.
  The clang-cl pins are now spelled `-vctoolsdir<dir>`, `-winsdkdir<dir>`, `-winsdkversion<v>`. With the old
  spelling every compile was non-cacheable, and clang-cl ignored `/winsdkversion:<v>`, so the compile-side SDK
  version was never pinned (ADR 0001 amendment; REF-W1 has only 10.0.26100.0). A cached sccache path that fails its
  check is searched for again (CTest `FrostCompilerCache.RechecksCachedSccache`). The branch also moved the .NET
  caches to E: in `tools/env.ps1` and added bootstrap.ps1's Python 3.11 check.
- **Runtime-DLL copy race** (`m0/dll-copy`: 6494415, 1252a18, 5978075; stopgap 51e9a05). The first verify of the
  merged tree failed its Windows Debug build with "open_for_read(...Qt6Widgetsd.dll): permission denied". vcpkg's
  z-applocal POST_BUILD step of one executable read a DLL while another executable's POST_BUILD copy wrote it into
  the same output directory. The race was also latent in tests/app, and in the ASan tree for tests/sched, tests/io
  and the S1 tools. Now each output directory has one copy target, `frost_runtime_dlls_<dir>`, with the union of
  its executables' DLLs (Qt, and the ASan runtime in ASan builds). Every executable in that directory, registered or
  not, is ordered after it. New CTest `FrostRuntimeDlls.CopiesOncePerDirectory`, which fails against the old helper
  (3 writers into one directory) and against 6494415.
- **Spikes**, each adversarially verified until the verifier confirmed it: `m0/s1` (87f376a), `m0/spikes-msbuild`
  (78373d4), `m0/s7` (8324436) and `m0/tracy` (377295b). Results below. B28 is recorded in 2c499a2.

#### S1: editor rendering and first frame (ADR 0007, proposed; questions A1–A4)
- **Probes and tools** (2026-10-31). `tests/perf/probes` holds `frost::perf_probes` in the diag layer, Windows parts
  only: Desktop Duplication with a frame marker and pixel samples, DWM timing and a vblank clock, the
  `ISessionState` precheck, frame analysis, delay-validation logic and a JSON report; 67 headless unit tests. On a
  GDI window it was validated by injected 8 and 25 ms delays (errors +0.12 and -0.57 ms) and by stalls and cadence.
  `tests/perf/s1` holds `s1_probe_validate`, `s1_editor` (app layer: the raster QGlyphRun view and a QRhiWidget
  D3D11 glyph atlas, with 16 headless tests), `s1_first_frame` and `run-s1.ps1`. They are Windows only, built only with
  `WIN32 AND FROST_BUILD_UI`, and never run by CTest.
- **Review round 1** (bc20789). A verdict needs samples: without enough it is null ("not measured"), and runs with
  too few decoded markers or a capture failure are invalid (exit 3). `s1_probe_validate` adds phase C, a stall-free
  environment floor. Both Qt backends get probe validations: `s1_editor --validate-delays` spins the delay inside
  the paint or `QRhiWidget::render`, so the flip-model swap chain is covered, and `--inject-cpu-ms` is a known
  GUI-thread spin the UI-CPU probe must see within 10 %. `s1_editor --first-frame` gives each backend's own B1. The
  child launcher runs in the executable's directory, in a kill-on-close job. DXGI enumeration stops on any failure,
  the display-state wait no longer outlives its context, the RHI view keeps the marker quads when glyphs overflow,
  and the cycle-meter test is robust under load. `run-s1.ps1` is one suite: the validations, five interleaved
  rounds of all seven configurations, and B1 for frost.exe and both backends (20 warm launches each for frost.exe
  and QRhi); `-DryRun` prints the plan (47 steps, estimated 9:35 on screen).
- **Run 2** (2026-10-31, under load, n=1 per configuration; superseded by runs 3 and 4). At 5120x1392 raster failed
  B6 CPU (p99 9.4 ms with full repaint, 7.9 ms with scroll-blit, 6.7 ms with fill only); QRhi passed it (p99
  1.0 ms) but failed the interval (20.8 ms with text) and missed (6.7 %) criteria. B5 passed on both. frost.exe B1
  warm was p50 246 ms and p99 344 ms (the max of 8); the cold proxy p50 331 ms.
- **Run 3, the main suite**, at bc20789 with the user's go (2026-11-01 01:02–01:11): 47 steps, 8:29, every run
  valid. All 6 probe validations passed: the GDI window at the start and end, delays through the raster and QRhi
  paths, and the UI-CPU injection on both backends. The desktop was the user's busy one (Wallpaper Engine, Discord,
  33–44 % system CPU), not a quiet one.
- **Run 4, a floors follow-up** (01:43–01:48, 4:24 on screen, under the measurement lock). New: `s1_editor
  --full-output` (a window over the whole output; `windowRect` with a unit test, 17 editor tests) and `run-s1.ps1
  -Suite floors`, which interleaves trivial 320x48 windows of both backends with QRhi runs and adds one round at
  5120x1440. Trivial windows missed more than the QRhi editor in 19 of 20 per-round pairs (medians 7.8 % raster and
  9.6 % QRhi, against 5.5 % for QRhi with text), so they are no floor, and they do not track the editor round by
  round, so they are context only. Covering the whole output cut QRhi's misses to 0.56–1.25 %, with every interval
  at 13.9 ms (one round). A trivial QRhi window also switches its vblank-to-screen level, so the switching is
  specific to QRhi's present path.
- **Result.** Raster fails B6 CPU at 5120x1392 in every round (p99 medians 9.18 / 7.38 / 6.29 ms). QRhi passes it
  in 10 of 10 rounds (p99 0.86 ms); its missed and interval criteria are open. B1 warm fails before any editor work
  (frost.exe p50 264.9 ms). QRhi adds 216.9 ms to the first frame.
- **ADR 0007's fix round**, after the verification of b2395cd, corrects the qpcVBlank claim (irregular, a grid
  anchor only), the earlier-runs section, the floor claims, the B1 endpoint (the detector fires on about the third
  composed frame of the fade-in, 12–19 ms late), the cold proxy (bias of unknown direction) and the B2/B15 cost of
  starting QRhi later, and discloses merged captures. All cited raw data is in `E:\FrostBuilds\perf\m0\s1\` (run
  4's console log is `run4-floors\s1-run4.log`). DWM ETW needs elevation and is deferred.

#### S2 and S6 on the MSBuild corpora (ADRs 0005 and 0006; questions B1–B7)
- **S2** (ADR 0005, accepted for the fixture, R2a and R2b). `-getTargetResult:GetClCommandLines` cannot be used
  directly: MSBuild 17.14's JSON formatter throws on its items (MSB1025). FrostIDE injects a wrapper target through
  `ForceImportAfterCppTargets`. p50 per fresh MSBuild process: fixture 358 ms and R2a 342 ms (first session); on
  R2b (Debug|x64 after a NuGet restore, 10 runs each) server 365 ms (20 files), hostlib 405 ms (44 files) and
  Host.EXE 331 ms (3 files, 14 references). TerminalAppLib (C++/WinRT) needs the UWP workload and is not measured
  (B5). Path (c) passes the design-time properties: without them, GetClCommandLines built a project reference,
  including a vcpkg manifest install that left 552 MB in `%LOCALAPPDATA%\vcpkg` (B6). Command lines extracted before
  the restore silently lack the WIL include directory.
- **S6** (ADR 0006, accepted apart from three parts that are proposed and wait for the user, B1–B4). Path (b)
  (net472, MSBuildLocator, StreamJsonRpc) stays M2's plan; D3 is not final. R2a: launch to the first design-time
  build p50 1.60 s; a second request in the same process 110 ms. R2b, B23's first project (Host.EXE, under decision
  5's solution-order rule): ProjectHost's part takes p50 1.90 s (2026-10-31) and 2.02 s (2026-11-01), 20 runs each,
  max 2.33 s over the 40, all with a warm file cache. It is an expected ProjectHost-side lower bound, not a B23
  measurement: M2's ETW measurement decides B23, and 3 s stays the ceiling. Later projects: Host.FeatureTests
  170–185 ms, hostlib 124–131 ms; Host.EXE requested again in a warm host 398 ms. S6 has no cold sample.
- Verified in three rounds (fixes in 26c1a67, d1f6fef, f5c3b7d), with the third round's lows fixed in d3e32d8 and
  d3976ae. The first measurements ran on a loaded machine (G HUB held 43 GB, 0.4–5.2 GB commit headroom, other
  agents building); the re-measurement held the lock `s26` (2026-11-01 01:30–01:32). Raw data is in
  `E:\FrostBuilds\perf\m0\s2-s6\` (`README.txt` maps it).
- ADR 0005 finds that R2b's Windows SDK prerequisite in PLAN.md section 4 and `tests/corpus/manifest.json`
  (10.0.22621) should read 10.0.26100.0. Both files are unchanged; that is B7 (new).

#### S7: stock clangd on R3 (ADR 0008; questions C1–C3)
- Stock clangd 23.1.4 serves R3's persisted index (12,364 shards, 381 MiB) only once all of it is loaded, on one
  core. Both B24 answers came at p50 15.31 s on a quiet machine (20 runs, lock `s7-2`) and at p50 20.93 s under the
  user's own workload (20 runs, lock `s7`).
- **Proposed, pending the user (C1):** B24's M3 value of p50 18.4 s (15.31 s ×1.2) and a quiet machine for the M3
  gate. The value is in budgets.json's M3 slot, whose note says it is proposed; PLAN.md's S7 sentence says the same.
  PLAN.md's B24 row, the M13 targets and the B24 preconditions are unchanged.
- **Other findings.** A first-ever index build at `-j=4` peaks at 8261 MiB of commit, above B26's 8 GB peak on its
  own. M13's p50 2 s is about 8× faster than stock clangd's load (the planned M7/M13 R&D). M3 uses the first
  background-index progress report after the load as the L2 marker.
- **Fix round 3**, after the second re-verification: the two points above are marked as proposed in the ADR's
  status and decisions, with the questions. The Mann-Whitney p is exact two-sided 2/C(40,20) = 1.45 × 10⁻¹¹
  (6.8 × 10⁻⁸ is the normal approximation). The driver's re-send delay is 0.100–0.101 s while both requests cycle
  (0.15 s was a filter threshold). The pooled r = 0.998 is between the two clusters, and set 2's slope (0.09 s per
  1 %) is less than half of set 1's. That FrostEditor had exited before set 2 rests on `conditions.txt` (02:00:58),
  not on a top-12 list. The WSL VM's start in the `-j=8` attempt is bounded to 127–158 s, and leaving out the
  `.asm`/`.rc` entries is marked untested. budgets.json's note says the DXGI monitor-power check was not done and
  names the 21 generated headers.
- **Close-out**, after the verification of 51cfceb. All cited raw data and our scripts are in
  `E:\FrostBuilds\perf\m0\s7\` (32 MB; `README.txt` maps each directory to the ADR section it backs), and the ADR
  cites it, not the session scratchpad; the Instruction.cpp text that the verbose clangd logs echo is replaced by a
  placeholder in the copies. The ADR says that no run called `SetThreadExecutionState(ES_DISPLAY_REQUIRED)` (PLAN
  6.3), that the power plan kept the machine from sleeping and the System event log shows no sleep or resume, and
  that the display does not enter the timings; Decision 2's summary of PLAN 6.3 and T12 is complete. The symbol
  answers' spacing at the answer is 0.101–0.104 s over all 40 runs (0.102–0.104 s was set 1's alone).

#### Tracy (ADR 0009; questions D1–D3)
- ADR 0009 chooses Tracy 0.14.1, the 2026.10.16 baseline's port (0.14.1#1). Using Tracy at all waits for the
  listener exception (D1). The branch is docs only: vcpkg.json, CMake and the build are untouched, and PLAN.md is
  unchanged (the earlier edit of its Dependencies line was reverted, D3).
- **First verification:** the threat model (a connected local process can read and corrupt the client's memory),
  the shutdown order for manual lifetime, the warning counts, the pinning plan (an override in M1), and the
  per-zone cost of the prescribed task zones.
- **Second verification.** With the port defaults, Tracy's symbol worker calls DbgHelp unlocked beside FrostIDE's
  own DbgHelp users, rewrites `_NT_SYMBOL_PATH` and preloads symbols for every module, so item 5 adds
  `TRACY_NO_CALLSTACK` and `TRACY_NO_SYSTEM_TRACING`. Measured on both OSes under the lock `tracy-2`: Tracy runs no
  symbol worker, neither loads DbgHelp nor changes `_NT_SYMBOL_PATH`, and its `ShutdownProfiler` no longer depends on
  the loaded modules (Windows p50 37.5–62.5 ms, against 151.1 ms with Qt loaded before). Zone costs are unchanged,
  and Tracy's own TU has 0 warnings on clang-cl. The shutdown figures give load, loaded modules and time since
  start, and B20's bullet says they are load-dependent. The dev-only manifest feature is required, with its own
  install tree; the release SBOM names only the release preset's tree. The override's precondition (the port tree
  must be in the vcpkg clone) is stated, and the process counts in the conditions are corrected.
- **Third verification** confirmed the ADR; its four lows are fixed: one cell rounded twice (connected, 4 threads,
  **nc**: 55.8 ns, raw median 55.846); the DbgHelp claim says what the probe could show (Tracy itself no longer loads
  `dbghelp.dll` or changes `_NT_SYMBOL_PATH`); the first WSL hold of the lock (lock 1) is compared cell by cell with
  lock 2, outliers included; the cost of giving up call stacks and system tracing is stated.
- **What the exception means** (ADR 0009, threat model). While Tracy runs, any local process (and, for a WSL
  instance, any Windows-side process) can connect, read FrostIDE's memory (open buffers, the IDE-bridge token,
  askpass passphrases, the environment) and corrupt it through a heap overflow, so treat it as possible code
  execution inside FrostIDE. Item 5 means no call stacks, no sampled call stacks and no context switches.
  FrostIDE's zones never use call stacks, and on Windows sampling and context switches need elevation, which
  FrostIDE never has, so the real loss is sampling and context switches on Linux.
- Raw data is in `E:\FrostBuilds\perf\m0\tracy\` (254 MiB; `README.txt` maps it): the logs, scripts, probe sources
  and captures of the three measurement passes, the third fix round's recompute script, and the three verifiers'
  re-runs and recompute scripts.
  Tracy's sources, the vcpkg and consumer build trees and the binaries were not kept.

#### B28: the dev loop on R1 (ADR 0010; question F3)
- **B28 is met at M0**, measured on 5978075 under the lock `b28` (04:22–04:42) on a quiet machine: system CPU 0.6 %,
  36.6 GB of commit free, no other agent building. sccache was active on Windows.
  - **One-.cpp rebuild** (target p50 20 s), 10 counted runs per row; a new probe constant per run made every timed
    compile an sccache miss. `src/core/src/error.cpp`: p50 1.58 s with `--target frost`, 3.09 s for the full build
    (max 3.53 s). `src/app/src/main.cpp`: p50 3.33 s either way. `src/sched/src/lane.cpp`: p50 1.16 s for the full
    build; `frost` does not link sched.
  - **`verify.ps1 -Quick -Jobs 4`** (target p50 5 min), 6 runs after probe changes, all green: p50 82.3 s, max
    85.5 s. At p50: Windows 35.8 s (31 s of it serial ctest), checks 3.0 s, WSL leg 43.1 s (23 s of it the Linux
    configure).
- **Proposed, pending the user (F3):** the full default build as B28's reading of "rebuilt and linked" from M1 on.
  Both readings meet the target at M0.
- About 15 s of each Linux configure goes to two FindOpenGL `find_path` searches for headers that are not installed
  (`GL/glu.h`, `GL/xmesa.h`); their NOTFOUND results make CMake search again on every configure. Whether the 38
  `/mnt` PATH entries make them slow was not tested.
- Raw data, the harness (`scripts\b28_bench.py`) and a `README.txt` are in `E:\FrostBuilds\perf\m0\b28\`.

#### Branches
- **Pushed at 05:24** under D23 (time from the remote-tracking reflog), with `-Full` still failing one test (E4) and
  2741b57's footer saying so. origin moved `m0/foundation` from 0e703dc to 2741b57 and gained `m0/io` (66dbabc),
  `m0/startup` (d41f2a1), `m0/s1` (009eb20), `m0/spikes-msbuild` (d3976ae), `m0/tracy` (de4a093), `m0/s7`
  (b540543), `m0/comments-1` to `m0/comments-4` (8e1e46e, 6a12cca, b22884a, 301430f), `m0/dll-copy` (1252a18) and
  `m0/docs` (8027101). All are merged into `m0/foundation`, and none needed a force. `m0/sched` (a51c23b) and
  `m0/tools` (33600e3) were already on origin. No `main` and no tag before your sign-off (G1).
- Pushed later: `m0/foundation` up to 5ee286f (e4f5756, 319fd93 and 5ee286f, all docs), and `m0/e4-asan-runtime`
  at 19157a8: 11a5a23, the comment fix 9c7a10d and a merge of `m0/foundation`, so it merges without conflicts.
  That branch has `-Full` green at 11a5a23 (the later commits are comments and docs) and an approving review, but it
  is not merged: it waits for your answer to E4 (E4 (d)).
- `m0/startup-wip` (343785a) stays local: d41f2a1 rebuilt the start-up work as one commit on 008a2de and
  superseded it.

### Next
- Your answers to the questions below, E4 first: it decides how the failing `-Full` test is resolved, among them
  whether `m0/e4-asan-runtime` is merged (option (d)), and so whether `-Full` can
  be green on `m0/foundation` before sign-off.
- The M0 demo pack (PLAN.md section 6.7: screenshots or a recording, the perf report, the feature-matrix diff, the ADR
  list, the comment-accuracy result) is `E:\FrostBuilds\demo\m0\index.html`; then the request for M0 sign-off (G1).
- Record each answer where it belongs: ADR 0007's status (A1–A4); ADR 0006's follow-ups, budgets.json's B23 notes
  and preconditions, PLAN.md's B23 row, its S6 line and its D3 row (B1–B4); R2b's SDK prerequisite in PLAN.md and
  the corpus manifest (B7); ADR 0008's status, budgets.json's B24 note and PLAN.md's S7 sentence, or the M3 slot set
  to the value you choose (C1, C2); ADR 0009 and PLAN.md's Dependencies line (D1–D3); budgets.json's B28 method
  (F3). ADR 0003 and `kQtPluginPathLimitUnits`'s comment state the measured limit for frost's own DLLs since
  e4f5756; the review's wording points on that commit are listed under E4 (d).
- M1 follow-ups:
  - S1 (ADR 0007): keep QRhi start-up off B1 without moving it onto B2 or B15 (options in its Consequences); profile
    the first `show()`; check QRhi's swap-chain frame latency; a quiet-desktop control and PresentMon's present mode
    for QRhi, to explain its misses at the work area.
  - Port `tests/perf/s1/child_process.cpp` (still `CreateProcessW`) to `IProcessLauncher`, now that m0/io has landed.
  - ADR 0010: the Linux configure's OpenGL searches, parallel ctest on Windows, and the B28 harness moved into
    `tools/perf.ps1`, re-measured as R1 grows.
  - ADR 0009's M1 work list, only if you grant the listener exception (D1).
  - The launcher's fallback to the 8.3 short name for a working directory beyond MAX_PATH (ADR 0003).
  - clang-tidy on changed files in verify (still "tidy not-run").
- M2: ProjectHost, and B23 measured with ETW markers (ADR 0006). M3: the B24 harness, starting from `s7_lsp.py` (in
  `E:\FrostBuilds\perf\m0\s7\spike\`) and `session_state.py` (in its `fix2\` sub-directory).

### Blockers
- **Sign-off:** E4 (the failing ASan test, so `-Full` is not green on `m0/foundation`) and G1 are yours. The push
  is done (05:24, D23).
- **M0 exit criterion not met on REF-W1:** the launch from a path over 260 characters (E1). E4's finding also means
  that E1 (b)'s "otherwise exits promptly with a clear message" does not hold yet for directories of 235–249 units
  (Debug; 236–249 in Release).
- Tracy in FrostIDE (ADR 0009's M1 work list) waits for D1.
- Values that wait for you: B6's gating (A2), B1's target (A3), B23's final values and D3 (B1, B2), B24's M3 value
  (C1).
- Cold samples (B1 cold, B23 cold) need a reboot or an elevated standby-list purge (A4, B1); RAMMap is not
  installed. DWM ETW needs elevation (ADR 0007).
- R2b's C++/WinRT projects need the UWP workload (B5).
- Host commit headroom: G HUB holds about 43 GB (F1); a `-j=8` clangd index ran out of commit tonight.
- VS 2026 Build Tools (v145, STL 14.52) need an elevated install by you; until then v145 fixtures are not covered.

### Questions for the user
Groups A to G as consolidated tonight; questions marked **(new)** come from this exit verification and from
consolidating this log.

**A. Renderer and first frame (S1, ADR 0007)**
- **A1.** Approve QRhi as M1's editor renderer on Windows? Raster stays as the golden oracle and the RDP/WARP
  fallback; Linux keeps raster until a Linux S1 leg runs. Raster fails B6's 5 ms UI-CPU limit by its own cost in
  every round (p99 6.3-9.2 ms at 5120x1392). QRhi passes with p99 below 1 ms, but its start-up adds about 210 ms to
  the first show().
- **A2.** How should B6's missed-refresh and interval criteria be gated?
  - (a) a quiet desktop with the absolute budget;
  - (b) the whole output (5120x1440) with the absolute budget: QRhi fill passed all three criteria there in one
    round;
  - (c) decide at M1, after a quiet-desktop control and PresentMon's present mode for QRhi. ADR 0007 proposes (c),
    with no budget change.
- **A3.** B1 warm already fails before any editor work exists: frost.exe p50 264.9 ms against 150 ms, and the max of
  20 launches is 360.1 ms against 250 ms. Keep 150/250 ms as M1's target, or re-baseline?
- **A4.** A true cold B1 measurement needs either a reboot (first-frame steps run first after logon) or an elevated
  RAMMap -Et before each launch. Which, and when?

**B. ProjectHost and B23 (S2/S6, ADRs 0005 and 0006)**
- **B1.** How is B23's "first project streamed" final value (R2b, p50 3 s) set, and with it when D3 becomes final?
  The spike's ProjectHost-side lower bound is p50 1.90-2.02 s, warm file cache.
  - (i) You take 10 cold samples for S6, each after a reboot or an elevated standby-list purge. D3 waits for them,
    which keeps PLAN's order.
  - (ii) M2's cold B23 test sets the value. Your part moves to M2. M2 builds the C# ProjectHost before B23 is fixed,
    which inverts PLAN's "S6 fixes B23 before D3 is final": a plan change to PLAN's S6 line and D3 row.
  - Under either option, 3 s stays the ceiling unless an ADR with your approval raises it.
- **B2.** B23's trees target (R3, 1.5 s) stays provisional until M2's first trees measurement. That is also a change
  to PLAN's B23 row ("final value set by S6").
- **B3.** B23's proposed preconditions (ADR 0006 decision 6), to be written into budgets.json if you approve:
  - packages restored;
  - Debug|x64;
  - the first project chosen by decision 5's rule (Host.EXE);
  - trust granted at the open request;
  - the gating sample cold.
- **B4.** Whether ProjectHost may start before L1 in an already-trusted root (ADR 0006 decision 2 gives three
  answers). It would put about 2 s of host work beside the tree fill and run project MSBuild logic before L1
  (PLAN's no-network-before-L1 rule).
- **B5.** Installing Visual Studio's "Universal Windows Platform development" workload, needed to evaluate R2b's
  TerminalAppLib (C++/WinRT).
- **B6.** About 552 MB of third-party downloads that vcpkg left on C: during the spikes: keep or delete?
- **B7 (new).** May PLAN.md section 4 and `tests/corpus/manifest.json` give R2b's Windows SDK prerequisite as
  10.0.26100.0 instead of 10.0.22621? The pinned R2b commit defaults to 10.0.26100.0, and `sdk/sdk.lock.json` pins
  it (ADR 0005, Consequences; ADR 0006, follow-ups). Neither file was changed.

**C. clangd index (S7, ADR 0008)**
- **C1.** Confirm B24's M3 value of 18.4 s (quiet-machine baseline p50 15.31 s x 1.2; it was 20.93 s under your
  FrostEditor workload) and the requirement that the M3 gate runs on a quiet machine with heavy workloads closed.
  tests/perf/budgets.json already carries max 18.4 s for this target, which had no value before; its note marks it
  as proposed and waiting for you. If you choose another value, it changes.
- **C2.** Should tests/corpus/manifest.json pin R3's configuration (LLVM_ENABLE_PROJECTS=clang, Release, Ninja,
  clang-cl) for all R3 budgets?
- **C3.** Budget risks to note, no decision now:
  - B24's M13 target (p50 2 s) is about 10x faster than stock clangd on R3, which loads the whole index on one core
    before answering.
  - A warm clangd on R3 holds 3.16 GiB, most of B26's 4 GB idle budget.
  - Note (new): ADR 0008 says about 8×, from the quiet-machine p50 of 15.3 s; about 10× matches the 20.9 s under
    your workload.

**D. Tracy (ADR 0009)**
- **D1.** Tracy's listener is an unauthenticated 127.0.0.1 TCP port. While it runs, any local process (and, for WSL,
  any Windows process) can read and corrupt the IDE's memory. PLAN 2.2/5.2 require every listener to have a token
  or an ACL.
  - (a) Grant a dev-builds-only exception: started only by an explicit per-run opt-in; on-demand, ONLY_LOCALHOST,
    NO_BROADCAST, NO_CODE_TRANSFER, NO_CALLSTACK, NO_SYSTEM_TRACING (so no call stacks or context-switch capture);
    listener-enumeration and release-build tests.
  - (b) Decline: no Tracy; profiling uses ETW, Perfetto export and external sampling profilers.
  - (c) Defer to M1.
- **D2.** Confirm the rule that budget and perf runs (frost-bench, verify -Perf) never start Tracy. StartupProfiler
  costs about 215 ms on the first-frame path.
- **D3.** Optional wording: change PLAN.md's "tracy (pinned in M0)" to "tracy (version chosen in M0: 0.14.1 from the
  vcpkg baseline; use pending the listener exception in ADR 0009)".

**E. Start-up and paths**
- **E1.** The M0 exit criterion "launch from a path with ... >260 characters" is not met on this machine, and the
  test skips there. The cause, corrected by tonight's comment review, is the Windows loader, not Qt: with
  LongPathsEnabled=0, LoadLibraryW fails beyond 250 UTF-16 units for any DLL that embeds an isolation-aware manifest
  (RT_MANIFEST 2), as every Qt DLL and plugin does. A DLL without one loads at any length. Whether
  LongPathsEnabled=1 lifts that limit has never been measured, because the switch has never been on here. frost's
  refusal message suggests it anyway.
  - (a) Enable LongPathsEnabled. This needs an elevated `reg add HKLM\SYSTEM\CurrentControlSet\Control\FileSystem
    /v LongPathsEnabled /t REG_DWORD /d 1 /f` and is system-wide. It is the only way to find out whether the switch
    helps: RunsFromPathLongerThanMaxPath then runs and measures it.
  - (b) Approve an ADR changing the criterion to "launches when LongPathsEnabled=1; otherwise exits promptly with a
    clear message", which is tested.
  - (c) Fund a workaround: a Qt patch (LGPL obligations) or a short-path plugin copy.
  - (d) Re-execute frost through GetShortPathNameW when the switch is off. It was measured to work tonight; it does
    not help on volumes without 8.3 names (Dev Drive, ReFS).
- **E2.** By M2: should trusted user-script children (MSBuild Exec and build events, CMake configure, later
  Build/Run and the terminal) get Visual Studio-compatible cmd.exe behaviour, with
  NoDefaultCurrentDirectoryInExePath stripped? Or keep the hardened behaviour, where a tool in the current directory
  must be called as .\tool? Restricted children keep it either way.
- **E3.** On POSIX, saving over a read-only (0444) file replaces it silently, because rename needs only directory
  write access. Should frost::io or M1's SaveManager refuse or warn on non-writable POSIX targets?
- **E4 (new).** Tonight's `verify.ps1 -Full` failed one test, `FrostProcess.PlatformPluginPathBoundary`, in the
  Windows ASan tree; every other leg is green. Measured: while LongPathsEnabled=0, the 250-unit limit also applies
  to frost.exe's own DLLs when frost starts through a plain path (shorter than MAX_PATH). The loader then ends the
  process with 0xC0000106 before frost's code runs, so there is no message. That happens from a directory of 236
  units in Release (Qt6Widgets.dll), 235 in Debug and 218 in ASan (clang_rt.asan_dynamic-x86_64.dll), up to 249;
  from 250 the launch uses `\\?\` and frost refuses cleanly. The test's directories (225 and 226 units) are in that
  range in the ASan tree only. ADR 0003 records the finding since e4f5756 (docs and comments only). The work was
  pushed at 05:24 under D23 with the failure in 2741b57's footer, so the push that (c) describes has happened; (c)'s
  other half, keeping the failure as known until E1 is decided, is still yours. How should it be handled?
  - (a) Approve skipping the boundary test where a DLL beside frost.exe has a longer name than
    `platforms\<plugin>` (today only the ASan runtime), with the reason in the skip message and a guard test that
    fails if any other DLL does. This weakens the ASan gate, so it needs an ADR.
  - (b) First make frost refuse cleanly at every length, for example through a small launcher without such imports,
    or by delay-loading Qt; neither is designed or measured. This goes with E1.
  - (c) Record the failure as known until E1 is decided, and push tonight's branches with a footer that shows it.
  - (d, revised; not E1's (d)) Keep the ASan runtime beside frost.exe, but in ASan trees copy it into the test's
    directories without its RT_MANIFEST 2. Branch `m0/e4-asan-runtime` at 19157a8 (11a5a23, a comment fix and a
    merge of `m0/foundation`): pushed and **not merged**; it needs your explicit yes. The review's issues are
    handled apart from one nit (below).
    - Why revised: the proposed (d), which leaves the runtime out of the copy and puts the build directory on the
      child's PATH, cannot work. frost.exe's `/DEPENDENTLOADFLAG:0xA00` makes the loader look for its static imports
      only beside frost.exe and in System32. Measured: such a copy exits with 0xC0000135 with or without the build
      directory on PATH, while compat_tests.exe (ASan, no such flag) finds the runtime through PATH. Moving the
      runtime out would need the ASan build of frost.exe to drop the flag, which changes the product build and the
      self-test's 0xa00 check.
    - What it does: the removed resource is a 364-byte trustInfo block (requestedExecutionLevel asInvoker), which
      has no effect in a DLL and is the only thing that triggers the limit. Measured with the stripped runtime: it
      loads at 251 units, the plugin at 250 passes the self-test, 251 is refused cleanly (exit 1), and Qt6Widgets.dll
      at 251 still ends frost with 0xC0000106, so the product gap is unchanged. The stripped copy is made once per
      run at a short scratch path. frost.exe and Qt's DLLs stay as built, the Release and Debug copies are unchanged,
      and no assertion changes. A new guard ASSERT before the first launch fails if any DLL beside the copied
      frost.exe still embeds RT_MANIFEST 2 and is not named shorter than `platforms\<plugin>`. A reverted scratch
      check showed it failing on a 24-unit DLL and on an unstripped runtime, and passing at 23 units. ADR 0003's new
      bullet says what the test does in ASan trees and why.
    - `-Full` in the branch's worktree, on the content committed as 11a5a23 (05:41–05:50, after
      `night.py build-gate`, 36.4 GB of commit free), is **green**: win debug 367/368 (1 skipped), win release
      370/371 (1 skipped), win asan 371/372 (1 skipped), wsl debug 334/334, wsl asan 338/338, wsl tsan 337/337, wsl
      licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run.
    - Independent adversarial review: verdict **approve**, weakening **No**. "In no build flavour does the test now
      check less of the product than before. No assertion, budget, warning or skip was relaxed; the only change to
      what is asserted is one new ASSERT." Release and Debug get byte-identical copies, and the guard is a no-op
      there today (longest DLL names 14 and 15 units against thresholds of 24 and 25). With the stripped runtime,
      ASan frost still dies at 236 and 242 units (measured), because Qt6Widgets.dll still passes 250.
    - The cost, in the review's words: "this turns green the only red signal of the product gap." The silent death
      from 236–249 units (Release) and 235–249 (Debug) then lives only in the `TODO(M1)`s in ADR 0003 and
      startup.hpp and in (b). No test went red on it at those lengths before or after; (c) keeps it visible, and (a)
      at least shows a skip. "It is a fixture change in one flavour, disclosed in the code, the ADR and the commit,
      and it still needs the user's explicit yes as E4 (d)."
    - The review's issues, and how each was handled:
      - Minor, on the branch: `removeIsolationManifest`'s comment says the file must be shorter than MAX_PATH, but
        EndUpdateResourceW fails with error 122 from 256 units already (255 works; measured). No practical effect:
        the strip runs at a short scratch root under %TEMP%, and a failure there fails the test. Fixed in 9c7a10d.
      - Minor, on e4f5756 (ADR 0003): "Below that range the start-up check refuses cleanly" has no lower bound.
        frost refuses cleanly only from 226 units (Release) or 225 (Debug); below that it starts normally (225 in
        Release and 224 in Debug: selftest PASSED). Fixed in 5ee286f.
      - Minor, on e4f5756: the ranges were sampled at their ends only (Release 234/235/236/249/250, Debug
        233/234/235/249/250, ASan 216/217/218/225/226/249/250), and the ADR calls them measured without saying the
        interiors are inferred. The review measured Release 241 and 245 and Debug 240 (all 0xC0000106), so the
        claim holds. 5ee286f says how the ranges were found.
      - Minor, on e4f5756: "the loader refuses every DLL next to frost.exe that embeds RT_MANIFEST resource 2"
        should say every such DLL the process loads at start-up (frost.exe's static imports). The root-cause bullet
        in "Exit verification" above makes the same broad claim ("every DLL beside frost.exe"). Both fixed in
        5ee286f.
      - Follow-up: E4 listed only (a)–(c), and "Exit verification" said a pass in ASan needs a skip or a change to
        how frost starts. This entry now records (d).
      - Nit: `removeIsolationManifest` fails with error 87 if a future runtime lacks RT_MANIFEST 2 in language 0x409
        (fail-closed, but a spurious failure after an LLVM change). That the resource holds only a trustInfo block
        is true today but not checked in code; if a future runtime's manifest gained dependency entries, stripping
        it would change how the runtime binds. 9c7a10d documents both at `removeIsolationManifest`.
      - Nit: `isAsanRuntime` matches the `clang_rt.asan_dynamic` prefix without checking the `.dll` extension
        (harmless today: the directory holds only the .dll). Open: a code change, left for the branch's next
        code commit rather than another `-Full` tonight.
    - Evidence: `E:\FrostBuilds\perf\m0\exit-full\e4-option-d\` (`README.txt` maps it).

**F. Machine and tooling**
- **F1.** Logitech G HUB (lghub_agent.exe) holds about 43 GB of commit. Restarting it would give the builds
  headroom: an -j=8 clangd index ran out of commit tonight.
- **F2.** Linux has no sccache: no pin in sdk.lock.json and bootstrap-wsl.sh installs none. Adding one needs a
  lock-file pin and an ADR. Do you want it?
- **F3 (new).** B28's "rebuilt and linked" (ADR 0010, question 1): should it be (a) the full default build after the
  change (proposed; core p50 3.09 s, main 3.33 s, sched 1.16 s), (b) `--target frost` (core 1.58 s, main 3.33 s), or
  (c) the executable or executables that cover the change, such as the changed module's test target? Should
  budgets.json's B28 method also fix the probe files and the job counts? Every reading meets the target at M0, so
  nothing waits on the answer.

**G. Sign-off**
- **G1.** M0 sign-off: after it, tag the M0 build, create main from m0/foundation and push it, then start M1.

## 2026-10-31: M0 bootstrap, session 1

### Done
- **Toolchain.** The pinned Windows toolchain is installed under E:\sdk by `tools/bootstrap.ps1`, and the Linux toolchain
  under `~/sdk` in WSL by `tools/bootstrap-wsl.sh`. Qt 6.12.0 is installed by `tools/qt-fetch.py` on both. Updates.xml
  and every Qt archive are SHA-256-pinned in `sdk/sdk.lock.json`. bsdtar extracts them, so there are no pip packages.
  vcpkg's tag is asserted against `vcpkg.json`'s builtin-baseline.
- **Build system.** Presets for clang-cl and clang + libstdc++ 14, overlay triplets, per-compiler warnings, and
  FrostLayers.cmake with its layering and Qt-only-in-ui/app checks. STL and SDK are pinned via
  `/vctoolsdir`/`/winsdkdir` (ADR 0001).
- **Modules.**
  - `frost::compat`: feature probes, function_ref, saturation arithmetic, FROST_ASSUME.
  - `frost::core`: Error/Result, contract macros with four semantics, strong index types, the IClock seam, and
    annotated Mutex/LockGuard/UniqueLock.
  - `frost::app`: command line, the M0 window, `--version`, `--selftest`.
  - The Windows manifest is embedded in frost.exe and in every test executable.
- **Verification.**
  - `tools/verify.ps1` and `tools/verify.sh` support `-Quick`/`-Full`. They run on a per-worktree ext4 mirror with
    flock, apply job caps and the `stress` label, and report skipped tests as skipped.
  - Checks: clang-format, frost-lint, licensecheck (on each platform's build trees) and featurecheck.
  - Full gate: win debug, release and ASan; WSL debug, ASan+UBSan and TSan, all green.
  - Windows ASan works: an ASan triplet, explicit runtime link inputs, and a heap-overflow liveness probe.
  - `toolchain_probe` checks the C++26 allowlist and the compile-fail probes. `FrostLayers.RejectsViolations`
    checks the link-graph layering rules.
- **Repository checks** (`m0/tools`, merged into `m0/foundation`):
  - frost-lint v0, licensecheck with `--sbom`, and featurecheck with `tests/features/matrix.json`.
  - `tests/perf/budgets.json` (B1–B28), the corpus manifest, and `tools/corpus_fetch.py` (ADR 0004).
- **Independent review** of `m0/foundation`: all 21 findings were applied in 4d047fd, and a re-review of that diff
  is running.
- **Spikes and docs.** ADR template; ADR 0001 with the S4, S5 and S9 results.
- **WSL.** `.wslconfig` sets a 6 GB cap, 8 GB swap on E: and mirrored networking.
- **Spikes.** S2 is in ADR 0005 (GetClCommandLines, p50 358 ms on the fixture) and S6 in ADR 0006 (ProjectHost
  cold start, p50 1.49 s).
- **Branches.** `m0/foundation` (including `m0/tools`) and `m0/tools` are pushed to origin.
- **In flight** (parallel agents, separate worktrees):
  - `m0/sched`: Chase-Lev pool, lanes, Task<T>, SimExecutor, the exit-criterion stress and perf tests.
  - `m0/io`: UTF-8/16, extended-length paths, IFileSystem + MemFs, IProcessLauncher + scripted double.
  - Both have been sent the frost-lint findings from their worktree snapshots.

### Next
- Act on the re-review, then review and merge `m0/sched` and `m0/io` (independent review for sched), run
  `verify -Full` on both sides and merge to `main`.
- Remaining spikes: S1 (first frame), S7 (clangd on R3), the Tracy version, and the R2a/R2b re-measurement of
  S2 and S6.
- B28 measurement on a quiet machine; the ≥40 unit test count is already met.

### Blockers
- Host commit headroom is about 5–6 GB, because `lghub_agent.exe` (Logitech G HUB) holds about 43 GB of private
  memory. That limits parallel builds, and it crashed the WSL VM once before the cap was set. Restarting G HUB is
  the user's call.
- VS 2026 Build Tools (v145, STL 14.52) need an elevated install by the user. Until then, v145 fixtures are not
  covered.

### Questions for the user
- None blocking. `gh auth login` would let the agent open PRs. Pushes already work through Git Credential Manager.
