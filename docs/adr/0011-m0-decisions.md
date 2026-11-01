# ADR 0011: M0 decisions

- Status: accepted
- Date: 2026-11-01
- Amended: 2026-11-01 by ADR 0029 (the thin path). B1: the S6 cold samples and the user's B23 first-project value
  are needed before M16, where ProjectHost lands, not before M2; M2 no longer depends on D3. B3: the five
  preconditions also apply to M2's B23 first-project gate on path (c), with `MSBuild.exe`'s cold start in place of
  ProjectHost's. B4: ProjectHost before L1 is M16's target; path (c) starts after L1. B5 and E2 are unchanged in M2.
- Plan reference: PLAN.md section 6.7 (working agreement, sign-off), section 7 (M0 spikes and exit, "Every
  milestone exit also requires"); ADRs 0002, 0003 and 0005–0010; `docs/progress.md` (entry "2026-11-01: M0
  overnight run and exit verification", questions A1–G1); the M0 demo pack `E:\FrostBuilds\demo\m0\index.html`
  (questions E5, G2 and G3)

## Context
The M0 overnight run ended with 28 questions for the user: A1 to G1 in `docs/progress.md`, and E5, G2 and G3,
which the demo pack raised. The demo pack gives each question the recommendation of an ADR, or says that none
makes one, and the progress log adds no recommendation of its own:
- **Nine had an ADR's recommendation:** A1 (ADR 0007 decision 1), A2 (ADR 0007, option (c) of its question 2),
  B2 (ADR 0006 decision 3, the trees proposal), B3 (ADR 0006 decision 6), B7 (ADR 0005, Consequences), C1
  (ADR 0008 decisions 1 and 2), C3 (ADR 0008, Consequences: no decision needed now), D2 (ADR 0009 item 3) and F3
  (ADR 0010 decision 2, for the reading; the question's second part, fixing the probe files and the job counts,
  had none).
- **The other 19 had none:** A3 (none explicit in ADR 0007), A4, B1, B4, B5, B6, C2, D1, D3, E1, E2, E3, E4, E5,
  F1, F2, G1, G2 and G3. For some of them an ADR sets out the options or a wording without choosing: B1 and B4
  (ADR 0006), D1's exception, to be granted or declined (ADR 0009), and D3's wording (ADR 0009 decision 2).

On 2026-11-01, at about 09:10, the user answered with one instruction: "Follow your own recommendation what to do.
The path limit should be no problem now, I've enabled long paths". After that instruction, the session wrote its
own recommendation for each of the other 19, and for F3's second part, into its decision list (`decisions.md`,
2026-11-01, about 09:10). So every question below is decided as recommended, on the user's instruction of
2026-11-01 to follow the recommendation: the nine on their ADR's recommendation, the rest on the session's, whose
reasoning each gives under "Basis". None of the wording below is the user's own.

The user had also turned the long-path switch on: `LongPathsEnabled` under
`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem` is now 1 (checked with `reg query`). Until then it had been 0,
so whether the switch lifts the loader's 250-unit limit (questions E1 and E4) had never been measured. It was
measured at 09:00 (Measurements). The switch does not lift the limit, and E1 is decided on that measurement.

Each decision gives its basis (the ADR decision it accepts, or the reasoning), its consequence, and where it is
implemented: now (on this branch), at a later M0 step, or in a later milestone.

## Decision
### A. Renderer and first frame (S1, ADR 0007)
- **A1. QRhi is M1's editor renderer on Windows.**
  - Basis: ADR 0007 decision 1 is accepted, with decisions 5 and 8. Raster fails B6's UI-CPU criterion at
    5120x1392 by its own cost in every round (p99 medians 6.29–9.18 ms against 5 ms). QRhi passes it in 10 of 10
    rounds (p99 0.86 ms).
  - Consequence: raster stays the golden oracle and the fallback for RDP, WARP and remote sessions. Linux keeps
    raster until a Linux S1 leg measures both backends. QRhi's start-up, which adds 216.9 ms to the first frame
    at p50, must stay off B1 without moving onto B2 or B15 (ADR 0007 decision 3).
  - Where: M1.
- **A2. B6's missed and interval criteria: option (c).**
  - Basis: ADR 0007's proposal for its question 2. Where QRhi's work-area misses come from is not established:
    trivial windows on the same desktop missed more.
  - Consequence: the gating is decided at M1, after a quiet-desktop control and PresentMon's present mode for
    QRhi. No budget changes.
  - Where: M1.
- **A3. B1 keeps 150/250 ms as M1's target.**
  - Basis: no re-baseline, because budgets are not weakened (CLAUDE.md, definition of done, item 3). ADR 0007
    decision 4's M1 work (start-up ETW markers, profiling the first `show()`, QRhi's start-up off the first frame)
    brings B1 down.
  - Consequence: B1 gates M1 (PLAN.md section 4), and frost.exe's warm first frame is p50 264.9 ms today, so this
    is M1 work to do, not a margin.
  - Where: M1.
- **A4. Cold B1 is measured after a reboot, not after a RAMMap purge.**
  - Basis: `RAMMap -Et` needs elevation and a Sysinternals download. A reboot needs neither.
  - Consequence: M1 builds a harness that runs the first-frame steps first after logon. The user reboots only when
    it suits them. Nothing happens now.
  - Where: M1.

### B. ProjectHost and B23 (S2/S6, ADRs 0005 and 0006)
- **B1. B23's first-project value: ADR 0006 decision 3, option (i), with the samples collected passively.**
  - Basis: option (i) keeps PLAN.md's order, "S6 fixes B23 before D3 is final". Collecting one sample per ordinary
    reboot asks for no reboot of its own.
  - Consequence: each ordinary reboot yields one cold S6 sample, through the same logon harness as A4. D3 stays
    provisional until 10 samples exist and the user has fixed B23's first-project value from them, as ADR 0006
    option (i) says. 3 s stays the ceiling: a final value above it needs an ADR and the user's approval. M1 does
    not depend on D3. M2 does (PLAN.md section 7, M2 "Depends on"), so the samples are needed before M2 starts;
    if they are not in by then, that is a question for the user. PLAN.md's S6 spike-list line now gives this
    method (10 cold samples, one per ordinary reboot).
  - Where: now (PLAN.md's S6 spike-list line); M1 (the harness and the samples), then the user's value before
    M2.
- **B2. B23's trees target stays provisional.**
  - Basis: ADR 0006 decision 3's trees proposal is accepted. S6 gives no evidence for the trees value, and no M0
    spike measured the native readers.
  - Consequence: 1.5 s stays provisional, and the ceiling, until M2's first B23 trees measurement on R3. This is a
    plan change. PLAN.md's B23 row note and its S6 spike-list line now say that S6 fixes only the first-project
    value, and `tests/perf/budgets.json`'s note on the trees target says the same. The target values are
    unchanged.
  - Where: now (PLAN.md, `budgets.json`); the measurement in M2.
- **B3. B23's preconditions: all five of ADR 0006 decision 6.**
  - Basis: ADR 0006 decision 6 is accepted. M2 needs (d) and (e) for a well-defined measurement:
    - (a) R2b's NuGet packages are restored with the repository's own `dep\nuget\nuget.exe`;
    - (b) the active solution configuration is Debug|x64;
    - (c) the first project streamed is chosen by ADR 0006 decision 5's rule (Host.EXE on R2b);
    - (d) trust is granted as soon as the open request is made, so no warm-up runs;
    - (e) the gating sample is cold, with a warm-cache sample reported alongside.
  - Consequence: `budgets.json`'s B23 preconditions state them. M2's open path must skip the warm-up when trust
    comes first. If ProjectHost's launch waits for L1 (B4's fallback), the timed interval holds that wait.
  - Where: now (`budgets.json`); M2 measures under them.
- **B4. ProjectHost before L1 in a trusted root: answer 2 of ADR 0006 decision 2.**
  - Basis: answer 2 keeps PLAN.md's no-network-before-L1 rule (sections 2.1 and 2.2) without putting the time to L1
    on B23's first-project path.
  - Consequence: M2's target is to launch ProjectHost before L1 but keep evaluation off the network until L1, and
    to defer a project that needs the network. Nobody has built or measured that yet. If answer 2 proves
    infeasible, answer 3 (wait for L1) is the fallback. Answer 1 (exempting the trusted project's own MSBuild
    logic from the network rule) is rejected. PLAN.md does not change now. Whichever answer ships, M2 measures the
    host's effect on L1 and on the tree fill.
  - Where: M2.
- **B5. The UWP workload is deferred to M2.**
  - Basis: only R2b's C++/WinRT projects need it, and TerminalAppLib is evaluated in M2. The install needs
    elevation, which means a UAC prompt on the desktop, so the user runs it then.
  - Consequence: `docs/progress.md` records the installer command. Until then R2b measurements use the
    OpenConsole host projects, as the corpus manifest says.
  - Where: M2.
- **B6. The vcpkg files that S2 left in `%LOCALAPPDATA%\vcpkg` are deleted after a check.**
  - Basis: the directory holds caches that vcpkg downloads again when it needs them (ADR 0005, Consequences). The
    check: every file's timestamp is from 2026-10-31 or later, and no process holds them.
  - Consequence: read by modification time, the check did not pass: 1,035 files and 3 directories under
    `downloads\tools\` keep the older times that vcpkg's extraction kept from the tool archives (ADR 0005 says
    the same). By creation time, `downloads\` (1,040 files, created 20:55:09–20:55:17 on 2026-10-31) and
    `registries\` (3,161 files, created 20:54:53–20:55:08) both match S2's TerminalAppLib reference build
    (20:54:53–20:55:17, ADR 0005), and no running process referenced the directory. At 09:55 on 2026-11-01 the
    session deleted those two: 415,726,990 bytes (396.5 MiB) and 136,086,954 bytes (129.8 MiB). `archives\` is
    kept: 3 binary-cache packages, 6,292,779 bytes (6.0 MiB), created 18:52:29–18:52:47, before S2's run, so
    they are not S2's, and where they came from is uncertain. `tools/env.ps1` sends vcpkg's downloads, binary
    cache and, since this branch, its registry clones (`X_VCPKG_REGISTRIES_CACHE`) to E:, so third-party builds
    must run under it (`docs/progress.md`).
  - Where: now (done).
- **B7. R2b's Windows SDK prerequisite is 10.0.26100.0.**
  - Basis: ADR 0005 (Consequences) and ADR 0006 (follow-ups): R2b's pinned commit defaults to 10.0.26100.0, and
    `sdk/sdk.lock.json` pins it. 10.0.22621 is not installed on REF-W1.
  - Where: now (PLAN.md section 4, `tests/corpus/manifest.json`).

### C. clangd index (S7, ADR 0008)
- **C1. B24's M3 value is p50 18.4 s, and the M3 gate runs on a quiet machine.**
  - Basis: ADR 0008 decisions 1 and 2 are accepted: the quiet-machine control's p50 of 15.31 s ×1.2.
  - Consequence: the M3 gate runs on a quiet REF-W1 with the user's heavy workloads closed, scheduled with the user
    (PLAN.md T12). Under set 1's workload every run of stock clangd took longer than 18.4 s (19.77–22.45 s), so a
    run under load does not compare. `budgets.json`'s note on the M3 slot and PLAN.md's S7 spike-list sentence
    now say confirmed.
  - Where: now (`budgets.json`, PLAN.md); the gate in M3.
- **C2. R3's configuration is pinned for all R3 budgets.**
  - Basis: ADR 0008 decision 2: the configuration fixes the index and so the load, and a run configured otherwise
    indexes something else and does not compare with the S7 baseline.
  - Consequence: `tests/corpus/manifest.json` pins S7's configure command: Ninja, `LLVM_ENABLE_PROJECTS=clang`,
    Release, clang-cl through FrostIDE's `cmake/toolchains/windows-clangcl.cmake`, llvm-ml64 as the MASM
    assembler and exported compile commands. The pinned compiler is clang-cl, so the pin describes REF-W1; no
    Linux configuration of R3 is pinned.
  - Where: now.
- **C3. Noted; no decision.** Two budget risks stay on record: B24's M13 target (p50 2 s) is about 8× (quiet) to
  10× (under the user's workload) faster than stock clangd on R3, and a warm clangd on R3 holds 3.16 GiB of B26's
  4 GB idle budget. Nothing changes now.

### D. Tracy (ADR 0009)
- **D1. Tracy's listener: option (a), a dev-builds-only exception.**
  - Basis: ADR 0009's threat model and its items 2–9, which become decisions. PLAN.md 2.2 and 5.2 require every
    listener to have a token or an ACL. Tracy's protocol has no place for either, so this is an exception for
    Tracy alone, under these conditions.
  - Consequence: Tracy is linked only in dev builds and started only by an explicit per-run opt-in, on demand, with
    `TRACY_ONLY_LOCALHOST`, `TRACY_NO_BROADCAST`, `TRACY_NO_CODE_TRANSFER`, `TRACY_NO_CALLSTACK` and
    `TRACY_NO_SYSTEM_TRACING` (among item 5's options), plus the listener-enumeration and release-build tests. So
    Tracy captures no call stacks, no samples and no context switches. ADR 0009 records the exception and its
    conditions. M1 adds PLAN.md 5.2's Tracy row together with the dependency (ADR 0009, Consequences).
  - Where: M1 (ADR 0009's M1 work list); the listener-enumeration test in M2.
- **D2. Budget and perf runs never start Tracy.**
  - Basis: ADR 0009 item 3's rule, confirmed. `StartupProfiler` costs about 215 ms on the first-frame path, and
    `ShutdownProfiler` alone took up to 62.5 ms at p50 against B20's 150 ms.
  - Where: M1, with Tracy (`frost-bench`, `verify -Perf`).
- **D3. PLAN.md's Tracy wording.**
  - Basis: ADR 0009 decision 2 proposed a clearer wording of PLAN.md's "tracy (pinned in M0)", adjusted here for D1
    (a): "tracy (version chosen in M0: 0.14.1 from the vcpkg baseline; use under the dev-builds-only listener
    exception of ADR 0009)".
  - Consequence: the M0 spike list's "Tracy version" line also records ADR 0009's choice, 0.14.1.
  - Where: now (PLAN.md section 2.1, Dependencies, and the M0 spike list).

### E. Start-up and paths
- **E1. frost must start from every directory length, whether the switch is on or off.**
  - Basis: with `LongPathsEnabled` = 1 the loader still refuses DLLs with RT_MANIFEST 2 beyond 250 UTF-16 units
    (Measurements). So E1's option (a), the switch, does not meet the M0 exit criterion "launch from a path with
    ... >260 characters" on its own.
  - Consequence: the criterion stays as written, so E1's option (b), an ADR that changes it, is not taken. Making
    frost start from every length is the long-path work on branch `m0/longpath`; this decision only records it.
    Until it lands, frost is worse on this machine with the switch on than it was with it off: its start-up check
    trusts the switch and no longer refuses. A Release frost from a directory of 226–235 units waits on Qt's
    message box, a Debug one from 225–234 units aborts, and both do the same from 250 units on.
  - Where: M0, on `m0/longpath`.
  - Outcome (2026-11-01, ADR 0012; `m0/longpath` merged into `m0/foundation` in 2d824ce): **met.** The user made
    two decisions, quoted in ADR 0012's Context:
    - "Use the launcher design" (about 11:00). frost.exe is now a small launcher that imports KERNEL32.dll only
      and loads frost_app.dll, and in ASan trees clang's ASan runtime, through `\\?\` paths. No DLL that embeds
      RT_MANIFEST 2 is a static import of frost.exe any more, and Qt's DLLs stay unmodified.
    - "Restart via short link (Recommended)" (about 14:15). The launcher's review found a second limit, not the
      loader's: NVIDIA's user-mode driver ends any process whose image path is 260 UTF-16 units or longer
      (0xC0000409 in nvldumdx.dll). So from 260 units, a start that may load a GPU driver restarts through a
      junction under `%LOCALAPPDATA%\FrostIDE\i` (ADR 0012, decision 8).

    The command-line modes and the offscreen self-test start in place at every measured length: 22 lengths from
    200 to 400 units, plain and `\\?\`, in Release, Debug and ASan (`StartsFromEveryDirectoryLength`). With long
    paths ineffective, simulated by removing longPathAware from a copy of the manifest, 8 of those lengths are
    tested, plain only up to 249 units, and the self-test fails only its long-path-manifest check, as expected
    (`StartsWithLongPathsIneffective`). The GUI and the on-screen self-test start through the link from 260 units
    on. The on-screen self-test is tested from image paths of 250 to 400 units
    (`SelfTestOnScreenFromEveryImagePathLength`). The GUI's own window is tested at 300 units on a private desktop
    (`GuiStartFromALongPathShowsItsWindow`) and was run on the user's desktop at 310. Some starts are refused with a
    message, among others install directories over about 4,089 units and elevated GUI or on-screen starts from
    260 units on (ADR 0012, Consequences, "New refusals"). The restart costs about 10 ms for an on-screen start,
    against the 5 ms that the user's option named. One direct ShellExecuteW of a 310-unit frost.exe returned 5 and
    started nothing; that the length causes it, and that Explorer and shortcuts, which go through the shell, fail
    the same way, is inferred, not measured. A terminal or script start works (ADR 0012, Measurements).
- **E2. Trusted user-script children get Visual Studio-compatible cmd.exe behaviour.**
  - Basis: frost's start-up sets NoDefaultCurrentDirectoryInExePath=1, and every child inherits it. cmd.exe in a
    user build script then no longer runs a tool from its current directory by bare name, as it does under Visual
    Studio. A trusted root already runs its own build logic, so restoring that search for its scripts grants
    nothing new. Restricted children keep the hardened behaviour.
  - Consequence: trusted user-script children (MSBuild Exec tasks and build events, CMake configure, and later
    Build/Run and the terminal) get NoDefaultCurrentDirectoryInExePath stripped from their environment.
    Restricted children (read-only git, clangd, bundled tools) keep it.
  - Where: M2, with the launcher's environment policy; the TODO(M2) in `src/io/include/frost/io/process.hpp`
    records the decision now.
- **E3. POSIX saves refuse read-only targets.**
  - Basis: on Windows, `writeFileAtomic` already refuses to replace a read-only file (PermissionDenied). On POSIX,
    rename needs only write access to the directory, so a 0444 file is replaced silently today.
  - Consequence: frost::io refuses a non-writable target with PermissionDenied on POSIX too, matching Windows, and
    M1's SaveManager offers an explicit "overwrite read-only file" action.
  - Where: M1, with a test; a TODO(M1) at `writeFileAtomic`'s POSIX doc in
    `src/io/include/frost/io/file_system.hpp` now.
- **E4. The failing ASan test is resolved by the E1 fix, if it can be.**
  - Basis: E4's option (b) is E1's fix. Option (d), branch `m0/e4-asan-runtime`, turns the only red test of the
    product gap green (its review). With the switch on, the ASan case still fails: the ASan frost dies with
    0xC0000106 from directories of 218 to 249 units, the test's 225 and 226 included (Measurements).
  - Consequence: if the E1 fix makes `FrostProcess.PlatformPluginPathBoundary`'s ASan case pass, or makes it moot,
    on its own, `m0/e4-asan-runtime` is not merged, and the pushed branch stays for the record. Otherwise it is
    merged (option (d)).
  - Where: M0, after `m0/longpath`.
  - Outcome (2026-11-01, ADR 0012, Measurements, "E4"): **closed by the E1 fix; `m0/e4-asan-runtime` is not
    merged** and stays pushed at 19157a8 for the record. Both conditions above hold:
    - The ASan case passes for a real reason. Its 225- and 226-unit starts died because the ASan runtime was a
      static import of frost.exe. The launcher has no such import and loads the runtime by its `\\?\` path, so
      the ASan frost starts at 225, 226 and every other measured length without a changed fixture.
    - The test itself is moot. 4c5533d replaced `PlatformPluginPathBoundary` with tests that assert at least as
      much (ADR 0012, "Removed and relaxed tests"). They pass in the ASan tree: win asan 419/419 at b9dd60f.

    Option (d) would also go against the user's first decision. Its stripped runtime belonged to "Approve
    stripping", the option the user passed over for "Use the launcher design" (ADR 0012, Context and
    Alternatives), and ADR 0012's decision 7 modifies no DLL.
- **E5. Feature-matrix row I7 gets its acceptance tests after the E1 fix.**
  - Basis: I7 ("Windows long paths, UTF-8 code-page manifest") is the matrix's only M0 row, and every milestone
    exit needs a green matrix slice (PLAN.md section 7). `FrostProcess.SelfTestPasses` proves the manifest and
    UTF-8 parts; the long-path part needs E1's fix.
  - Consequence: once the E1 fix lands, I7's acceptance tests are `FrostProcess.SelfTestPasses` plus the long-path
    launch tests, and I7 is marked as the matrix's state rules in `tools/featurecheck.py` allow (green needs an
    acceptance test). That is the demo pack's option (a). `tests/features/matrix.json` changes after
    `m0/longpath` merges, not on this branch.
  - Where: M0, after `m0/longpath`.
  - Outcome (2026-11-01, ADR 0012): **done**, on `m0/longpath` itself (4c5533d, 33a329a and 2f40f9f) rather than
    after its merge. I7 is `green` in `tests/features/matrix.json`, with six acceptance tests:
    `FrostProcess.SelfTestPasses`, `StartsFromEveryDirectoryLength`, `StartsWithLongPathsIneffective`,
    `RunsFromPathLongerThanMaxPath`, `SelfTestOnScreenFromEveryImagePathLength` and
    `GuiStartFromALongPathShowsItsWindow`. featurecheck passes. This is option (a) with more tests than it named.

### F. Machine and tooling
- **F1. Logitech G HUB: no action.**
  - Basis: later in the night G HUB held only 175–189 MB, so the 43 GB of commit was transient.
  - Consequence: nothing is restarted. The overnight tooling (`night.py`) keeps checking free commit before builds.
  - Where: nothing to implement.
- **F2. No sccache on Linux.**
  - Basis: ADR 0010 measured the Linux build of a core change at 1.4 s. The WSL leg's cost is its configure (23 s),
    about 15 s of it two FindOpenGL `find_path` searches for headers that are not installed, and a compiler cache
    does not touch configure.
  - Consequence: no Linux sccache pin and no ADR for one. M1 caches or avoids the FindOpenGL searches (ADR 0010's
    follow-up; a TODO(M1) in `docs/progress.md`).
  - Where: M1.
- **F3. B28's "rebuilt and linked" is the full default build after the change: option (a).**
  - Basis: ADR 0010 decision 2 is accepted. It is the strictest reading, the one `verify -Quick`'s build step
    uses, and it also covers library files that `frost` does not link, such as `src/sched`.
  - Consequence: `budgets.json`'s B28 method fixes the reading, the probe files (`src/core/src/error.cpp`,
    `src/app/src/main.cpp`, `src/sched/src/lane.cpp`) and the job counts (ninja's default parallelism for the
    rebuild, as ADR 0010 measured it, and `-Jobs 4` for `verify -Quick`). The targets are unchanged.
  - Where: now.

### G. Sign-off
- **G1. M0 sign-off.**
  - Basis: PLAN.md section 6.7 ("The next milestone starts only after explicit user sign-off") and section 7
    ("Every milestone exit also requires").
  - Consequence: M0 is signed off once `verify -Full` is green on both platforms at `m0/foundation`'s final head
    and every item in this ADR is recorded. Then that commit is tagged `m0` (annotated, with the `-Full` footer),
    `main` is created from it, and both are pushed. Starting M1 is a separate question for the user, because M1
    work loads the PC.
  - Where: M0, after E1, E4, E5 and G3.
  - Note (2026-11-01, afternoon): E1, E4, E5 and G3 are done (their outcomes above and below). `m0/decisions` and
    `m0/longpath` are merged into `m0/foundation` (52809c6, 2d824ce), and the exit record follows them. The
    sign-off runs `verify -Full` on both platforms at that final head and tags and pushes only if it is green
    (`docs/progress.md`, entry "M0 complete"). Recording the result takes one more commit, docs only, which fills
    the `-Full` footer into `docs/progress.md`: that commit, "M0: sign-off record", is the one tagged `m0` and the
    one `main` starts from, and the tag's message says that `-Full` ran on its parent. The separate question about
    starting M1 was answered by the user at about 14:35: "go ahead finishe M1, start M2 when M1 is finished and so
    on".
- **G2. ADR 0002 is accepted.**
  - Basis: `m0/sched` is merged and was independently reviewed, and its M0 exit numbers are met, including the
    exit-scale TSan re-run on 3563873 (`docs/progress.md`).
  - Where: now (ADR 0002's status).
- **G3. One more comment-accuracy round, and the three open code items.**
  - Basis: PLAN.md 6.6 asks for comment-accuracy findings to be fixed before sign-off. The `m0/dll-copy` build code
    merged after the four rounds and had independent checks, but no comment-accuracy round, and the E1 fix will
    be new code.
  - Consequence: a comment-accuracy round covers `cmake/FrostLayers.cmake`'s runtime-DLL pass,
    `tests/cmake/check_runtime_dlls.cmake` with its fixture, and the E1 fix. The three open code items:
    - POSIX strerror text in a translated locale: a TODO(M1) at the site (`src/io/src/os_error.cpp`), now;
    - `screen_probes`' capture retry after a rotated output: its TODO(M1) already exists;
    - the refusal message's LongPathsEnabled advice: fixed by the E1 fix.

    This is closest to the demo pack's option (c), with the E1 fix added to the round.
  - Where: M0. The round over the `m0/dll-copy` code is done: comment-accuracy round 5 applied its 9 findings in
    bc42ada, comments only (`docs/progress.md`). The round over the E1 fix is `m0/longpath`'s review. M1: the
    strerror text and the capture retry.
  - Note (2026-11-01, afternoon): the round over the E1 fix is done. `m0/longpath`'s reviews of 4c5533d and
    2f40f9f were fixed in 33a329a and b9dd60f (ADR 0012). The refusal message with its LongPathsEnabled advice no
    longer exists: frost no longer refuses long plugin paths.

## Measurements
### Long-path launch with LongPathsEnabled = 1
- **When and what.** 2026-11-01 at 09:00, on REF-W1 (Windows 11 10.0.26300), with `LongPathsEnabled` = 1. The
  binaries are the Release, Debug and ASan (RelWithDebInfo) builds of frost.exe that `verify -Full` built from
  2c499a2 at 04:47–04:53. `m0/foundation`'s later commits change docs and comments only.
- **Method**, the same as for the `-Full` finding (`E:\FrostBuilds\perf\m0\exit-full\scripts`):
  - each run copies frost.exe, every DLL beside it and `platforms\` into a directory of an exact length in UTF-16
    units;
  - it starts `frost --selftest` the way frost::io's launcher does: through the plain path while frost.exe's path
    is shorter than 260 units (MAX_PATH), and through the `\\?\` form from 260 on;
  - it runs on a private desktop that is never shown, with critical-error boxes off, and a 60 s timeout;
  - the lengths are chosen around each build's limits (the longest DLL at 249–251 units, the platform plugin at
    250 and 251, and the last plain and first `\\?\` launch), so ranges are sampled at their ends only.

| Build | Directory (units) | Launch | Result |
|---|---|---|---|
| Release | 225 | plain | self-test passes |
| Release | 226, 234, 235 (plugin at 251–260) | plain | timeout at 60 s |
| Release | 236, 249 (`Qt6Widgets.dll` at 251, 264) | plain | 0xC0000106 before `main` |
| Release | 250 | `\\?\` | timeout at 60 s |
| Debug | 224 | plain | self-test passes |
| Debug | 225, 233, 234 (plugin at 251–260) | plain | 0xC0000409 |
| Debug | 235, 249 (`Qt6Widgetsd.dll` at 251, 265) | plain | 0xC0000106 before `main` |
| Debug | 250 | `\\?\` | 0xC0000409 |
| ASan | 216, 217 | plain | self-test passes |
| ASan | 218, 225, 226, 249 (`clang_rt.asan_dynamic-x86_64.dll` at 251–282) | plain | 0xC0000106 before `main` |
| ASan | 250 | `\\?\` | timeout at 60 s |

- **Reading.** The loader limit is unchanged: every DLL with RT_MANIFEST 2 still fails beyond 250 units, at the
  same lengths as with the switch off (ADR 0003, Consequences). What changed is frost's own check. It trusts the
  switch and no longer refuses, so Qt tries the platform plugin beyond 250 units, fails, and shows its message
  box (on the hidden desktop: the timeouts) or aborts through qFatal (0xC0000409, Debug). With the switch off,
  frost refused cleanly at those lengths (exit 1).
- **Raw data.** `E:\FrostBuilds\perf\m0\longpaths-on\run.log` and `scripts\` (`measure_dll_limit.py`,
  `run_hidden_nobox.py`, `results.jsonl`).

## Consequences
- **On this branch** (`m0/decisions`):
  - the statuses of ADRs 0002, 0006, 0007, 0008, 0009 and 0010, each with a "Decided 2026-11-01 (ADR 0011)" note
    beside the original text;
  - PLAN.md: B23's row note (B2) and the S6 spike-list line (B1, B2), the Tracy wording (D3), R2b's Windows SDK in
    section 4 (B7), and in the M0 spike list the S7 sentence (C1) and the Tracy version line (D3);
  - `tools/env.ps1`: vcpkg's registry clones go to E: (`X_VCPKG_REGISTRIES_CACHE`, B6), which
    `tests/tools/test_windows_scripts.py` checks;
  - `tests/perf/budgets.json`: B23's preconditions and its trees note (B2, B3), B24's M3 note (C1) and B28's
    method (F3). No target value changes;
  - `tests/corpus/manifest.json`: R2b's SDK (B7) and R3's configuration (C2);
  - comments only: the TODO(M2) in `process.hpp` (E2), and TODO(M1)s at `writeFileAtomic` (E3) and at the POSIX
    strerror site (G3);
  - comments only: comment-accuracy round 5 over the `m0/dll-copy` build code, G3's first part; its 9 findings
    are applied in bc42ada (`docs/progress.md`).
- **Left for M0's final integration:** PLAN.md's M0 status paragraph and its next-step line.
- **Remaining M0 steps, in order:** the E1 fix (`m0/longpath`, which also touches ADR 0003), whose review is G3's
  comment-accuracy round over it; E4's outcome; I7's acceptance tests (E5); `verify -Full` on both platforms at
  `m0/foundation`'s final head; then G1.
- **Update 2026-11-01 (afternoon), M0's exit record:** PLAN.md's M0 status paragraph and its next-step line now
  say that M0's exit is met. The E1 fix, its review, E4's outcome and I7's acceptance tests are done (the outcomes
  under E1, E4, E5 and G3). What remains is `verify -Full` at `m0/foundation`'s final head, then G1.
- **M1:** QRhi on Windows (A1) and the B6 gating decision (A2); B1's work (A3); the logon harness for cold B1 and
  the passive S6 samples (A4, B1); Tracy under the exception (D1, D2); POSIX read-only refusal (E3); the
  FindOpenGL searches (F2); the strerror text and the capture retry (G3).
- **M2:** B23's trees measurement (B2), measured under the five preconditions (B3); ProjectHost before L1 without
  network (B4); the UWP workload (B5); the environment policy for user-script children (E2). D3 must be final
  before M2 (B1).
- **M3:** B24's gate on a quiet machine (C1).
- No budget target, test or warning is weakened. The only budget changes are notes, preconditions and a method.

## Alternatives considered
- **A2 (a) or (b) now** (a quiet desktop or the whole output, with the absolute budget): neither is known to
  pass at the work area, and the misses' source is unknown, so the choice waits for M1's control.
- **A3, re-baselining B1:** it weakens a budget, which needs an ADR and the user's approval. Not proposed.
- **A4, RAMMap -Et before each launch:** it needs elevation and a Sysinternals download.
- **B1 (ii), the final value from M2's B23 test:** M2 would build the C# ProjectHost before B23 is fixed, which
  inverts PLAN.md's gate, and B23 would be set by the measurement it gates.
- **B4 answer 1, exempting the trusted project's MSBuild logic from the network rule:** rejected; it changes
  PLAN.md's rule. Answer 3 (wait for L1) stays the fallback.
- **B5, installing the workload now:** a UAC prompt while the user is at the PC, for a project first evaluated in
  M2.
- **C1, 25.1 s from the runs under the user's workload, or a re-measurement with the DXGI check:** the quiet
  control is the baseline that an M3 gate on a quiet machine compares with.
- **D1 (b) or (c), declining or deferring the exception:** (b) drops Tracy from PLAN.md, and interactive
  profiling falls back to ETW, Perfetto and external sampling profilers; (c) leaves ADR 0009's M1 work list
  blocked.
- **E1 (b), changing the M0 exit criterion:** not taken; the criterion stays and frost is fixed. How the fix
  starts frost at every length (E1's (c) and (d) among the candidates) is the `m0/longpath` work's design.
- **E2, keeping the hardened behaviour for every child:** user build scripts written for Visual Studio that call a
  tool in their current directory by bare name would fail under FrostIDE only.
- **E3, a warning only, or a refusal in the SaveManager only:** frost::io would keep behaving differently on the
  two platforms.
- **E4 (a), skipping the test in ASan trees:** it weakens the ASan gate. **E4 (c)**, keeping the failure as known,
  is superseded once the E1 fix lands.
- **E5 (b), carrying I7 into M1:** a scope move. **E5 (c)** depends on E1 (b), which is not taken.
- **F2, a Linux sccache pin:** a core change's Linux build already takes 1.4 s, so it would save little.
- **F3 (b) `--target frost` and (c) the executables that cover the change:** both are weaker readings; (b) times
  nothing at all for a change in `src/sched`.
- **G3 (a), accepting the result as it is:** the `m0/dll-copy` code would go into M0 without a comment-accuracy
  round. **G3 (b)**, fixing the code items before sign-off as well: the strerror fix would need a test under a
  translated locale before sign-off, and this WSL has none (comment-accuracy round 2).
