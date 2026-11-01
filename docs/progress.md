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
- M3: the B24 harness, starting from `s7_lsp.py` and `session_state.py` (in `E:\FrostBuilds\perf\m0\s7\spike\`).

### Blockers
- None for M0. B24's M3 value waits for the user's confirmation.

### Questions for the user
1. B24's M3 value: confirm p50 18.4 s from the quiet control, or choose otherwise, for example 25.1 s from the runs
   under your workload, or a re-measurement that also makes the DXGI monitor-power check S7 did not make.
2. Should the M3 gate require a quiet REF-W1, with your heavy workload closed and the run scheduled with you? Under
   set 1's workload every stock clangd run took longer than 18.4 s (19.77–22.45 s).
3. Should `tests/corpus/manifest.json` pin R3's configuration (Ninja, `LLVM_ENABLE_PROJECTS=clang`, Release, the
   clang-cl toolchain) for all R3 budgets?

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
