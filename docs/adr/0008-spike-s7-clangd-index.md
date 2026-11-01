# ADR 0008: Spike S7, stock clangd index load and first workspace/symbol on R3

- Status: accepted.
- Date: 2026-11-01
- Plan reference: PLAN.md section 2 ("Semantic engine"), section 3 (warm open step 4, availability level L2),
  section 4 (B24, B26), M0 spike S7

## Context
M3 runs stock clangd from the pinned LLVM 23.1.4. On a warm reopen, clangd loads its persisted background index,
and FrostIDE reaches L2, navigation from the persisted index. B24 times that on R3: "first workspace/symbol and
cross-file definition from the persisted index". The M3 target is "S7 baseline ×1.2" and the M13 target is p50 2 s,
p99 3 s. S7 measures the baseline with stock clangd on R3 (llvm-project at llvmorg-23.1.4, commit 7a941dfb2, the
corpus manifest's pin), so the M3 value is a number before M3 starts. The spike also records how clangd loads the
index, what it costs in memory, and what building the index costs.

## Decision
1. **B24's M3 value is p50 25.1 s on REF-W1**, the S7 baseline p50 of 20.93 s (20 runs) ×1.2. It goes into the
   structured slot of `tests/perf/budgets.json` (B24, gate M3, p50). The PLAN.md cell stays verbatim. budgets.json
   has no M3 p99 slot; for reference, the measured maximum ×1.2 is 26.9 s.
2. **What the baseline measures, and so what M3 must measure the same way.** The clock starts at clangd's launch,
   and the restored editor's file is opened at once. The sample is the later of two events. The first is the first
   `workspace/symbol` answer for `SmallVector` that comes from the persisted index. The second is the first
   `textDocument/definition` that resolves to the definition in another file. Other conditions: a complete, valid
   persisted index, a warm file cache, and the flags the index was built with. An answer counts as "from the
   persisted index" only if it holds something the opened file's dynamic index cannot hold: for the symbol query,
   `llvm::SmallVector` in SmallVector.h together with symbols declared in other TUs' main files; for the
   definition, a location in `BasicBlock.cpp`.
3. **Stock clangd answers from the full persisted index or not at all.** No shard is loaded lazily. Before L2,
   answers come only from the dynamic index of the opened file: partial symbol results at about 2.4 s, and a
   definition request lands on the declaration in the header at about 2.0 s. The first persisted answer was
   identical to the answer after the load settled in all 20 runs: 100 results, which is clangd's default limit.
4. **First-ever index builds of R3 on REF-W1 run at `-j=4` or less** while G HUB holds about 43 GB of commit. The
   `-j=8` attempt ran out of commit. The index load on a warm reopen uses about one core whatever `-j` is, so `-j`
   matters only for building and re-indexing.

## Measurements
- **Machine.** REF-W1: Ryzen 9 5900X (12 cores, 24 threads), 32 GB, Windows 11 10.0.26300. The corpus, build
  directory and index are on E:, a SATA SSD (Samsung 860 EVO) formatted NTFS, so not a Dev Drive. The Defender
  service was running; its scan settings could not be read (`Get-MpComputerStatus` failed with a provider error).
- **clangd.** `E:\sdk\llvm-23.1.4\bin\clangd.exe` 23.1.4 (7a941dfb2), the official package, launched as
  `clangd --background-index -j=4 --compile-commands-dir=E:\corpus\R3-build --log=info`, the flags of the index
  build. There is no `.clangd` file in R3 or the build directory, and there is no user `config.yaml`. clang-tidy
  ran with R3's own `.clang-tidy` files (the clangd default).
- **R3 build directory.** Configured with `cmake -S llvm -G Ninja -DLLVM_ENABLE_PROJECTS=clang
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, using FrostIDE's
  `cmake/toolchains/windows-clangcl.cmake` and `CMAKE_ASM_MASM_COMPILER=llvm-ml64.exe`. A first attempt had failed
  because no ml64 was found. Configure took 48 s. Before indexing, the TableGen targets were built (804 Ninja
  steps, 198 s) plus 21 generated headers they miss (3 s), so every TU sees its `.inc` files. Those steps compiled
  only the TableGen tools and their libraries (589 objects: LLVMSupport, LLVMDemangle, LLVMTableGen,
  LLVMCodeGenTypes, llvm-min-tblgen, llvm-tblgen, clang-tblgen). `compile_commands.json` has 5945 entries: 5666
  distinct C/C++ files, the 4 BLAKE3 `.asm` files and `windows_version_resource.rc` (269 entries); clangd enqueues
  5671 files. 5586 commands carry CMake's clang-cl PCH flags (`/Yu /Fp /FI cmake_pch.hxx`). Only LLVMSupport's two
  `.pch` files exist (built for the TableGen tools). The opened TU's LLVMCore `.pch` does not, and the TU still
  parsed without errors, with 9 clang-tidy warnings and 1 unused-include warning.
- **Driver.** `s7_lsp.py` is a standard-library LSP client, kept with the raw logs in the session scratchpad.
  Each run starts a fresh clangd, and time is `perf_counter()` seconds since just before `Popen`. The driver sends
  `initialize`, then `didOpen` for `llvm/lib/IR/Instruction.cpp`. It then keeps repeating two requests, each
  re-sent 50 ms after its previous answer, until each comes from the persisted index:
  - `workspace/symbol` for `SmallVector`;
  - `textDocument/definition` at 0-based line 86, character 22 of the same file. That position is the `getModule`
    in `return getParent()->getModule();` (`Instruction::getModule`). Its declaration is BasicBlock.h:220 and its
    definition BasicBlock.cpp:220.

  Answer times are taken when the reader thread receives each response, so the sample resolves to about 0.1 s.
  Memory comes from `K32GetProcessMemoryInfo`, with private bytes as "private" and `PeakPagefileUsage` as "peak
  commit". After both answers the driver waits for the background-index progress to end, asks once more, and shuts
  clangd down. `s7_stats.py` makes the summaries, and `s7_phases.py` maps verbose clangd log markers onto the
  driver's clock.
- **Procedure.** First an untimed sanity run at `-j=4` with `--log=verbose`, behind `night.py build-gate`. Then,
  inside the `night.py` measurement lock `s7` (01:18:10–01:28:59): one warm-up run, 20 timed runs, and 3 runs with
  `--log=verbose` for the phase breakdown. 3 s pass between runs.
- **Conditions.** No build of ours ran; other agents waited behind the lock. The user's own processes kept running
  and were not touched:
  - FrostEditor: 4 instances with 25 FrostRuntime helpers, 5.6 GB private, and about 5.5 cores busy when sampled
    at 01:13;
  - Discord, 2.1 GB private;
  - Wallpaper Engine;
  - the WSL VM, 3.9 GB, idle for us;
  - one process under `E:\r11`.

  G HUB held only 175 MB during the runs. In each run, the system CPU averaged 35–48 % (p50 40 %), and free commit
  never fell below 21.7 GiB (22,268 MiB). `night.py measure-begin s7` printed:
  ```
  {"measurement": "s7", "start": "2026-11-01T01:18:10", "free_commit_gb": 24.4, "commit_limit_gb": 64.1, "free_phys_gb": 7.8, "system_cpu_pct": 46.4, "our_builds_running": [], "builds_wait_timed_out": false, "top_cpu_processes": [{"Name": "Discord", "Id": 24484, "ws_mb": 747}, {"Name": "Discord", "Id": 2364, "ws_mb": 86}, {"Name": "wallpaper64", "Id": 24044, "ws_mb": 156}, {"Name": "WindowsTerminal", "Id": 10768, "ws_mb": 134}, {"Name": "claude", "Id": 8908, "ws_mb": 489}, {"Name": "Discord", "Id": 14552, "ws_mb": 89}, {"Name": "FanControl", "Id": 22180, "ws_mb": 47}, {"Name": "RazerAppEngine", "Id": 24352, "ws_mb": 341}]}
  ```
  (`top_cpu_processes` is sorted by accumulated CPU time, not current load.)

**B24 on R3** (fresh clangd per run, `-j=4`, `--log=info`, warm file cache; seconds since clangd launch; at
n = 20 the maximum stands in for p99).

| Measurement | Runs | min | p50 | max (= p99 at n < 100) |
|---|---|---|---|---|
| **B24 sample: both answers from the persisted index** | 20 | 19.77 | **20.93** | 22.45 |
| … first `workspace/symbol` "SmallVector" from the persisted index (100 results) | 20 | 19.72 | 20.89 | 22.40 |
| … first cross-file definition (BasicBlock.cpp:220) | 20 | 19.77 | 20.93 | 22.45 |
| Background-index progress end | 20 | 21.12 | 22.38 | 23.95 |
| Before L2: first `workspace/symbol` holding `llvm::SmallVector` (dynamic index, 31 results) | 20 | 2.22 | 2.42 | 2.48 |
| Before L2: first definition answer, the declaration (BasicBlock.h:220); opened file `idle` | 20 | 1.87 | 2.00 | 2.05 |
| `initialize` reply | 20 | 0.019 | 0.021 | 0.039 |
| Warm-up run (run 0, not in the sample): B24 sample | 1 | | 22.03 | |

**Memory of the clangd process** (MiB, same 20 runs). B24's answers arrive together, so "after the first answer"
and "after both answers" are the same instant.

| Measurement | Runs | min | p50 | max |
|---|---|---|---|---|
| Private bytes after the answers | 20 | 3226 | 3234 | 3246 |
| Working set after the answers | 20 | 3046 | 3051 | 3059 |
| Peak commit during the run | 20 | 3534 | 3542 | 3550 |
| Peak working set during the run | 20 | 3281 | 3286 | 3290 |

**How the index loads** (runs 21–23 with `--log=verbose`, otherwise identical; clangd's log markers mapped onto
the driver's clock through the `initialize` reply). These three runs gave the same B24 sample as the info-level
runs, p50 21.03 s.

| Phase (s since launch) | min | p50 | max |
|---|---|---|---|
| Background index enqueued (right after `didOpen` loads the compilation database) | 0.14 | 0.14 | 0.15 |
| All shards read: `BackgroundIndex: building version 1 after loading index from disk` | 9.36 | 9.38 | 9.41 |
| Index served: `BackgroundIndex: serving version 1 (2051929463 bytes)` | 20.92 | 20.98 | 21.05 |
| First persisted answer after "serving" | +0.003 | | +0.074 |
| Progress end after "serving" | +1.51 | +1.52 | +1.52 |

- **Load phases.** Reading the 12,364 shards (381 MiB on disk) takes 9.2–9.3 s. Building the in-memory index
  (1.91 GiB) takes 11.5–11.7 s. Over the 20 timed runs, clangd used 2.3 cores (p50) in the first 3 s, while it
  built the opened file's preamble and the standard-library index. From then until the answer it used 1.03 cores
  (1.01–1.04), so the load is serial. Three `-j=8` runs under load had the same one-core profile. The persisted
  index serves all at once: no answer came from part of it.
- **The index is not rebuilt.** None of the 12,365 files in the index directory changed during the runs, except
  `.gitignore`, which clangd rewrites on every start. The 5 `.asm`/`.rc` entries have no shard, because indexing
  them fails with "Couldn't build compiler invocation". clangd logs "Failed to load shard" for them and retries
  them on every start. 1.4–1.5 s after the index serves, it reports a re-index queue of "0/6", and the retries
  fail within about 20 ms, which ends the progress. In that 1.4 s gap clangd keeps one core busy (0.94–1.01 cores
  over the 20 runs) and logs nothing about the index; the answers are already served. No C/C++ file was
  re-indexed.

**Other runs** (not part of the result).

| Run | Conditions | B24 sample | Notes |
|---|---|---|---|
| Sanity run, 01:16, `-j=4`, verbose, before the lock | First load in about three hours, so shards were probably partly out of the file cache; system CPU 55–67 % | 31.28 s | Shards read in 16.34 s (9.25 s warm), index built in 14.49 s; progress end 6.9 s after "serving" |
| Earlier agent, 2026-10-31 22:24–22:26, `-j=8` | Other agents building; system CPU 48–97 % | 22.12 / 25.62 / 35.59 s (3 runs) | Kept as an "under load" data point |

**Building the index** (2026-10-31, by the earlier S7 run, with `--log=info` and Instruction.cpp opened; upper
bounds, because the machine was loaded):

| | `-j=8` (attempt 1) | `-j=4` |
|---|---|---|
| Outcome | clangd exited after 237 s at 1020 of 5672 files, when free commit fell to 53 MB | Complete: 5666 TUs indexed, 5 `.asm`/`.rc` failed |
| Wall time | — | 2539 s (42.3 min) |
| CPU time | 1662 s by exit (7.0 cores) | 9844 s (3.9 cores on average) |
| Peak working set / peak commit (MiB) | 6096 / 7034 | 6182 / 8261 |
| Private bytes when finished (MiB) | — | 7835, 60 s after progress end (3043 after 263 s, 4574 after 1312 s) |
| On disk | 5099 shards, 155 MiB (discarded) | 12,364 `.idx` shards, 381.4 MiB (399,932,566 bytes; largest 37 MiB) |
| Machine | G HUB held 44 GB; system CPU mean 56 % | G HUB held 44 GB for the first ~14 min; free commit min 3.7 GB; system CPU mean 38 %; other agents were building (the WSL VM was busy in 31 of 83 samples) |

## Consequences
- **M3 (B24).** The M3 test reproduces Decision 2 on R3 with the IDE's LSP client. FrostIDE adds the time from
  open to clangd launch, so the gate measures from launch, or it reports that offset separately. The B24 harness
  can start from `s7_lsp.py` (to `tests/perf` in M3, with an SPDX header already in place).
- **M3 (L2 marker).** Stock clangd sends nothing when the persisted index starts serving. In these runs, the first
  background-index progress report after the load (the re-index queue, "0/6") arrived 1.4–1.5 s after serving.
  The backgroundIndexProgress `end` comes after the re-index, so on a reopen with changed files it can be minutes
  late. M3 uses that first post-load report as the L2 ETW marker and documents the lag. Its behaviour on a reopen
  with changed files is untested, and M3 checks it. The benchmark keeps the S7 probe method. Whether frost-clangd's
  extension points can report the exact swap is an M7 question.
- **M3 (UI before L2).** For about 21 s on R3, Navigate To and Search Everywhere get only the dynamic index's
  partial results. Go to Definition across files lands on the declaration. The UI marks these results as partial
  while L2 is pending and re-runs an open symbol query once L2 is reached.
- **M3 (start-up order).** In every run, the compilation database was loaded and the index load enqueued only
  after the first `didOpen`. Starting clangd without opening a file was not tested, so the supervisor sends
  `didOpen` for the restored active editor right after `initialize`. `-j` does not speed up the load, so the
  ResourceGovernor cannot shorten L2. It sets clangd's `-j` for the first index build and for re-indexing only.
- **M3 (compile database).** FrostIDE's command synthesis leaves non-C/C++ entries (`.asm`, `.rc`) out of what it
  hands to clangd. Otherwise clangd retries them on every start and logs an error for each. clangd keeps the index
  in `<compile-commands-dir>\.cache\clangd\index`. To keep the index "in FrostIDE's cache" (PLAN.md section 2), the
  synthesized `compile_commands.json` lives in FrostIDE's cache directory, and `--compile-commands-dir` points
  there.
- **M3 (B26 memory).** A warm clangd holds 3.16 GiB private after L2 on R3, which leaves less than 0.9 GiB of
  B26's 4 GB idle budget for everything else. A first-ever index build at `-j=4` peaks at 8261 MiB of commit, above
  B26's 8 GB peak by itself, and ends at 7835 MiB private. The reload measured here frees most of that: 3234 MiB
  after a fresh start. So the M3 ResourceGovernor restarts clangd once the first full index is built, and bounds
  `-j` by free commit, as Decision 4 requires on this machine. B26 itself is unchanged.
- **M3 (PCH).** With CMake's clang-cl PCH flags and the TU's `.pch` not built, stock clangd still parsed the
  opened TU without errors. M3's "PCH and forced-include awareness" item should verify this on the R2 corpora before
  synthesizing PCH flags.
- **M13 (B24 p50 2 s, p99 3 s).** That target is about 10× faster than stock clangd's serial load of R3 (20.9 s,
  9.3 s reading shards and 11.6 s building the index). No `-j` setting or warm-cache change closes the gap. Meeting
  it needs an index that serves without rebuilding it in memory at start-up, or one that serves while it loads.
  That is the T2 R&D the plan already schedules (M7 frost-clangd, M13). It may conflict with the "no patches"
  rule for frost-clangd, which is a decision for M7.

## Alternatives considered
- Using the first non-empty `workspace/symbol` answer (2.4 s) as the baseline: rejected. It comes from the opened
  file's dynamic index and does not include anything from the persisted index, while B24 measures answers "from
  the persisted index".
- Using backgroundIndexProgress `end` as the end point: rejected. It comes about 1.5 s after the index serves, and
  on a reopen with changed files it waits for the re-index.
- Taking the baseline from the earlier `-j=8` runs: rejected. Their flags differ from the index build's, other
  agents were building at the time, and the spread was wide (22.1–35.6 s). The load profile is the same at both
  `-j` values, so `-j=4` loses nothing.
- Measuring at `-j=8` or the governor default (cores/2 = 12): not allowed tonight (at most `-j=4` while other
  agents run). It would not change the load, which is serial, and the first-ever `-j=8` build ran out of commit.
- A monolithic `clangd-indexer` index loaded with `--index-file`: not measured. It is a candidate for the M7/M13
  R&D, not an M3 change.
