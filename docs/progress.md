# FrostIDE progress log

Session hand-off log (CLAUDE.md, PLAN.md section 6.7). Newest entry first. Each entry lists what was done,
what is next, blockers and open questions for the user.

## 2026-10-31: M0 spike S1 (branch `m0/s1`)

### Done
- **Probes.** `tests/perf/probes` holds `frost::perf_probes`, in the diag layer, Windows parts only.
  - Desktop Duplication with a frame marker and pixel samples.
  - DWM timing and a vblank clock.
  - The `ISessionState` precheck.
  - Frame analysis, delay-validation logic and a JSON report.

  It has 67 headless unit tests. It was validated by injected 8 and 25 ms delays (errors +0.12 and -0.57 ms) and
  by stalls and cadence.
- **Spike tools.** `tests/perf/s1` holds:
  - `s1_probe_validate`;
  - `s1_editor`, in the app layer: the raster QGlyphRun view and a QRhiWidget D3D11 glyph atlas, with 10
    headless tests;
  - `s1_first_frame`;
  - `run-s1.ps1`.

  They are Windows only, built only with `WIN32 AND FROST_BUILD_UI`, and never run by CTest.
- **ADR 0007.** At 5120x1392 the raster backend fails B6 CPU: p99 9.4 ms with full repaint, 7.9 ms with
  scroll-blit and 6.7 ms with fill only. QRhi uses p99 1.0 ms. B5 passes on both.
  frost.exe B1 warm is p50 246 ms and p99 344 ms, which fails; the cold proxy is p50 331 ms.

### Next
- User decision on ADR 0007 (QRhi editor backend in M1). Then independent review and merge of `m0/s1`.
- When m0/io lands, port `tests/perf/s1/child_process.cpp` to `IProcessLauncher`.

### Blockers
- None. DWM ETW needs elevation, so it is deferred (ADR 0007).

### Questions for the user
- Approve the QRhiWidget backend as M1's default editor renderer (ADR 0007, decision 1)?
- B6 "missed ≤1 %" cannot pass on this desktop while video, Wallpaper Engine and GPU load run. Should gate runs
  happen on a quiet desktop, or should the gate be relative to a same-session baseline? A budget change needs your
  approval.

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
