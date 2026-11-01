# ADR 0008: Spike S7, stock clangd index load and first workspace/symbol on R3

- Status: accepted.
- Date: 2026-11-01
- Plan reference: PLAN.md section 2 ("Semantic engine"), section 3 (warm open step 4, availability level L2),
  section 4 (B24, B26), section 6.3 (perf runs on REF-W1), M0 spike S7

## Context
M3 runs stock clangd from the pinned LLVM 23.1.4. On a warm reopen, clangd loads its persisted background index,
and FrostIDE reaches L2, navigation from the persisted index. B24 times that on R3: "first workspace/symbol and
cross-file definition from the persisted index". The M3 target is "S7 baseline ×1.2" and the M13 target is p50 2 s,
p99 3 s. S7 measures the baseline with stock clangd on R3 (llvm-project at llvmorg-23.1.4, commit 7a941dfb2, the
corpus manifest's pin), so the M3 value is a number before M3 starts. The spike also records how clangd loads the
index, what it costs in memory, and what building the index costs. The baseline was measured twice with the same
protocol: first while the user's own workload kept the machine busy, then as a quiet-machine control after that
workload had ended.

## Decision
1. **B24's M3 value is p50 18.4 s on REF-W1**: the S7 baseline p50 of 15.31 s from the quiet-machine control
   (20 runs) ×1.2 = 18.37 s. It goes into the structured slot of `tests/perf/budgets.json` (B24, gate M3, p50). The
   PLAN.md cell stays verbatim. budgets.json has no M3 p99 slot; for reference, the control's maximum ×1.2 is
   18.9 s. Under the user's workload the same protocol gave p50 20.93 s, which would have made the value 25.1 s,
   6.7 s looser.
2. **What the baseline measures, and so what an M3 run must reproduce to compare with it.**
   - *Clock and sample.* The clock starts at clangd's launch, and the restored editor's file is opened at once. The
     sample is the later of two events: the first `workspace/symbol` answer for `SmallVector` that comes from the
     persisted index, and the first `textDocument/definition` that resolves to the definition in another file. An
     answer counts as "from the persisted index" only if it holds something the opened file's dynamic index cannot
     hold: for the symbol query, `llvm::SmallVector` in SmallVector.h together with symbols declared in other TUs'
     main files; for the definition, a location in `BasicBlock.cpp`.
   - *R3 configuration, which fixes the index and so the load.* R3 at 7a941dfb2, configured with `-G Ninja
     -DLLVM_ENABLE_PROJECTS=clang -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, FrostIDE's
     `cmake/toolchains/windows-clangcl.cmake` and llvm-ml64, with the TableGen targets and 21 generated headers
     built (see Measurements). clangd reads CMake's own `compile_commands.json`: 5945 entries for 5671 files (5666
     C/C++ files, the 4 BLAKE3 `.asm` files, and `windows_version_resource.rc` with 269 entries); 5586 entries carry
     clang-cl PCH flags. The index built from it has 12,364 shards (399,932,566 bytes) and serves as a
     2,051,929,463-byte in-memory index. Neither PLAN.md nor the corpus manifest pins R3's configuration. A run with
     more projects, another build type, or compile commands that differ for any C/C++ file indexes something else
     and does not compare with this baseline. Leaving out only the 5 non-C/C++ files does compare: they have no
     shard, and clangd retries them only after the index serves.
   - *Index and file cache.* A complete, valid persisted index built by the same clangd with the same flags
     (`--background-index -j=4`), and a warm file cache: one untimed warm-up run comes first.
   - *Machine state.* A controlled REF-W1 run as PLAN.md section 6.3 defines it: the session-state precheck passes,
     and the run is aborted if user input arrives. Also, no heavy workload of the user's runs at the same time; in
     the control, the system CPU averaged 10.7–15.8 % per run, about 5 points of it clangd's own. The sample tracks
     the machine's load. Under the user's workload, B24 rose by 0.20 s per 1 % of system CPU (Pearson r = 0.95 over
     20 runs), and its p50 was 37 % above the quiet control's.
3. **Stock clangd answers from the full persisted index or not at all.** No shard is loaded lazily. Each verbose
   log (3 runs in each set, and the sanity run) has exactly one `building version 1 after loading index from disk`
   and one `serving version 1 (2051929463 bytes)`. Every persisted answer came at most 0.10 s after that line,
   within one poll, and none before it. Before L2, answers come only from the opened file's dynamic index: partial
   symbol results at about 1.7 s, and a definition request lands on the declaration in the header at about 1.4 s
   (quiet control). In all 40 timed runs the first persisted symbol answer was also identical to the answer after
   the load settled. That is weaker evidence, because both are cut at clangd's default limit of 100 results.
4. **A first-ever index build of R3 needs about 8.1 GiB of commit for clangd at `-j=4` (peak 8261 MiB), so the M3
   ResourceGovernor sizes `-j` by free commit.** S7 does not show whether `-j=8` fits on REF-W1. The one `-j=8`
   attempt ran out of commit after 237 s, but the cause is confounded. Between 141 s and 152 s free commit fell by
   2865 MiB while clangd's private bytes grew by 150 MiB. In the same half-minute the WSL VM (another agent's work)
   started and took 2554 MiB, and 3122 MiB about 16 s before the crash; G HUB held 43.3 GiB throughout. `-j=8` does
   commit faster early on (4953 MiB against 2654 MiB at 60 s), but its peak before the crash, 7034 MiB, stayed below
   the `-j=4` build's 8261 MiB. Tonight's builds ran at `-j=4` because the night's coordination rule says so, not
   because of this result. The index load on a warm reopen uses about one core whatever `-j` is, so `-j` matters
   only for building and re-indexing.

## Measurements
- **Machine.** REF-W1: Ryzen 9 5900X (12 cores, 24 threads), 32 GB, Windows 11 10.0.26300, High performance power
  plan. The corpus, build directory and index are on E:, a SATA SSD (Samsung 860 EVO) formatted NTFS, so not a Dev
  Drive. The Defender service was running; its scan settings could not be read (`Get-MpComputerStatus` failed with
  a provider error).
- **clangd.** `E:\sdk\llvm-23.1.4\bin\clangd.exe` 23.1.4 (7a941dfb2), the official package, launched as
  `clangd --background-index -j=4 --compile-commands-dir=E:\corpus\R3-build --log=info`, the flags of the index
  build. There is no `.clangd` file in R3 or the build directory, and there is no user `config.yaml`. clang-tidy
  ran with R3's own `.clang-tidy` files (the clangd default).
- **R3 build directory.** Configured with `cmake -S llvm -G Ninja -DLLVM_ENABLE_PROJECTS=clang
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, using FrostIDE's
  `cmake/toolchains/windows-clangcl.cmake` and `CMAKE_ASM_MASM_COMPILER=llvm-ml64.exe`. A first attempt had failed
  because no ml64 was found. Configure took 48 s. Before indexing, the TableGen targets were built (804 Ninja
  steps, 198 s) plus 21 generated headers they miss (3 s), so every TU sees its `.inc` files. Those steps compiled
  only the TableGen tools and the libraries they link: 607 compile steps, 303 in the main build and 304 in the three
  nested `NATIVE` builds that LLVM's CMake set up for its host tools (211 + 25 + 68). They cover LLVMSupport (with
  BLAKE3), LLVMDemangle, LLVMTableGen, LLVMCodeGenTypes, llvm-min-tblgen, llvm-tblgen and clang-tblgen; 606 of them
  write `.obj` files and one writes LLVMSupport's `.pch`. `compile_commands.json` has 5945 entries: 5666 distinct
  C/C++ files, the 4 BLAKE3 `.asm` files and `windows_version_resource.rc` (269 entries); clangd enqueues 5671
  files. 5586 commands carry CMake's clang-cl PCH flags (`/Yu /Fp /FI cmake_pch.hxx`). Only LLVMSupport's two
  `.pch` files exist, one per build tree. The opened TU's LLVMCore `.pch` does not, and the TU still parsed without
  errors, with 9 clang-tidy warnings and 1 unused-include warning.
- **Driver.** `s7_lsp.py` is a standard-library LSP client, kept with the raw logs in the session scratchpad.
  Each run starts a fresh clangd, and time is `perf_counter()` seconds since just before `Popen`. The driver sends
  `initialize`, then `didOpen` for `llvm/lib/IR/Instruction.cpp`. It then keeps repeating two requests, each
  re-sent 50 ms after its previous answer, until each comes from the persisted index:
  - `workspace/symbol` for `SmallVector`;
  - `textDocument/definition` at 0-based line 86, character 22 of the same file. That position is the `getModule`
    in `return getParent()->getModule();` (`Instruction::getModule`). Its declaration is BasicBlock.h:220 and its
    definition BasicBlock.cpp:220.

  Answer times are taken when the reader thread receives each response. At the answer, consecutive symbol answers
  were 0.102–0.104 s apart and definition answers 0.101–0.102 s, so the sample resolves to about 0.1 s. Memory
  comes from `K32GetProcessMemoryInfo`, with private bytes as "private" and `PeakPagefileUsage` as "peak commit";
  the driver also samples the system CPU every 0.5 s. After both answers the driver waits for the background-index
  progress to end, asks once more, and shuts clangd down. `s7_stats.py` makes the summaries, `s7_phases.py` maps
  verbose clangd log markers onto the driver's clock, and `s7_fix2.py` and `s7_quiet.py` compute the load
  correlation, the CPU profile and the comparison of the two sets.
- **Procedure.** First an untimed sanity run at `-j=4` with `--log=verbose`, behind `night.py build-gate`. Then,
  for each set, inside a `night.py` measurement lock: one warm-up run, 20 timed runs, and 3 runs with
  `--log=verbose` for the phase breakdown, 3 s apart. Set 1, under the user's workload, ran in the lock `s7`
  (01:18:10–01:28:59). Set 2, the quiet control, ran in the lock `s7-2`: taken at 01:59:51, then waiting for other
  agents' builds until 02:00:53, runs 02:01:15–02:09:38, released at 02:09:55. It used the same driver arguments,
  one driver call per run, so that `session_state.py` could check the session before the first run and after every
  run (`run_quiet.ps1`).
- **Session state and input.** `session_state.py` reads `GetLastInputInfo`, opens the input desktop, and asks WTS
  for the lock state and the active console session. Read at 01:53, before the control's first run and after each of
  its 24 runs, it reported the last user input at 01:07:04, so no input arrived during either set. At each check the
  session was the unlocked, active console session. Set 1 had no check before it; the check at 01:53 covers it,
  because unlocking a locked session takes input. The precheck's third item, a powered-on monitor through DXGI, was
  not made. The power plan never turns the display off on AC power, and WMI listed the monitor with Availability 8
  ("Off Line"), which this check cannot confirm. clangd draws nothing, so the display does not enter these timings.
- **Conditions, set 1 (under the user's workload).** No build of ours ran; other agents waited behind the lock.
  The user's own processes kept running and were not touched (`conditions.txt` at the lock's start):
  - FrostEditor: 4 instances, 2384 MiB private, with 25 FrostRuntime helpers, 3235 MiB;
  - Discord, 2123 MiB;
  - Wallpaper Engine;
  - the WSL VM, 3860 MiB, idle for us;
  - one small `python.exe` under `E:\r11`.

  How many cores FrostEditor used was not recorded during the runs. In each run the system CPU averaged
  35.0–48.8 % (p50 39.7 %), of which clangd's own share was about 5 %. G HUB held 175 MiB, and free commit never
  fell below 21.7 GiB (22,268 MiB). `night.py measure-begin s7` printed:
  ```
  {"measurement": "s7", "start": "2026-11-01T01:18:10", "free_commit_gb": 24.4, "commit_limit_gb": 64.1, "free_phys_gb": 7.8, "system_cpu_pct": 46.4, "our_builds_running": [], "builds_wait_timed_out": false, "top_cpu_processes": [{"Name": "Discord", "Id": 24484, "ws_mb": 747}, {"Name": "Discord", "Id": 2364, "ws_mb": 86}, {"Name": "wallpaper64", "Id": 24044, "ws_mb": 156}, {"Name": "WindowsTerminal", "Id": 10768, "ws_mb": 134}, {"Name": "claude", "Id": 8908, "ws_mb": 489}, {"Name": "Discord", "Id": 14552, "ws_mb": 89}, {"Name": "FanControl", "Id": 22180, "ws_mb": 47}, {"Name": "RazerAppEngine", "Id": 24352, "ws_mb": 342}]}
  ```
  (`top_cpu_processes` is sorted by accumulated CPU time, not current load.)
- **Conditions, set 2 (quiet control).** FrostEditor, its helpers and the `E:\r11` process had exited by 01:58;
  nothing of ours stopped them. Discord (2122 MiB), Wallpaper Engine, G HUB (185 MiB) and the WSL VM (3899 MiB)
  kept running; the WSL VM had exited by the end of the set. At the lock's start the other agents' builds had just
  finished: Win32_Processor read 87 % and then 60 %, while no remaining Windows process group used more than
  0.21 cores. The warm-up run's system CPU was 31–44 % in its first 3 s and 9–13 % in its last 5 s. In the 20 timed
  runs the system CPU averaged 10.7–15.8 % per run (p50 11.9 %), of which clangd's own share was about 5 %. At the
  end, Win32_Processor read 4 % and then 1 %. G HUB held 185–187 MiB, and free commit never fell below 27.4 GiB
  (28,065 MiB). `night.py measure-begin s7-2` printed:
  ```
  {"measurement": "s7-2", "start": "2026-11-01T02:00:53", "free_commit_gb": 30.4, "commit_limit_gb": 64.1, "free_phys_gb": 9.5, "system_cpu_pct": 78.4, "our_builds_running": [], "builds_wait_timed_out": false, "top_cpu_processes": [{"Name": "Discord", "Id": 24484, "ws_mb": 749}, {"Name": "Discord", "Id": 2364, "ws_mb": 88}, {"Name": "wallpaper64", "Id": 24044, "ws_mb": 157}, {"Name": "WindowsTerminal", "Id": 10768, "ws_mb": 145}, {"Name": "claude", "Id": 8908, "ws_mb": 493}, {"Name": "Discord", "Id": 14552, "ws_mb": 93}, {"Name": "FanControl", "Id": 22180, "ws_mb": 48}, {"Name": "RazerAppEngine", "Id": 24352, "ws_mb": 342}]}
  ```

**B24 on R3** (fresh clangd per run, `-j=4`, `--log=info`, warm file cache; seconds since clangd launch; at
n = 20 the maximum stands in for p99).

Quiet = set 2, the control; Loaded = set 1, under the user's workload.

| Measurement | Runs per set | Quiet min | Quiet p50 | Quiet max | Loaded min | Loaded p50 | Loaded max |
|---|---|---|---|---|---|---|---|
| **B24 sample: both answers from the persisted index** | 20 | 15.16 | **15.31** | 15.71 | 19.77 | **20.93** | 22.45 |
| … first `workspace/symbol` "SmallVector" from the persisted index (100 results) | 20 | 15.16 | 15.26 | 15.67 | 19.72 | 20.89 | 22.40 |
| … first cross-file definition (BasicBlock.cpp:220) | 20 | 15.11 | 15.31 | 15.71 | 19.77 | 20.93 | 22.45 |
| Background-index progress end | 20 | 16.16 | 16.33 | 16.77 | 21.12 | 22.38 | 23.95 |
| Before L2: first `workspace/symbol` holding `llvm::SmallVector` (dynamic index, 31 results) | 20 | 1.61 | 1.71 | 1.76 | 2.22 | 2.42 | 2.48 |
| Before L2: first definition answer, the declaration (BasicBlock.h:220); opened file `idle` | 20 | 1.37 | 1.39 | 1.43 | 1.87 | 2.00 | 2.05 |
| `initialize` reply | 20 | 0.027 | 0.028 | 0.030 | 0.019 | 0.021 | 0.039 |
| System CPU, run mean from launch to the B24 sample (%) | 20 | 10.7 | 11.9 | 15.8 | 35.0 | 39.7 | 48.8 |
| Warm-up run (run 0, not in the sample): B24 sample | 1 | | 16.94 | | | 22.03 | |

- **The sample tracks the load.** Over set 1's 20 runs, the B24 sample correlates with the run's mean system CPU
  at Pearson r = 0.953 (Spearman 0.716), with a slope of +0.20 s per 1 % of system CPU. The two quietest runs
  (35.0 % and 35.8 %) gave 19.77 s and 19.85 s. The busiest (48.8 %) gave 22.45 s, the maximum. Run 19 also had
  system CPU samples of 93–100 % at 10.7–13.5 s, and in that span its symbol requests were re-sent 0.15–0.54 s
  after the previous answer instead of 0.05 s; at the answer itself the resolution was the usual 0.1 s. Set 1's
  p50 is 37 % above set 2's (Mann-Whitney U = 0, two-sided p = 7 × 10⁻⁸: every loaded run was slower than every
  quiet run). Over all 40 runs, which fall in two clusters, the sample is close to linear in the system CPU:
  12.88 s + 0.20 s per 1 % (r = 0.998). Set 1's own fit predicts 15.40 s at set 2's p50 load of 11.9 %, against
  15.31 s measured. Within set 2 the slope is 0.09 s per 1 % (r = 0.93). The M3 value comes from the measured
  control, not from a fit.

**Memory of the clangd process** (MiB, same runs). B24's answers arrive together, so "after the first answer"
and "after both answers" are the same instant.

| Measurement | Runs per set | Quiet min | Quiet p50 | Quiet max | Loaded min | Loaded p50 | Loaded max |
|---|---|---|---|---|---|---|---|
| Private bytes after the answers | 20 | 3218 | 3234 | 3242 | 3226 | 3234 | 3246 |
| Working set after the answers | 20 | 3041 | 3050 | 3054 | 3046 | 3051 | 3059 |
| Peak commit during the run | 20 | 3526 | 3542 | 3550 | 3534 | 3542 | 3550 |
| Peak working set during the run | 20 | 3276 | 3283 | 3288 | 3281 | 3286 | 3290 |

**How the index loads** (runs 21–23 of each set with `--log=verbose`, otherwise identical; clangd's log markers
mapped onto the driver's clock through the `initialize` reply). Their B24 samples had p50 15.26 s (quiet) and
21.03 s (under load), within 0.1 s of their sets' info-level runs.

| Phase (s since launch) | Quiet min | Quiet p50 | Quiet max | Loaded min | Loaded p50 | Loaded max |
|---|---|---|---|---|---|---|
| Background index enqueued (right after `didOpen` loads the compilation database) | 0.14 | 0.14 | 0.14 | 0.14 | 0.14 | 0.15 |
| All shards read: `BackgroundIndex: building version 1 after loading index from disk` | 6.74 | 6.77 | 6.77 | 9.36 | 9.38 | 9.41 |
| Index served: `BackgroundIndex: serving version 1 (2051929463 bytes)` | 15.05 | 15.16 | 15.20 | 20.92 | 20.98 | 21.05 |
| … reading the shards (enqueued to all read) | 6.60 | 6.63 | 6.64 | 9.22 | 9.25 | 9.26 |
| … building the in-memory index (all read to served) | 8.31 | 8.39 | 8.43 | 11.51 | 11.62 | 11.66 |
| First persisted `workspace/symbol` answer, after "serving" | +0.018 | +0.061 | +0.097 | +0.003 | +0.038 | +0.074 |
| First cross-file definition, after "serving" | +0.009 | +0.045 | +0.066 | +0.021 | +0.051 | +0.086 |
| B24 sample (the later of the two), after "serving" | +0.061 | +0.066 | +0.097 | +0.051 | +0.074 | +0.086 |
| First progress report after the load ("0/6"), after "serving" | +1.05 | +1.05 | +1.08 | +1.48 | +1.49 | +1.50 |
| Progress end, after "serving" | +1.07 | +1.07 | +1.10 | +1.51 | +1.52 | +1.52 |

- **Load phases.** Reading the 12,364 shards (381 MiB on disk) takes 6.60–6.64 s on the quiet machine (9.22–9.26 s
  under load). Building the in-memory index (1.91 GiB) takes 8.31–8.43 s (11.51–11.66 s). clangd's CPU, from
  the driver's 0.5 s samples interpolated linearly at the phase edges: in the first 3 s, while it built the opened
  file's preamble and the standard-library index, it used 2.03 cores (p50; 2.00–2.09) on the quiet machine and
  2.20 (2.15–2.22) under load. From 3 s until the first persisted answer it used 1.01 cores (1.008–1.019), and
  1.04 (1.02–1.05) under load, so the load is serial. The three earlier `-j=8` runs under load had the same
  one-core profile (1.00–1.05 cores in that span). The persisted index serves all at once: no answer came from
  part of it.
- **The index is not rebuilt.** None of the 12,365 files in the index directory changed during either set, except
  `.gitignore`, which clangd rewrites on every start. The 5 `.asm`/`.rc` files have no shard, because indexing them
  fails with "Couldn't build compiler invocation". clangd logs "Failed to load shard" for them and retries them on
  every start. 1.05–1.08 s after the index serves on the quiet machine (1.48–1.50 s under load), it reports a
  re-index queue of "0/6", and the retries fail within about 30 ms, which ends the progress. From the first
  persisted answer to the progress end, clangd keeps about one core busy (0.87–1.02 cores on the quiet machine,
  0.90–1.02 under load) and logs nothing about the index; the answers are already served. No C/C++ file was
  re-indexed.

**Other runs** (not part of the result).

| Run | Conditions | B24 sample | Notes |
|---|---|---|---|
| Sanity run, 01:16, `-j=4`, verbose, before the lock | First load in about three hours, so shards were probably partly out of the file cache; system CPU 41.9–72.2 %, mean 57.6 % | 31.28 s | Shards read in 16.34 s (9.25 s warm), index built in 14.49 s; progress end 6.9 s after "serving" |
| Independent re-measurement, lock `verify-s7`, 01:47–01:50, `-j=4` | Same user workload as set 1; system CPU 39.2–41.5 % | 20.64 / 20.94 / 22.07 s (3 runs; warm-up 20.85 s) | Reproduces set 1 |
| Earlier agent, 2026-10-31 22:24–22:26, `-j=8` | Other agents building; system CPU 48–97 % | 22.12 / 25.62 / 35.59 s (3 runs) | Kept as an "under load" data point |

**Building the index** (2026-10-31, by the earlier S7 run, with `--log=info` and Instruction.cpp opened). The wall
and CPU times are upper bounds, because the machine was loaded; the memory figures are clangd's own and are not.
The machine figures come from the driver's samples and from `machine_load.csv` (every 31 s).

| | `-j=8` (attempt 1) | `-j=4` |
|---|---|---|
| Outcome | clangd exited after 237 s at 1020 of 5672 files ("LLVM ERROR: out of memory"), when free commit fell to 53 MiB | Complete: 5666 TUs indexed, 5 `.asm`/`.rc` failed |
| Wall time | — | 2539 s (42.3 min) |
| CPU time | 1662 s by exit (7.0 cores) | 9844 s (3.9 cores on average) |
| Private bytes 60 s in (MiB) | 4953 | 2654 |
| Peak working set / peak commit (MiB) | 6096 / 7034 | 6182 / 8261 |
| Private bytes when finished (MiB) | — | 7835, 60 s after progress end (3043 after 263 s, 4574 after 1312 s) |
| On disk | 5099 shards, 155 MiB (discarded) | 12,364 `.idx` shards, 381.4 MiB (399,932,566 bytes; largest 37 MiB) |
| Machine | G HUB held 43.3 GiB (44,339 MiB) throughout; system CPU mean 56 %. The WSL VM, absent until at least 127 s in, started 141–152 s in (when free commit fell by 2865 MiB) and grew to 3122 MiB by 21:35:09; free commit fell from 4623 MiB (21:33:35) to 1358 MiB (21:35:09) while clangd grew by 673 MiB | G HUB held 43.3 GiB for the first 14 min; free commit min 3727 MiB; system CPU mean 38 %; other agents were building: the WSL VM was present in 39 of 83 samples and above 1.5 GiB in 30 |

## Consequences
- **M3 (B24).** The M3 gate reproduces Decision 2 on R3 with the IDE's LSP client: the same R3 configuration and
  compile commands for the C/C++ files, the same index flags, a warm-up run, and a controlled REF-W1 run without the
  user's workload, scheduled with the user (PLAN.md T12). Its clock starts at clangd's launch, as in Decision 2, and
  that is what it compares with 18.4 s. The time from IDE open to clangd's launch is FrostIDE's own cost; the
  gate reports it next to the sample, and it is not part of the comparison. At about 0.20 s per 1 % of system CPU,
  some 15 points of extra load would use up the ×1.2 margin, so the gate records each run's system CPU as S7 did.
  The B24 harness can start from `s7_lsp.py` and `session_state.py` (to `tests/perf` in M3, with SPDX headers
  already in place).
- **M3 (L2 marker).** Stock clangd sends nothing when the persisted index starts serving. In these runs, the first
  background-index progress report after the load (the re-index queue, "0/6") arrived 1.05–1.08 s (quiet) or
  1.48–1.50 s (under load) after serving. The backgroundIndexProgress `end` comes after the re-index, so on a reopen
  with changed files it can be minutes late. M3 uses that first post-load report as the L2 ETW marker and documents
  the lag. Its behaviour on a reopen with changed files is untested, and M3 checks it. The benchmark keeps the S7
  probe method. Whether frost-clangd's extension points can report the exact swap is an M7 question.
- **M3 (UI before L2).** For about 15 s (quiet) to 21 s (under load) on R3, Navigate To and Search Everywhere get
  only the dynamic index's partial results. Go to Definition across files lands on the declaration. The UI marks
  these results as partial while L2 is pending and re-runs an open symbol query once L2 is reached.
- **M3 (start-up order).** In every run, the compilation database was loaded and the index load enqueued only
  after the first `didOpen`. Starting clangd without opening a file was not tested, so the supervisor sends
  `didOpen` for the restored active editor right after `initialize`. `-j` does not speed up the load, so the
  ResourceGovernor cannot shorten L2. It sets clangd's `-j` for the first index build and for re-indexing only.
- **M3 (compile database).** FrostIDE's command synthesis leaves non-C/C++ entries (`.asm`, `.rc`) out of what it
  hands to clangd. Otherwise clangd retries them on every start and logs an error for each. This does not change
  the index or the B24 sample (Decision 2). clangd keeps the index in `<compile-commands-dir>\.cache\clangd\index`.
  To keep the index "in FrostIDE's cache" (PLAN.md section 2), the synthesized `compile_commands.json` lives in
  FrostIDE's cache directory, and `--compile-commands-dir` points there.
- **M3 (B26 memory).** A warm clangd holds 3.16 GiB private after L2 on R3, which leaves less than 0.9 GiB of B26's
  4 GB idle budget for everything else. A first-ever index build at `-j=4` peaks at 8261 MiB of commit, above B26's
  8 GB peak by itself, and ends at 7835 MiB private. The reload measured here frees most of that: 3234 MiB after a
  fresh start. So the M3 ResourceGovernor restarts clangd once the first full index is built, and sizes `-j` by free
  commit (Decision 4). B26 itself is unchanged.
- **M3 (PCH).** With CMake's clang-cl PCH flags and the TU's `.pch` not built, stock clangd still parsed the
  opened TU without errors. M3's "PCH and forced-include awareness" item should verify this on the R2 corpora before
  synthesizing PCH flags.
- **M13 (B24 p50 2 s, p99 3 s).** That target is about 8× faster than stock clangd's serial load of R3 (15.3 s on a
  quiet machine: 6.6 s reading shards and 8.4 s building the index). No `-j` setting or warm-cache change closes the
  gap. Meeting it needs an index that serves without rebuilding it in memory at start-up, or one that serves while
  it loads. That is the T2 R&D the plan already schedules (M7 frost-clangd, M13). It may conflict with the "no
  patches" rule for frost-clangd, which is a decision for M7.

## Alternatives considered
- Taking the baseline from set 1, under the user's workload (p50 20.93 s, M3 value 25.1 s): rejected. The sample
  tracks the load, set 1 is 37 % slower than the control, and PLAN.md section 6.3 measures perf on a
  controlled machine. A gate value from loaded runs would be 6.7 s looser than the one from the control.
- Using the first non-empty `workspace/symbol` answer (1.7 s) as the baseline: rejected. It comes from the
  opened file's dynamic index and does not include anything from the persisted index, while B24 measures answers
  "from the persisted index".
- Using backgroundIndexProgress `end` as the end point: rejected. It comes 1.1–1.5 s after the index serves, and
  on a reopen with changed files it waits for the re-index.
- Taking the baseline from the earlier `-j=8` runs: rejected. Their flags differ from the index build's, other
  agents were building at the time, and the spread was wide (22.1–35.6 s). The load profile is the same at both
  `-j` values, so `-j=4` loses nothing.
- Measuring at `-j=8` or the governor default (cores/2 = 12): not allowed tonight (at most `-j=4` while other
  agents run). It would not change the load, which is serial.
- A monolithic `clangd-indexer` index loaded with `--index-file`: not measured. It is a candidate for the M7/M13
  R&D, not an M3 change.
