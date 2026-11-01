# FrostIDE progress log

Session hand-off log (CLAUDE.md, PLAN.md section 6.7). Newest entry first. Each entry lists what was done,
what is next, blockers and open questions for the user.

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
