# FrostIDE progress log

Session hand-off log (CLAUDE.md, PLAN.md section 6.7). Newest entry first. Each entry lists what was done,
what is next, blockers and open questions for the user.

## 2026-11-01: M0 spike S7 (branch `m0/s7`), ADR 0008 after three verification rounds

### Done
- **S7 result** (ADR 0008). Stock clangd 23.1.4 serves R3's persisted index (12,364 shards, 381 MiB) only once all
  of it is loaded, on one core. Both B24 answers came at p50 15.31 s on a quiet machine (20 runs, lock `s7-2`) and
  at p50 20.93 s under the user's own workload (20 runs, lock `s7`).
- **Proposed, pending the user's confirmation:** B24's M3 value of p50 18.4 s (15.31 s ×1.2) and a quiet machine
  for the M3 gate. The value is in budgets.json's M3 slot, and the slot's note says it is proposed; PLAN.md's S7
  sentence says the same. PLAN.md's B24 row, the M13 targets and the B24 preconditions are unchanged.
- **Other findings.** A first-ever index build at `-j=4` peaks at 8261 MiB of commit, above B26's 8 GB peak on its
  own. M13's p50 2 s is about 8× faster than stock clangd's load (the planned M7/M13 R&D). M3 uses the first
  background-index progress report after the load as the L2 marker.
- **Fix round 3**, after the second re-verification:
  - the two points above are marked as proposed in the ADR's status and decisions, with the questions;
  - the Mann-Whitney p: exact two-sided 2/C(40,20) = 1.45 × 10⁻¹¹; 6.8 × 10⁻⁸ is the normal approximation;
  - the driver's re-send delay is 0.100–0.101 s while both requests cycle (0.15 s was a filter threshold);
  - the pooled r = 0.998 is between the two clusters; set 2's slope (0.09 s per 1 %) is less than half of set 1's;
  - that FrostEditor had exited before set 2 now rests on `conditions.txt` (02:00:58), not on a top-12 list;
  - the WSL VM's start in the `-j=8` attempt is bounded to 127–158 s; leaving out the `.asm`/`.rc` entries is
    marked untested;
  - budgets.json's note says the DXGI monitor-power check was not done and names the 21 generated headers.
- **Close-out**, after the verification of 51cfceb:
  - all cited raw data and the scripts we wrote now live in `E:\FrostBuilds\perf\m0\s7\` (32 MB; its `README.txt`
    maps each directory to the ADR section it backs), and the ADR cites it instead of the session scratchpad. The
    Instruction.cpp text that the verbose clangd logs echo was replaced by a placeholder in the copies;
  - the ADR says that no run called `SetThreadExecutionState(ES_DISPLAY_REQUIRED)` (PLAN 6.3), that the power plan
    kept the machine from sleeping and the System event log shows no sleep or resume, and that the display does not
    enter the timings; Decision 2's summary of PLAN 6.3 and T12 is complete;
  - the symbol answers' spacing at the answer is 0.101–0.104 s over all 40 runs (0.102–0.104 s was set 1's alone).

### Next
- Before `m0/s7` merges: `tools\verify.ps1 -Quick` and `tools/verify.sh` (CLAUDE.md's definition of done), and
  `-Full` on both for the merge (PLAN.md section 6.7). Neither has run on this branch; its changes are the ADR,
  PLAN's S7 sentence, budget data and this log.
- Once the user has answered: record the answer in ADR 0008's status, budgets.json's note and PLAN's S7 sentence,
  or set the M3 slot to the value the user chooses.
- M3: the B24 harness, starting from `s7_lsp.py` (in `E:\FrostBuilds\perf\m0\s7\spike\`) and `session_state.py`
  (in its `fix2\` sub-directory).

### Blockers
- None for M0. B24's M3 value waits for the user's confirmation.

### Questions for the user
1. B24's M3 value: confirm p50 18.4 s from the quiet control, or choose otherwise, for example 25.1 s from the runs
   under your workload, or a re-measurement that also makes the DXGI monitor-power check S7 did not make.
2. Should the M3 gate require a quiet REF-W1, with your heavy workload closed and the run scheduled with you? Under
   set 1's workload every stock clangd run took longer than 18.4 s (19.77–22.45 s).
3. Should `tests/corpus/manifest.json` pin R3's configuration (Ninja, `LLVM_ENABLE_PROJECTS=clang`, Release, the
   clang-cl toolchain) for all R3 budgets?

## 2026-11-01: m0/tracy, ADR 0009 after three verifications

### Done
- ADR 0009 chooses Tracy 0.14.1 (the 2026.10.16 baseline's port, 0.14.1#1). The first verification's findings
  are handled: the threat model (a connected local process can read and corrupt the client's memory), the
  shutdown order for manual lifetime, the warning counts, the pinning plan (an override in M1), and the
  per-zone cost of the prescribed task zones.
- The second verification's findings are handled:
  - Tracy's symbol worker: with the port defaults it calls DbgHelp unlocked beside FrostIDE's own DbgHelp
    users, rewrites `_NT_SYMBOL_PATH` and preloads symbols for every module. Item 5 now adds
    `TRACY_NO_CALLSTACK` and `TRACY_NO_SYSTEM_TRACING`. Measured on both OSes under the `tracy-2` measurement
    lock: Tracy runs no symbol worker, neither loads DbgHelp nor changes `_NT_SYMBOL_PATH` (FrostIDE's own
    DbgHelp users and a developer's `_NT_SYMBOL_PATH` are unaffected), and its `ShutdownProfiler` no longer
    depends on the loaded modules (Windows p50 37.5–62.5 ms, against 151.1 ms with Qt loaded before). Zone
    costs are unchanged, and Tracy's own TU has 0 warnings on clang-cl.
  - The shutdown figures now give load, loaded modules and time since start, and B20's bullet says they are
    load-dependent.
  - The dev-only manifest feature is required, with its own install tree; the release SBOM names only the
    release preset's tree.
  - The override's precondition (the port tree must be in the vcpkg clone) is stated.
  - The process counts in the conditions are corrected.
- The third verification confirmed the ADR. Its four low findings are fixed:
  - one cell rounded twice (connected, 4 threads, **nc**: 55.8 ns, raw median 55.846);
  - the DbgHelp claim now says what the probe could show, which is that Tracy itself no longer loads
    `dbghelp.dll` or changes `_NT_SYMBOL_PATH`;
  - the first WSL hold of the lock (lock 1) is compared cell by cell with lock 2, including its outliers;
  - the cost of giving up call stacks and system tracing is stated, in the ADR and in question 1 below.
- The raw data the ADR relies on now lives in `E:\FrostBuilds\perf\m0\tracy\` (254 MiB): the logs, scripts,
  probe sources and captures of the three measurement passes, the third fix round's recompute script, and the
  three verifiers' re-runs and recompute scripts. Its `README.txt` maps each directory to the ADR section it
  backs, and the ADR's Method names it. Tracy's sources, the vcpkg and consumer build trees and the binaries
  were not kept.
- PLAN.md is unchanged on this branch. The previous round's edit of the Dependencies line was reverted: the
  wording is a question for the user, not a change made without approval.

### Next
- M1 work list in ADR 0009, only once the user grants the listener exception.

### Blockers
- Any use of Tracy (the whole M1 work list in ADR 0009) is blocked on the listener exception (question 1).
  Nothing in M0 is blocked: the branch adds only the ADR and this entry.

### Questions for the user
1. ADR 0009: grant or decline the listener exception for Tracy (an unauthenticated 127.0.0.1 TCP listener in
   dev builds, started only by an explicit per-run opt-in).
   - While Tracy runs, any local process (and, for a WSL instance, any Windows-side process) can connect. It
     can then read FrostIDE's memory: open buffers, the IDE-bridge token, askpass passphrases, the
     environment. It can also corrupt that memory through a heap overflow, so treat it as possible code
     execution inside FrostIDE (ADR 0009, threat model).
   - Granting the exception also means accepting item 5's options as they stand, including
     `TRACY_NO_CALLSTACK` and `TRACY_NO_SYSTEM_TRACING`: Tracy then captures no call stacks, no sampled call
     stacks and no context switches. FrostIDE's zones never use call stacks, and on Windows sampling and
     context switches need elevation, which FrostIDE never has, so the real loss is sampling and context
     switches on Linux. Keeping them was not evaluated further (ADR 0009, Alternatives, "Keeping call
     stacks").
2. Confirm the new rule that budget and perf runs (`frost-bench`, `verify -Perf`) never start Tracy.
3. Optional: PLAN.md 2.1 lists "tracy (pinned in M0)". Should it read "tracy (version chosen in M0: 0.14.1 from
   the vcpkg baseline; use pending the listener exception in ADR 0009)"? Not applied.

## 2026-11-01: M0 spike S1 result (branch `m0/s1`)

### Done
- **Run 3, the main suite.** The coordinator ran `tests\perf\s1\run-s1.ps1` at bc20789 with the user's go,
  2026-11-01 01:02-01:11: 47 steps, 8:29, every run valid. All 6 probe validations passed: the GDI window at the
  start and end, delays through the raster and QRhi paths, and the UI-CPU injection on both backends. The
  desktop was the user's busy one (Wallpaper Engine, Discord, 33-44 % system CPU), not a quiet one.
- **ADR 0007 fix round** after an independent verification of b2395cd. It corrects:
  - the qpcVBlank claim (irregular, used only as a grid anchor);
  - the earlier-runs section (it gave run 2's numbers for both runs and claimed an agreement run 1 does not
    show);
  - the floor claims;
  - the B1 endpoint (the detector fires on about the third composed frame of the fade-in, 12-19 ms late);
  - the cold proxy (bias of unknown direction);
  - the B2/B15 cost of starting QRhi later.

  It also discloses merged captures, and all cited raw data now lives in `E:\FrostBuilds\perf\m0\s1\`
  (run 4's console log is `run4-floors\s1-run4.log`).
- **Run 4, a floors follow-up** (01:43-01:48, 4:24 on screen, under the measurement lock).
  - Changes: `s1_editor --full-output` (window over the whole output; `windowRect` with a unit test, 17 editor
    tests) and `run-s1.ps1 -Suite floors`, which interleaves trivial 320x48 windows of both backends with QRhi
    runs and adds one round at 5120x1440.
  - Findings: trivial windows missed more than the QRhi editor in 19 of 20 per-round pairs (medians 7.8 %
    raster and 9.6 % QRhi, against 5.5 % for QRhi with text), so they are not a floor, and they do not track
    the editor round by round, so they are context only. Covering the whole output cut QRhi's misses to
    0.56-1.25 %, with every interval at 13.9 ms (one round). A trivial QRhi window also switches its
    vblank-to-screen level, so the switching is specific to QRhi's present path.
- Result, unchanged in direction: raster fails B6 CPU at 5120x1392 in every round (p99 medians 9.18 / 7.38 /
  6.29 ms). QRhi passes CPU in 10 of 10 rounds (p99 0.86 ms); its missed and interval criteria are open. B1
  warm fails before any editor work (frost.exe p50 264.9 ms). QRhi adds 216.9 ms to the first frame.

### Next
- The user's answers to ADR 0007's four questions. Then the independent review and merge of `m0/s1`.
- M1: keep QRhi start-up off B1 without moving it onto B2 or B15 (options in ADR 0007, Consequences); profile
  the first `show()`; check QRhi's swap-chain frame latency.
- M1: a quiet-desktop control and PresentMon's present mode for QRhi, to explain its misses at the work area.
- When m0/io lands, port `tests/perf/s1/child_process.cpp` (CreateProcessW today) to `IProcessLauncher`.

### Blockers
- None. DWM ETW needs elevation, so it is deferred (ADR 0007).

### Questions for the user
1. Approve QRhi as M1's editor renderer on Windows (the default, with raster as the oracle and RDP/WARP
   fallback)?
2. How should B6's missed and interval criteria be gated? The options are: a quiet desktop with the absolute
   budget; the whole output (5120x1440) with the absolute budget; or deciding at M1 after a quiet-desktop
   control and PresentMon. The ADR proposes the last, with no budget change.
3. B1 warm fails before any editor work (frost.exe p50 264.9 ms against 150 ms; max of 20, 360.1 ms, against
   250 ms). Keep 150/250 ms as M1's target, or re-baseline?
4. A true cold B1 run needs a reboot (first-frame steps first after logon) or an elevated RAMMap -Et before each
   launch. Which, and when?

## 2026-10-31: M0 spike S1 (branch `m0/s1`)

### Done
- **Probes.** `tests/perf/probes` holds `frost::perf_probes`, in the diag layer, Windows parts only.
  - Desktop Duplication with a frame marker and pixel samples.
  - DWM timing and a vblank clock.
  - The `ISessionState` precheck.
  - Frame analysis, delay-validation logic and a JSON report.

  It has 67 headless unit tests. On a GDI window it was validated by injected 8 and 25 ms delays (errors +0.12
  and -0.57 ms) and by stalls and cadence.
- **Spike tools.** `tests/perf/s1` holds:
  - `s1_probe_validate`;
  - `s1_editor`, in the app layer: the raster QGlyphRun view and a QRhiWidget D3D11 glyph atlas, with 16
    headless tests;
  - `s1_first_frame`;
  - `run-s1.ps1`.

  They are Windows only, built only with `WIN32 AND FROST_BUILD_UI`, and never run by CTest.
- **ADR 0007** (run 2, under load, n=1 per configuration). At 5120x1392 the raster backend fails B6 CPU: p99
  9.4 ms with full repaint, 7.9 ms with scroll-blit and 6.7 ms with fill only. QRhi passes B6 CPU (p99 1.0 ms) but
  fails the interval (20.8 ms with text) and missed (6.7 %) criteria. S1 is not yet shown to pass for QRhi, and
  QRhi's B1 is unmeasured. B5 passes on both. frost.exe B1 warm is p50 246 ms and p99 344 ms (= max of 8), which
  fails; the cold proxy is p50 331 ms.
- **Review round 1** (code and method):
  - Verdicts need samples: a gate without enough samples is null ("not measured"). Runs with too few decoded
    markers or a capture failure are invalid (exit 3) in `s1_editor` and `s1_first_frame`.
  - `s1_probe_validate` adds phase C, a stall-free environment floor.
  - New probe validations for both Qt backends: `s1_editor --validate-delays` (the delay spun inside the paint or
    `QRhiWidget::render`, so the flip-model swap chain is covered) and `--inject-cpu-ms` (a known GUI-thread spin
    the UI-CPU probe must see within 10 %).
  - `s1_editor --first-frame` gives each backend's own B1 through `s1_first_frame --args`.
  - The child launcher runs in the executable's directory, in a kill-on-close job.
  - DXGI enumeration stops on any failure; the display-state wait no longer outlives its context; the RHI view
    keeps the marker quads when glyphs overflow; the cycle-meter test is robust under load.
  - `run-s1.ps1` is now one suite: the validations, five interleaved rounds of all seven configurations, B1 for
    frost.exe and both backends (20 warm launches each for frost.exe and QRhi), and a min/median/max summary.
    `-DryRun` prints the plan (47 steps, estimated 9:35 on screen).
  - The ADR text is qualified as above; the Debug validation report and a DWM counter dump are kept with the raw
    data (`s1-run1`).

### Next
- The quiet-window run, arranged with the user (nothing else on screen, no builds):
  `powershell -ExecutionPolicy Bypass -File tests\perf\s1\run-s1.ps1 -BuildDir build\s1 -OutDir <dir>`, after a
  Release build of `s1_probe_validate s1_editor s1_first_frame frost`. Its summary replaces ADR 0007's tables.
- Then the user decision on ADR 0007 (QRhi editor backend in M1), independent review and merge of `m0/s1`.
- When m0/io lands, port `tests/perf/s1/child_process.cpp` to `IProcessLauncher`.

### Blockers
- The quiet-window run needs the user to free the screen for about 10 minutes.
- DWM ETW needs elevation, so it is deferred (ADR 0007).

### Questions for the user
- When can the quiet-window run take the screen (about 10 minutes)?
- Approve the QRhiWidget backend as M1's default editor renderer (ADR 0007, decision 1) once that run confirms it?
- In two runs under load, no backend reached B6's "missed ≤1 %" while video, Wallpaper Engine and GPU load ran.
  Should gate runs happen on a quiet desktop, or be reported against a same-session environmental floor? A budget
  change needs your approval.

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
