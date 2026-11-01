# ADR 0010: Budget B28, the dev loop on R1

- Status: accepted. **Decided 2026-11-01 (ADR 0011, F3)**, on the user's instruction of that day to follow the
  recommendation: decision 2 is accepted, so the full default build gates B28 from M1 on, and `budgets.json`'s
  B28 method fixes the probe files and the job counts. Until then the status read:
  accepted for M0. B28 is met on both of its variants, under each reading of "rebuilt and linked"
  measured here. Which reading gates B28 from M1 on is proposed (decision 2) and waits for the user (question 1).
  No budget, test or warning changes.
- Date: 2026-11-01
- Plan reference: PLAN.md section 4 (B28), section 6.4 (`verify -Quick`), M0 exit ("B28 met");
  `tests/perf/budgets.json` (B28); ADR 0001 (the first, unrepresentative B28 sample)

## Context
PLAN.md's B28 row reads "Dev loop: one-.cpp change to rebuilt and linked / `verify -Quick`", target "p50 20 s /
p50 5 min", corpus R1 (FrostIDE itself), gate M0. `budgets.json` splits it into two variants:
- "one-.cpp rebuild", p50 at most 20 s;
- "verify -Quick", p50 at most 5 min.

Its precondition is "FrostIDE itself (R1) on REF-W1; the Debug build uses sccache (PLAN.md section 6.4)", and its
method is "Wall time from a one-.cpp change to the rebuilt, linked target; wall time of tools/verify.ps1 -Quick."
ADR 0001 has only a first sample, taken while three agents were building, and defers the measurement to a quiet
machine before the M0 exit. sccache came in on 2026-11-01 through `cmake/FrostCompilerCache.cmake`, on Windows
only: Linux has no sccache pin, so the WSL leg compiles without a cache.

Neither text says which target counts as "rebuilt and linked". PLAN.md has "rebuilt and linked", and
`budgets.json` has the singular "the rebuilt, linked target". At M0, R1's default Debug build links `frost`
(from `frost_app` and `frost_core`) and the test and spike executables. 14 of those link `frost_core` as well.
`frost` does not link `frost_sched`, so a change in `src/sched` relinks no `frost` at all.

## Decision
1. **B28 is met at M0.**
   - One-.cpp rebuild: the worst p50 is 3.33 s (`src/app/src/main.cpp`), and the slowest of all 50 counted
     runs took 3.53 s, against p50 20 s.
   - `verify -Quick`: p50 82.3 s (1.37 min), max 85.5 s, against p50 5 min.
2. **Proposed, for the user to decide (question 1): B28's "rebuilt and linked" is the full default build.**
   That is `cmake --build build\win-clangcl --config Debug` after the change, which rebuilds and relinks every
   executable the change reaches.
   - For a given change it does all the work of any single target's build and more, so it is the strictest
     reading. Here the `--target frost` build had the same p50 for main (3.33 s each; 3.332 against 3.326 s
     raw) and was 1.5 s faster for core.
   - It is what a plain `cmake --build` and `verify -Quick`'s build step do.
   - It also covers library files that `frost` does not link, such as `src/sched`.

   `--target frost` is measured and reported alongside. It is the reading that `budgets.json`'s singular "the
   rebuilt, linked target" suggests. At M0 both readings meet the target with a wide margin, so nothing waits on
   the answer.

   **Decided 2026-11-01 (ADR 0011, F3):** accepted.
3. **Probe files.** Each one-.cpp change goes to one of three files:
   - `src/core/src/error.cpp` ("core"): `frost` and 14 other executables link it, so it has the widest fan-out
     (15 executables relink);
   - `src/app/src/main.cpp` ("main"): `frost`'s own translation unit and the slowest of the three compiles
     (2.9–3.05 s; it includes `<QApplication>` and `<windows.h>`);
   - `src/sched/src/lane.cpp` ("sched"): only the three sched test executables link it.

   The core file stands in for "a library .cpp that many targets link" in the `--target frost` variant, because
   `frost` does not link sched.
4. **The harness stays outside the repository**, as the spike scripts do. It lives with the raw data in
   `E:\FrostBuilds\perf\m0\b28\scripts\` until M1's perf tooling (`tools/perf.ps1`, PLAN.md section 4) takes
   B28 over.

## Measurements
### Method
- **Probe.** Each timed run inserts `[[maybe_unused]] constexpr int kB28Probe = N;` before the probed file's first
  namespace, with an N never used before (milliseconds since the epoch, mod 2e9). This changes the preprocessed
  output, so sccache cannot serve the compile from its cache. A comment-only change or a touch would be served
  from the cache and would time a cache hit, not a compile. The probe passes clang-format and frost-lint.
- **Starting state.** Every timed run starts from an up-to-date tree: an untimed full build ("settle") runs
  first.
- **Timer.** The timer starts right after the file is written and stops when the `cmake --build` process exits.
  So it includes `cmake --build`'s start, ninja's dependency scan, every edge, and the POST_BUILD steps.
- **Parallelism.** The rebuilds run with ninja's default parallelism (no `-j`: 26 jobs on 24 threads).
- **Rounds.** A round is the five combinations in a fixed order: core with `--target frost`, core with the full
  build, main with each, then sched with the full build. One warm-up round ran, then 10 counted rounds.
- **p50.** p50 is the median, so for 10 runs it is the mean of the 5th and 6th.
- **What else is recorded.** Per run, the harness records the sccache counter deltas (`sccache --show-stats
  --stats-format json`) and the per-edge times from the lines `.ninja_log` gained.
- **`verify -Quick`.** 6 runs of `tools\verify.ps1 -Quick -Jobs 4`, alternating core and main, each after a settle
  and a probe change. `-Jobs 4` is the value the overnight workflow uses; the script's default is 8.
  - verify.ps1 echoes a native command's output only after that command ends. The harness therefore timestamps
    the script's own `===` markers as they arrive and uses them as the phase boundaries.
- **WSL leg split.** Three more runs of the WSL leg alone, with `verify.sh` started exactly as verify.ps1 starts
  it. Each run waits until the distro has stopped by itself, times `wsl.exe -d Ubuntu-24.04 --exec true` (the
  distro start), and timestamps `verify.sh`'s lines as they arrive. One CMake profile of the Linux configure
  (`--profiling-format=google-trace`) followed.
- **Restore.** After each series the harness put the probed files' original bytes back, and `git status` was clean
  every time.

### Conditions
- **Machine.** REF-W1: AMD Ryzen 9 5900X (12 cores, 24 threads), 32 GB, Windows 11 10.0.26300, High performance
  power plan. Sources and build tree are on E: (NTFS, so not a Dev Drive). Defender's state could not be read
  (`Get-MpComputerStatus`: "Provider load failure").
- **Windows toolchain.** clang-cl 23.1.4 and lld-link, CMake 4.4.5, Ninja 1.13.2 (Ninja Multi-Config). sccache
  0.19.0 runs with a local cache in `E:\sdk\sccache-cache`, with preprocessor cache mode on. Configure printed
  `FrostIDE compiler cache: sccache 0.19.0 (E:/sdk/sccache-0.19.0/sccache.exe, SCCACHE_DIR E:\sdk\sccache-cache)`.
- **Linux toolchain.** WSL 2, Ubuntu 24.04, clang 23.1.4 with libstdc++ 14, 6 GB VM cap. Configure printed
  `FrostIDE compiler cache: off (no sccache pinned for linux in sdk/sdk.lock.json)`.
- **Tree.** `m0/foundation` at 5978075, main worktree. A warm-up `verify.ps1 -Quick -Jobs 4` brought the Windows
  tree and the WSL mirror up to date (81.5 s, green) after `night.py build-gate`, before the lock. It is in no
  table.
- **Lock.** Every timed run, the WSL split and the configure profile ran under the overnight measurement lock
  `b28`, from 04:22:28 to 04:41:52. The user was away and no other agent was building. The lock record:

  ```
  {"measurement": "b28", "start": "2026-11-01T04:22:29", "free_commit_gb": 36.6, "commit_limit_gb": 64.1, "free_phys_gb": 15.4, "system_cpu_pct": 0.6, "our_builds_running": [], "builds_wait_timed_out": false, "top_cpu_processes": [{"Name": "Discord", "Id": 24484, "ws_mb": 752}, {"Name": "Discord", "Id": 2364, "ws_mb": 89}, {"Name": "wallpaper64", "Id": 24044, "ws_mb": 158}, {"Name": "WindowsTerminal", "Id": 10768, "ws_mb": 150}, {"Name": "claude", "Id": 8908, "ws_mb": 489}, {"Name": "Discord", "Id": 14552, "ws_mb": 106}, {"Name": "FanControl", "Id": 22180, "ws_mb": 62}, {"Name": "RazerAppEngine", "Id": 24352, "ws_mb": 354}]}
  ```

  `top_cpu_processes` is ordered by each process's total CPU time since it started, so it names long-running
  background apps, not current load. The current load is `system_cpu_pct`, sampled over 3 s.
- **Session.** Claude started the runs from the logged-in user session. PLAN.md section 6.3's session-state
  precheck was not run, because B28 times builds and tests and nothing on screen.
- **WSL distro state.** At every WSL leg of the `verify -Quick` series the distro was stopped
  (`wsl.exe --list --running`, checked as verify.ps1 reached its last check). The distro stops by itself about
  16 s after its last process exits, and the Windows part of a run takes about 39 s. So every WSL leg includes a
  distro start, as it does in normal use. The first run started with the distro already stopped for longer, and
  its WSL leg was the slowest (46.0 s against 42.4–43.3 s). Whether the VM itself also had to start then was not
  checked.

### No-op floor (5 runs each, 04:22:54)
| Command, no change | Edges | p50 | max |
|---|---|---|---|
| `--target frost` | 1 | 0.136 s | 0.288 s |
| full build | 9 | 0.322 s | 0.330 s |

The edges are the runtime-DLL copy targets: one per output directory, and they always run (`FrostLayers.cmake`).

### One-.cpp rebuild (10 counted runs per row, 04:22:56–04:25:43)
| Probe | Command | Edges | p50 | max | min | Warm-up run |
|---|---|---|---|---|---|---|
| core (`error.cpp`) | `--target frost` | 4 | **1.58 s** | 1.60 s | 1.44 s | 1.58 s |
| core | full build | 26 | **3.09 s** | 3.53 s | 2.32 s | 2.48 s |
| main (`main.cpp`) | `--target frost` | 3 | **3.33 s** | 3.36 s | 3.18 s | 3.32 s |
| main | full build | 11 | **3.33 s** | 3.44 s | 3.27 s | 3.37 s |
| sched (`lane.cpp`) | full build | 14 | **1.16 s** | 1.17 s | 1.02 s | 1.01 s |
| sched | `--target frost` | not timed: `frost` does not link `frost_sched`, so nothing of the change is in it | | | | |

- **sccache.** All 55 timed runs, the warm-up round included, exited 0. Each made exactly one sccache compile
  request, and every one was a miss (0 hits). In the dry test before the lock, the build after restoring the
  three files' original bytes got its 3 compiles from the cache (3 hits).
- **Where the time goes** (`.ninja_log`, counted runs):
  - core: the compile takes 1.22–1.39 s and `frost_core.lib` 0.02 s. Alone, `frost.exe` links in 0.14–0.30 s.
    In the full build it links alongside 14 other executables and takes 0.61–1.95 s, and the other links take
    0.13–1.95 s. That contention is why core's full build rose from 2.3 s to 3.5 s over the rounds; why it grew
    beyond contention was not isolated.
  - main: the compile takes 2.90–3.05 s and the link 0.15–0.31 s. The full build adds only the DLL copies.
  - sched: the compile takes 0.63–0.79 s, and the three sched test executables link in 0.12–0.29 s.
  - `cmake --build` and ninja's own start take 0.02–0.17 s per run.

### verify -Quick (6 runs, 04:25:57–04:34:12)
Every run printed `Verified: win debug 367/368 (1 skipped), wsl debug 334/334, wsl licensecheck ok, format ok,
frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run`.

| Run | Probe | Wall | Windows configure + build + tests | Checks | WSL leg |
|---|---|---|---|---|---|
| v00 | core | 85.5 s | 36.3 s | 2.9 s | 46.0 s |
| v01 | main | 82.2 s | 35.7 s | 3.0 s | 43.3 s |
| v02 | core | 80.4 s | 34.8 s | 3.0 s | 42.4 s |
| v03 | main | 82.3 s | 35.9 s | 3.0 s | 43.1 s |
| v04 | core | 81.2 s | 35.1 s | 3.0 s | 42.9 s |
| v05 | main | 82.6 s | 36.2 s | 3.0 s | 43.1 s |
| **p50 / max** | | **82.3 s / 85.5 s** | 35.8 s / 36.3 s | 3.0 s / 3.0 s | 43.1 s / 46.0 s |

The PowerShell start and `env.ps1` take another 0.28 s before the first marker.

| Phase (p50 / max over 6 runs) | Time |
|---|---|
| Windows configure (`cmake --preset win-clangcl`) | 1.77 / 1.80 s |
| Windows build (`-j 4`): core 2.01–2.33 s, main 3.27–3.43 s | 2.80 / 3.43 s |
| Windows tests (`ctest -LE stress`, 368 tests) | 31.02 / 32.15 s |
| clang-format check | 2.06 / 2.10 s |
| frost-lint | 0.53 / 0.68 s |
| licensecheck | 0.29 / 0.30 s |
| featurecheck | 0.08 / 0.08 s |
| WSL leg (`verify.sh --no-checks`) | 43.12 / 45.99 s |

- **Shares at p50.** Windows 44 %, checks 4 %, WSL 52 %.
- **The change itself** costs 2.0–3.4 s on Windows (the build step), and 1.4 s for a core change on Linux (WSL
  leg split, below). The rest is fixed cost.
- **Windows tests run serially.** verify.ps1 passes no `-j` to ctest, and in v02 the 368 per-test times add up
  to 30.55 s of the 30.85 s ctest total. The 350 tests under 0.2 s take 7.41 s together. The longest two are
  `FrostRuntimeDlls.CopiesOncePerDirectory` (6.68 s) and `tools.corpus_fetch` (5.19 s).
- **The other sccache requests.** Each verify run made 4 sccache requests: the probe's miss, plus three counted as
  2 compile failures and 1 cache error. A rebuild run, which runs no tests, made exactly one, so those three come
  from outside the build step. The likely source is the three `ToolchainProbe.Rejects.*` tests (embed,
  contracts, reflection): they run `cmake --build` on compile-fail targets in the same tree, so their compiles go
  through sccache too. That was not traced request by request.

### WSL leg split (3 runs, 04:37–04:40)
| Phase | w00 | w01 | w02 |
|---|---|---|---|
| Distro start (`wsl.exe --exec true`, distro stopped) | 4.32 s | 2.81 s | 2.60 s |
| `env.sh` and rsync to ext4 | 0.67 s | 0.65 s | 0.66 s |
| Configure (`cmake --preset linux-clang`) | 23.27 s | 23.20 s | 23.20 s |
| Build (`-j 4`) | 3.16 s (two files: main.cpp had just been restored) | 1.41 s | 1.40 s |
| Tests (`ctest`, 334 tests) | 14.32 s | 14.27 s | 14.08 s |
| licensecheck | 0.20 s | 0.20 s | 0.20 s |
| **Distro start + leg** | 45.9 s | 42.5 s | 42.1 s |

That matches the verify series' WSL legs (42.4–46.0 s).

**The Linux configure's 23 s**, from one profile (23.9 s wall):
- `find_package(Qt6)` takes 17.1 s. Most of it is two FindOpenGL `find_path` calls for headers that are not
  installed: `GL/glu.h` (7.40 s) and `GL/xmesa.h` (7.17 s). `find_library` for GLU adds 0.78 s and 0.72 s.
- The cache holds `OPENGL_GLU_INCLUDE_DIR-NOTFOUND` and `OPENGL_xmesa_INCLUDE_DIR-NOTFOUND`, and CMake repeats a
  search on every configure while its result is NOTFOUND.
- `project()` takes 6.52 s, nearly all of it in vcpkg's manifest install (`execute_process`, 6.50 s).
- Inside WSL, 38 of PATH's 52 entries are Windows directories under `/mnt`, because WSL appends the Windows PATH.
  `find_path` also searches the PATH directories and prefixes derived from them. Whether those 9P lookups are
  what makes each search take about 7 s was not tested.
- The whole Windows configure takes 1.6–1.8 s.

### Raw data
`E:\FrostBuilds\perf\m0\b28\` (11 MB). Its `README.txt` maps each directory to the sections above:
- `lock-begin.json`, `lock-end.txt` and `conditions.txt`: the conditions;
- `noop\`, `rebuild\`, `verify\` and `wslsplit\`: one log per run, `runs.jsonl` and `summary.json`;
- `wsl-configure-profile\`: the configure trace;
- `warmup\`: the warm-up run;
- `scripts\`: `b28_bench.py` (the harness), `trace_top.py`, a copy of the lock helper `night.py`, and HEAD's
  bytes of the probed files.

## Consequences
- The M0 exit item "B28 met" is satisfied.
- **Margins.** The one-.cpp rebuild is about 6 times under 20 s (worst p50 3.33 s), and `verify -Quick` about 3.6
  times under 5 min. Most of `verify -Quick` is fixed cost, not the change: 31 s of Windows tests, 23 s of Linux
  configure and 14 s of Linux tests.
- **sccache.** A new change always misses, so sccache cannot shorten this loop. Each timed compile includes
  sccache's miss path (preprocess, hash, compile, store). No run without sccache was made, so that overhead is not
  separated out. Its benefit (the restored original bytes were served from the cache) shows after branch
  switches and rebuilds of known content, which B28 does not time.
- **Follow-ups for M1**, none done here and none weakening a test or budget:
  - **Linux configure.** About 15 s of each WSL configure goes to the two failing OpenGL header searches. First
    test whether the `/mnt` PATH entries are the cause. Then pick a remedy, for example:
    - give CMake a PATH without them in `verify.sh`;
    - install GLU's development headers in `bootstrap-wsl.sh`'s apt phase (`GL/xmesa.h` would still be
      missing);
    - skip the re-configure when nothing CMake reads has changed.

    vcpkg's 6.5 s manifest install per Linux configure was not investigated.
  - **Windows tests.** They run serially for 31 s. Parallel ctest could cut most of that, once the tests that
    start processes, copy DLLs or run CMake are shown to be safe in parallel.
  - **Re-measure B28 as R1 grows.** M1 adds the editor and the shell, with more translation units and links.
    `tools/perf.ps1` should take over this harness.

## Alternatives considered
- **A comment-only change or a touch as the probe:** sccache, or ninja with unchanged content, would time a cache
  hit, not a compile.
- **`--target frost` as the gating reading:** measured and reported. It is not proposed, because it leaves out the
  executables a library change also relinks, and it times nothing at all for a sched change. The user decides
  (question 1).
- **Timing the phases with timestamps added to verify.ps1:** that would change the script under measurement. The
  harness timestamps the script's markers from outside instead.
- **Probing a sched file for the `--target frost` variant:** `frost` does not link sched, so there would be
  nothing to time.

## Questions for the user
1. Which reading of "rebuilt and linked" should gate B28 from M1 on?
   - (a) The full default build after the change. This is proposed in decision 2: core p50 3.09 s, main 3.33 s,
     sched 1.16 s.
   - (b) `--target frost`: core p50 1.58 s, main 3.33 s.
   - (c) The executable or executables that cover the change, such as the changed module's test target.

   Should `budgets.json`'s B28 method then also fix the probe files (here core, main.cpp and sched) and the job
   counts (here ninja's default for the rebuild, and `-Jobs 4` for `verify -Quick`)? All readings meet the
   target at M0, so nothing waits on the answer.

   **Decided 2026-11-01 (ADR 0011, F3):** (a), and yes: `budgets.json`'s B28 method fixes the three probe files,
   ninja's default parallelism for the rebuild and `-Jobs 4` for `verify -Quick`.
