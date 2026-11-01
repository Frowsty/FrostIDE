# FrostIDE progress log

Session hand-off log (CLAUDE.md, PLAN.md section 6.7). Newest entry first. Each entry lists what was done,
what is next, blockers and open questions for the user.

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
