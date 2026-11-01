# FrostIDE progress log

Session hand-off log (CLAUDE.md, PLAN.md section 6.7). Newest entry first. Each entry lists what was done,
what is next, blockers and open questions for the user.

## 2026-11-01 (afternoon): M0 complete

M0's exit is met. `m0/decisions` and `m0/longpath` are merged into `m0/foundation`, and this exit record follows
them: PLAN.md's M0 status and next-step lines, notes in ADRs 0011 and 0012, and pointers in the entries below. The
launch from a path over 260 characters, the last unmet criterion, is met by ADR 0012, on two decisions by the user.
The sign-off follows ADR 0011's G1: `verify -Full` on both platforms at `m0/foundation`'s final head, then, if it
is green, the annotated tag `m0` with its footer on that commit, `main` created from it, and both pushed. The work
was done in the main worktree, starting from `m0/foundation` at 593e15f, which was clean.

### Done
#### Landed on `m0/foundation`
- **52809c6**, `git merge --no-ff m0/decisions` (c0c3f3c). It brings ADR 0011 and the decisions it applies
  (7fb5a27), comment-accuracy round 5 (bc42ada), and the decisions check follow-ups with
  `X_VCPKG_REGISTRIES_CACHE` (c0c3f3c). No conflicts.
- **2d824ce**, `git merge --no-ff m0/longpath` (b9dd60f):
  - 67838d1: test children start on a private desktop;
  - 4c5533d: the launcher, with its review fixes in 33a329a;
  - 2f40f9f: the short-junction restart, with its review fixes in b9dd60f;
  - ADR 0012.

  Three header comments conflicted: `cmake/FrostLayers.cmake`, `tests/cmake/check_runtime_dlls.cmake` and the
  fixture's `shared/CMakeLists.txt`. Each keeps round 5's corrections and adds `m0/longpath`'s shared libraries.
  The merged tree's non-comment lines in those files equal `m0/longpath`'s.
- **The exit record** (09f428f): PLAN.md's M0 status paragraph and next-step line; notes in ADR 0011 (E1,
  E4, E5, G1, G3, Consequences) and ADR 0012 (the on-screen restart, the shell's limit); this entry; and the
  decided and done pointers in the entries below. Its commit message gives the on-screen restart's time as about
  16:00; it was about 16:30 (the fixes below).
- **The exit record's check** (a read-only check of 09f428f) found 12 docs issues, all fixed in the next commit,
  docs only, together with the on-screen README and the demo pack:
  - the restart's time (about 16:30, not 16:00);
  - the M0 status line's "S1 (ADR 0007, proposed)", now accepted;
  - the shell's limit, now marked as partly inferred (one ShellExecuteW at 310 units was measured);
  - the full list of refusals (ADR 0012, Consequences, "New refusals") instead of "two";
  - what is tested with long paths ineffective (8 lengths, `StartsWithLongPathsIneffective`);
  - which GUI starts were tested at which lengths;
  - "no measurable B1 cost on screen" instead of "no B1 cost";
  - the on-screen README's evidence gaps: the CreateProcessW runs' script was not saved, the `.lnk` caveat, and
    "within about 0.2 s" (every `after_s` is 0.202, the polling interval by the look of it);
  - in the demo pack, row 16 back to pending until the push, its section 1 intro, and the new questions renamed
    LP1 and LP2, away from PLAN's load stages L0-L4;
  - which commit gets the tag (ADR 0011, G1 note).
- **Not merged:** `m0/e4-asan-runtime` (19157a8). It stays pushed for the record (E4, below).

#### E1 in short: frost starts from any path length (ADR 0012)
- **The switch does not lift the loader's limit.** LongPathsEnabled=1 has been on since this morning. The
  loader still refuses a plain path over 250 UTF-16 units for any DLL that embeds RT_MANIFEST 2, which every Qt
  DLL and plugin and clang's ASan runtime do. The `\\?\` form of the same path loads at any length (ADR 0011,
  Measurements; ADR 0012, Context).
- **The launcher** (4c5533d, review fixes 33a329a). frost.exe is now a small launcher. It uses the static CRT and
  imports KERNEL32.dll only. It loads frost_app.dll, and in ASan trees the ASan runtime, through `\\?\` paths.
  frost_app.dll then pins Qt's platform plugin through `\\?\` before QApplication exists. Qt's DLLs are not
  modified. The spike held the M0 criterion in 132 of 132 cells. The launcher's B1 cost is +0.1 to +1.0 ms
  offscreen, and on screen it measured -1.9 ms, which is within noise (below).
- **The GPU driver's MAX_PATH crash** (found by the review of 4c5533d). With Qt's native platform plugin, the GUI
  and `--selftest=onscreen` died from an image path of 260 units (MAX_PATH) on. The cause is a fail-fast
  (0xC0000409) in the DllMain of nvldumdx.dll, NVIDIA's user-mode driver loader. A python.exe that creates a
  Direct3D 11 device dies the same way, so the limit is the driver's and applies to any process.
- **The junction restart** (2f40f9f, review fixes b9dd60f). From an image path of 260 units or more, a start
  that may load a GPU driver restarts through `%LOCALAPPDATA%\FrostIDE\i\<hash>`. That is a junction to the
  install directory, which needs no administrator rights, and the launcher waits for the restarted frost and
  returns its status. The review fixes:
  - state honestly what holding the link guarantees;
  - refuse the restart for an elevated frost;
  - keep the restart marker only when it names the launcher's own image file.
- **Matrix row I7 is green**, with six acceptance tests (E5, below).
- **Evidence**, under `E:\FrostBuilds\perf\m0\longpaths-on\`:
  - `run.log` and `scripts\`: the 09:00 measurement with the switch on;
  - `probe\README.txt` (facts 1-32) and `launcher-spike\README.txt` (facts 1-22);
  - `launcher-impl\`: test results, `timing_ab.json` and `review-fixes\`;
  - `launcher-impl\junction\`: the premise, file-system facts and cost probes, and `review-fixes\verify_full.log`,
    the -Full at b9dd60f;
  - `launcher-impl\onscreen\README.txt` and `launcher-impl\b1-onscreen\README.txt`: the on-screen checks.

#### The user's decisions
- About 11:00, of "Approve stripping", "Use the launcher design" and "Neither for now": **"Use the launcher
  design"** (ADR 0012, Context).
- About 14:15, of "Restart via short link (Recommended)", "Use 8.3 short names" and "Known limitation":
  **"Restart via short link (Recommended)"** (ADR 0012, Context).
- They rest on this morning's instruction, "Follow your own recommendation what to do. The path limit should be
  no problem now, I've enabled long paths" (ADR 0011).
- At about 14:35 the user also said: "You may run continuously until I stop you. You don't have to ask for
  permission to do any window testing etc. I will not be on my PC for a while possibly until tomorrow so please go
  ahead finishe M1, start M2 when M1 is finished and so on". That answers G1's separate question about starting
  M1.

#### On-screen checks on the user's desktop
- **14:20-14:26, approved by the user at the PC** (the Release launcher of 33a329a):
  - The driver's limit (`launcher-impl\onscreen\README.txt`). From a 210-unit image path the GUI showed its
    window and `--selftest=onscreen` passed on 'windows'. From 310 units both ended with 0xC0000409 after 0.1-0.3
    s, without a window. The python.exe Direct3D 11 control, started through `\\?\`, created a device at 246 and
    251 units and died at 260, 271 and 311.
  - B1 on screen (`launcher-impl\b1-onscreen\README.txt`), with `s1_first_frame --mode warm --runs 15` and the
    rounds interleaved. The single-image frost.exe had p50 197.5 ms and the launcher p50 195.6 ms, 24 valid runs
    each. The launcher has no measurable B1 cost on screen (-1.9 ms, within noise). **B1 warm still fails**: its
    target is p50 150 ms. It stays M1's target (ADR 0011, A3).
- **About 16:30, while the user was away**, under the 14:35 permission (the Release build of b9dd60f;
  `launcher-impl\onscreen\README.txt`, `restart_fg.json`, `restart_fg2.json`):
  - The restart is confirmed in the foreground. The runs used CreateProcessW, as a terminal or a script starts a
    program, and alternated 210 and 310 units, twice each. In every run a Qt window was visible within about 0.2
    s of the start, and it was the foreground window. From 310 units the window belonged to the restarted
    process. One junction was created and then reused, and the launcher exited 0 after WM_CLOSE. So
    AllowSetForegroundWindow is not needed on this path. The script of these runs was not saved; the README
    records their method.
  - The shell's limit, partly inferred. One direct ShellExecuteW of the 310-unit frost.exe returned 5: no window
    and no junction appeared. Inferred, not measured: that the path's length causes the 5 (no shorter-path
    control was run), and that Explorer and shortcuts, which start programs through the shell, fail the same way
    (neither was tried). A terminal or script launch works. If the inference holds, this is a residual limit of
    Windows, not of frost; ADR 0012's Consequences record it. Only 310 units was tried. The `.lnk` run shows
    less: the shortcut could not be given the 310-unit target in the first place.

#### E4 closed, E5 and G3 done
- **E4: closed without merging `m0/e4-asan-runtime`.** Its ASan case now passes for a real reason. The runtime
  is no longer a static import of frost.exe, and the launcher loads it through `\\?\`. 4c5533d replaced
  `PlatformPluginPathBoundary` with tests that assert at least as much, and they pass in the ASan tree (419/419
  at b9dd60f). Stripping the runtime's manifest also belonged to "Approve stripping", the option the user passed
  over. The branch stays pushed at 19157a8 (ADR 0011, E4's outcome).
- **E5: done.** I7 is `green`, with `FrostProcess.SelfTestPasses`, `StartsFromEveryDirectoryLength`,
  `StartsWithLongPathsIneffective`, `RunsFromPathLongerThanMaxPath`, `SelfTestOnScreenFromEveryImagePathLength`
  and `GuiStartFromALongPathShowsItsWindow` as its acceptance tests.
- **G3: done.** The comment-accuracy round over the E1 fix is `m0/longpath`'s reviews, whose findings are fixed
  in 33a329a and b9dd60f. The refusal message with the LongPathsEnabled advice is gone.

#### Verification
- `m0/longpath` at b9dd60f, `tools\verify.ps1 -Full -Jobs 4` on a hidden desktop: win debug 415/415, win release
  418/418, win asan 419/419, wsl debug 335/335, wsl asan 339/339, wsl tsan 338/338, wsl licensecheck ok, format
  ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run.
- After each merge, on the merged tree: frost_lint ok (280 files, then 287), licensecheck ok, featurecheck ok, the
  tools tests 80/80 and `git diff --check` clean.
- This exit record (docs only): frost_lint ok, featurecheck ok, licensecheck ok, `git diff --check` clean.
- `verify -Full` at `m0/foundation`'s final head (the sign-off): <!--FULL-->

### Next
- The sign-off (ADR 0011, G1). If `-Full` at the final head is green, record its footer in one more docs-only
  commit ("M0: sign-off record"), tag that commit `m0` (annotated, with the `-Full` footer and a note that
  `-Full` ran on its parent), create `main` from it, and push `m0/foundation`, `main`, `m0/decisions`,
  `m0/longpath` and the tag.
- **Next: M1.** It is already under way. `m1/piece-tree` has frost::text's piece tree (ADR 0013, three commits,
  not merged). `m1/storage`, `m1/input-keymap` and `m1/theme-icons` have worktrees but no commits yet, and
  `m1/diff-merge`, `m1/encodings` and `m1/undo-anchors` have worktrees on `m1/piece-tree`'s head (6fb6826),
  created at about 16:45 (the state then).
- M1 work from the decisions is listed in ADR 0011's Consequences (A1-A4, B1, D1, D2, E3, F2, G3). ADR 0012
  adds its TODO(M1)s in `main.cpp`: code that starts frost again, writes logs or dumps, or registers frost must
  take the install directory from `restartedFrom`; an explicit AppUserModelID; and logging a failed
  non-platform preload in the GUI.
- ADR 0012's TODO(M2): sweep stale short links.

### Blockers
- None for M0 apart from the `-Full` run at the final head.
- B1 warm fails: about 195 ms on screen against p50 150 ms. That is M1 work (ADR 0011, A3), not an M0 exit
  criterion.

### Questions for the user
- **The restart's cost: about 10 ms, against the "about 5 ms" in the option you chose.** The real
  `--selftest=onscreen` from 300 units pays +8.5 to +10.3 ms (paired p50 +9.0 and +9.7 ms). The stand-in without
  Qt pays +6.4 to +7.8 ms, about one more launcher start. Only GUI and on-screen starts from an image path of 260
  units or more pay it. No budget covers it. Is that acceptable, or should it be reduced? (ADR 0012,
  Measurements, "The short link")
- **An elevated frost from 260 units or more refuses to restart.** A frost started with "Run as administrator"
  from such a path exits with status 4 and a message. Programs of the same user that run without elevation could
  retarget the junction and so choose the code an elevated frost loads, which is the usual junction-based UAC
  bypass. So an elevated GUI start from a path that deep does not work. Is that acceptable? The alternatives (a
  DACL-protected link, an administrator-owned folder) and their costs are in ADR 0012's Alternatives. The refusal
  is not tested automatically, because no test can elevate without a prompt.

## 2026-11-01 (morning): M0 decisions

At about 09:10 the user answered the M0 questions (A1–G1 in the entry below, and E5, G2 and G3 from the demo
pack) with one instruction: "Follow your own recommendation what to do. The path limit should be no problem now,
I've enabled long paths". Every question is decided as recommended, on that instruction. Nine questions had an
ADR's recommendation (A1, A2, B2, B3, B7, C1, C3, D2 and F3); for the other 19 the session wrote its own into its
decision list after the instruction (ADR 0011, Context). ADR 0011 records each decision with its basis, its
consequence and where it is implemented. The work is on branch `m0/decisions`, from `m0/foundation` at 593e15f;
it is not merged and not pushed.

### Done
#### The decisions in short (ADR 0011)
- **A, renderer and first frame** (ADR 0007, now accepted). QRhi is M1's editor renderer on Windows; raster stays
  the golden oracle and the RDP/WARP fallback, and Linux keeps raster (A1). B6's missed and interval criteria are
  gated at M1, after a quiet-desktop control and PresentMon's present mode (A2, option (c)). B1 keeps 150/250 ms
  (A3). Cold B1 is measured after reboots, through a logon harness that M1 builds (A4).
- **B, ProjectHost and B23** (ADRs 0005 and 0006).
  - B1: option (i). Each ordinary reboot yields one cold S6 sample through the same harness. D3 stays provisional
    until 10 samples exist and the user has fixed B23's first-project value from them; 3 s stays the ceiling. M2
    depends on D3, so the samples are needed before M2.
  - B2: the trees target (1.5 s) stays provisional until M2's first trees measurement on R3.
  - B3: all five B23 preconditions of ADR 0006 decision 6 are in `budgets.json`.
  - B4: ProjectHost before L1 in a trusted root, with evaluation kept off the network until L1, is M2's target
    (answer 2). Waiting for L1 is the fallback; changing PLAN.md's network rule is rejected.
  - B5: the UWP workload waits for M2 (installer command below).
  - B6: S2's files in `%LOCALAPPDATA%\vcpkg` are deleted after a check: `downloads\` and `registries\` went at
    09:55; `archives\`, which is not S2's, is kept (below).
  - B7: R2b's Windows SDK prerequisite is 10.0.26100.0.
- **C, clangd index** (ADR 0008, now accepted). B24's M3 value of p50 18.4 s is confirmed, and the M3 gate runs
  on a quiet machine (C1). The corpus manifest pins R3's configuration (C2). C3 is noted, with no decision.
- **D, Tracy** (ADR 0009, now accepted). The dev-builds-only listener exception is granted under ADR 0009's
  conditions (D1, option (a)). Budget and perf runs never start Tracy (D2). PLAN.md's dependency entry has the
  new wording (D3).
- **E, start-up and paths.**
  - E1: the switch does not lift the limit (below). frost is to start from every directory length with the switch
    on or off; that is the `m0/longpath` work.
    - **Done** (afternoon; ADR 0012, merged in 2d824ce): met, through the launcher and the short-junction restart,
      on the user's decisions "Use the launcher design" and "Restart via short link (Recommended)". See the entry
      "2026-11-01 (afternoon): M0 complete".
  - E2: in M2, trusted user-script children get NoDefaultCurrentDirectoryInExePath stripped; restricted children
    keep it.
  - E3: in M1, frost::io refuses non-writable POSIX targets, and the SaveManager offers an explicit overwrite.
  - E4: if the E1 fix makes `PlatformPluginPathBoundary`'s ASan case pass, or makes it moot, `m0/e4-asan-runtime`
    is not merged and stays pushed for the record. Otherwise it is merged (option (d)).
    - **Closed** (afternoon; ADR 0011, E4's outcome): the launcher does both, so `m0/e4-asan-runtime` is not merged
      and stays pushed at 19157a8. See the entry "M0 complete".
  - E5: once the E1 fix lands, matrix row I7 gets `FrostProcess.SelfTestPasses` and the long-path launch tests
    as its acceptance tests.
    - **Done** (afternoon; ADR 0011, E5's outcome): I7 is `green` with six acceptance tests. See the entry "M0
      complete".
- **F, machine and tooling.** No action on G HUB (F1). No Linux sccache; M1 caches or avoids the FindOpenGL
  searches instead (F2, TODO below). B28 reads the full default build, and its method fixes the probe files and
  the job counts (F3; ADR 0010 decision 2 accepted).
- **G, sign-off.** M0 is signed off once `verify -Full` is green on both platforms at `m0/foundation`'s final
  head and every item is recorded. Then that commit is tagged `m0` (annotated, with the `-Full` footer), `main` is
  created from it, and both are pushed. Starting M1 is a separate question (G1). ADR 0002 is accepted (G2). One
  more comment-accuracy round covers the `m0/dll-copy` build code (done: round 5, below) and the E1 fix
  (`m0/longpath`'s review), and the POSIX strerror text gets a TODO(M1) (G3).
  - **Afternoon:** G3's round over the E1 fix is done (`m0/longpath`'s reviews, fixed in 33a329a and b9dd60f).
    G1's conditions are met apart from `verify -Full` at the final head, which the sign-off runs before it tags
    `m0` and creates `main`. The user answered the M1 question at about 14:35. See the entry "M0 complete".

#### Applied on `m0/decisions`
- `docs/adr/0011-m0-decisions.md` (new).
- ADR statuses, each with a "Decided 2026-11-01 (ADR 0011)" note beside the original text, which is kept: 0002
  (accepted); 0006 (decisions 2, 3 and 6, and the trees proposal); 0007 (decision 1, question 2's option (c),
  questions 3 and 4); 0008 (decisions 1 and 2, the R3 pin); 0009 (the exception, so items 2–9; item 3's rule; the
  wording); 0010 (decision 2).
- PLAN.md: B23's row note (B2) and the S6 spike-list line (B1, B2), the Tracy wording (D3), R2b's Windows SDK in
  section 4 (B7), and in the M0 spike list the S7 sentence, now confirmed (C1), and the Tracy version line, now
  0.14.1 (D3, ADR 0009). Nothing else.
- `tools\env.ps1`: `X_VCPKG_REGISTRIES_CACHE` sends vcpkg's registry clones to `<SDK root>\vcpkg-registries`,
  created up front like the other caches (B6). `tests/tools/test_windows_scripts.py`'s cache test checks it, and
  it now clears the variables before it dot-sources `env.ps1`, so a value inherited from the calling session
  cannot pass. Against the old `env.ps1` the test fails ("X_VCPKG_REGISTRIES_CACHE= is not under E:\sdk"), also
  with the variable set in the calling session.
- `tests/perf/budgets.json`: B23's five preconditions and its trees note, B24's M3 note (now confirmed) and
  B28's method. No target value changed. `plan_target` of B23 follows PLAN.md's new cell, as
  `tests/tools/test_budgets.py` requires.
- `tests/corpus/manifest.json`: R2b's SDK prerequisite, and an R3 `configuration` with S7's configure command
  (placeholders for the checkout and the SDK root). The manifest test in `tests/tools/test_corpus_fetch.py` now
  asserts both pins; against the old manifest it fails.
- Comments only: the TODO(M2) on `ProcessSpec::environment` records E2's decision; new TODO(M1)s at
  `writeFileAtomic`'s read-only paragraph (E3) and at `errnoError`'s strerror text (G3).

#### Comment-accuracy round 5: the `m0/dll-copy` build code (G3, first part; bc42ada)
- Covered `cmake/FrostLayers.cmake`'s runtime-DLL pass, `tests/cmake/check_runtime_dlls.cmake` and its fixture
  (`tests/cmake/runtime_dlls_fixture/`). 9 findings, all applied in bc42ada, comments only: no CMake command,
  argument or test step changed.
  - `FrostLayers.cmake` (6): the header names the runtime-DLL pass (`frost_copy_runtime_dlls` and the deferred
    `_frost_add_runtime_dll_copies`) and says its output-directory checks run on every platform; the race first
    failed in the merged tree's verify (87f376a, the merge of `m0/s1`), and 51e9a05 was the stopgap; vcpkg's
    installed `bin` directory does not exist while the triplets link statically (it is not "empty"); the copy
    target's name; the cost note cites `Budget: B28.`; CMP0112 covers `$<TARGET_FILE_DIR>`, and
    `$<TARGET_RUNTIME_DLLS>` adds no dependency either; `frost_add_test`'s doc (TEST options and
    `frost_copy_runtime_dlls`).
  - `check_runtime_dlls.cmake` (2): the same race reference; steps 4 and 5 claim only what the script checks.
  - The fixture's `shared/CMakeLists.txt` (1): four executables in all.
- Verified (bc42ada's footer): `tools\verify.ps1 -Quick -Jobs 4` **FAILED** in 2 tests: win debug 365/368
  (1 skipped, 2 failed: `FrostProcess.RunsFromPathLongerThanMaxPath` and `FrostProcess.PlatformPluginPathBoundary`,
  the same two LongPathsEnabled=1 failures as 7fb5a27), `FrostRuntimeDlls.CopiesOncePerDirectory` passed, wsl
  debug 334/334, wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok; frost_lint ok
  (280 files), `git diff --check` clean, the diff holds comment lines only.
- Left for G3: the round over the E1 fix, which is `m0/longpath`'s review.

#### Long paths with LongPathsEnabled = 1 (E1, E4)
- Measured at 09:00 with the switch on, by the method of the `-Full` finding (`exit-full\scripts`), on the
  Release, Debug and ASan builds of frost.exe from `-Full` at 2c499a2. ADR 0011 has the table. Log:
  `E:\FrostBuilds\perf\m0\longpaths-on\run.log`; the scripts and `results.jsonl` are in its `scripts\`.
- **The switch does not lift the limit.** Every DLL that embeds RT_MANIFEST 2 still fails to load beyond 250
  UTF-16 units, at the same lengths as with the switch off.
- **frost is now worse on this machine.** Its start-up check trusts the switch and no longer refuses:
  - Release: at 225 units the self-test passes. From 226 to 235 units (the plugin at 251–260) the run times out
    at 60 s: Qt fails to load the plugin and waits on its message box (on the hidden desktop). From 236 to 249
    units frost ends with 0xC0000106 before `main`, as before. At 250 (the `\\?\` launch) it times out again.
  - Debug: the same pattern one unit earlier, with 0xC0000409 (Qt's qFatal) instead of the timeouts: 224 passes,
    225–234 and 250 abort, 235–249 end with 0xC0000106.
  - ASan: 0xC0000106 from 218 to 249 units, so `PlatformPluginPathBoundary` (225 and 226 units) still fails in
    the ASan tree; at 250 it times out.
  - The ranges are sampled at their ends only.
- With the switch off, frost refused cleanly (exit 1) from 226 units (Release) or 225 (Debug) to the start of the
  0xC0000106 range, and from 250 on. Until the E1 fix lands, frost started from such a directory here shows a
  dialog or aborts, and the refusal message's advice to turn the switch on is wrong (G3: the E1 fix replaces it).
- `FrostProcess.RunsFromPathLongerThanMaxPath` no longer skips, and `ReportsLongPathLimitCleanly` now skips: both
  follow the switch. So the Windows legs of `verify` run the long-path tests against the new behaviour (see
  Verification).

#### B6: S2's vcpkg files deleted from C:; `archives\` kept
- Listed recursively (creation and modification times) on 2026-11-01: 4,204 files in 110 directories,
  558,106,723 bytes (532 MiB): `archives\` 6,292,779 bytes (3 files), `downloads\` 415,726,990 (1,040) and
  `registries\` 136,086,954 (3,161).
- No running process has its executable there or names the directory on its command line. Open handles were not
  enumerated: no handle tool is installed.
- Every entry was created on 2026-10-31, between 18:52:29 and 20:55:17. But 1,035 files and 3 directories have
  older modification times, back to 2018-01-28. All of them are in `downloads\tools\powershell-core-7.5.3-windows`
  (928 files, and 3 directories under `Modules\`), `downloads\tools\7zip-25.01-windows` (106 files) and
  `downloads\tools\ninja` (1 file), which vcpkg extracted at 20:55 from the archives it had just downloaded; the
  extraction kept the archives' timestamps (ADR 0005 says the same). The first count, "1,038 files" (also in
  7fb5a27's message), counted the 3 directories as files.
- `archives\` (18:52) holds binary-cache packages of gtest, vcpkg-cmake and vcpkg-cmake-config, FrostIDE's own
  dependencies. ADR 0005 says it predates S2's run and is not from it; most likely a FrostIDE configure without
  `tools\env.ps1` wrote it (not traced).
- Read by modification time, the check failed, so at first nothing was deleted. By creation time, `downloads\`
  (created 20:55:09–20:55:17) and `registries\` (created 20:54:53–20:55:08) both match S2's TerminalAppLib
  reference build (20:54:53–20:55:17 on 2026-10-31, ADR 0005).
- **Deleted at 09:55:** `%LOCALAPPDATA%\vcpkg\downloads` (1,040 files, 415,726,990 bytes, 396.5 MiB) and
  `%LOCALAPPDATA%\vcpkg\registries` (3,161 files, 136,086,954 bytes, 129.8 MiB).
- **Kept:** `%LOCALAPPDATA%\vcpkg\archives` (3 binary-cache zips, 6,292,779 bytes, 6.0 MiB, created
  18:52:29–18:52:47), because its origin is uncertain. It is all that `%LOCALAPPDATA%\vcpkg` now holds.
- **Third-party builds must run under `tools\env.ps1`** (dot-sourced), which sends vcpkg's downloads
  (`VCPKG_DOWNLOADS`), binary cache (`VCPKG_DEFAULT_BINARY_CACHE`) and, since this branch, registry clones
  (`X_VCPKG_REGISTRIES_CACHE`; unset, vcpkg uses `%LOCALAPPDATA%\vcpkg\registries`) to E:. Whether S2's run had
  `env.ps1` active is not recorded.

#### B5: the installer command for M2 (not run)
The user runs it from an elevated prompt when M2 evaluates TerminalAppLib:

```
"C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" --add Microsoft.VisualStudio.Workload.UniversalBuildTools --add Microsoft.VisualStudio.ComponentGroup.UWP.VC.BuildTools --passive --norestart
```

- The IDs come from the installed instance's catalog (VS 2022 Build Tools 17.14.37710.0, instance 39ecdf9a) on
  2026-11-01. The workload is titled "WinUI application development build tools" there, and the component group
  "C++ (v143) Universal Windows Platform tools". The group brings C++/WinRT
  (`Microsoft.Windows.CppWinRT.Dev17`) and the ARM, ARM64 and ARM64EC compilers. The download size was not
  measured.
- TerminalAppLib sets `ApplicationType=Windows Store` (ADR 0005). Whether these two are enough for it is checked
  in M2; re-read the IDs from the catalog then, because the installer may have moved on.

#### F2: TODO(M1)
- TODO(M1): cache or avoid the Linux configure's two FindOpenGL `find_path` searches for `GL/glu.h` and
  `GL/xmesa.h` (about 15 s of each configure; ADR 0010's follow-up). First test whether the 38 `/mnt` PATH entries
  make them slow.

#### Verification
- Checks in the worktree: frost-lint ok (280 files), featurecheck ok, `git diff --check` clean (staged tree), the
  tools tests 80/80 (frost_lint, licensecheck, featurecheck, corpus_fetch, budgets, windows_scripts), and
  clang-format on the three touched C++ files.
- `tools\verify.ps1 -Quick -Jobs 4` in the worktree after `night.py build-gate` (09:33–09:36, 164 s, a fresh
  build tree): **FAILED** on two Windows tests; everything else is green. `FAILED: Verified: win debug 365/368
  (1 skipped), wsl debug 334/334, wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok,
  tidy not-run`.
  - The failures are `FrostProcess.RunsFromPathLongerThanMaxPath` and `FrostProcess.PlatformPluginPathBoundary`,
    both 0xC0000409 with Qt's "no Qt platform plugin could be initialized". The skip is now
    `ReportsLongPathLimitCleanly`. All three follow LongPathsEnabled: until today the first skipped and the second
    passed in this Debug tree. It is the switch-on behaviour measured at 09:00 (above), not this branch, which
    changes only comments in `src/`.
  - The same filter run from `m0/foundation`'s own Debug build (`E:\dev\FrostIDE\build\win-clangcl`, built by
    `-Full` at 2c499a2) fails the same two tests the same way. So `verify -Quick` is red on this machine for
    every branch until the E1 fix (`m0/longpath`) lands. This branch does not touch `src/app` or `tests/app`.
- The follow-ups to the decisions check (ADR 0011's Context, B1, B6 and G3, round 5's record, `env.ps1` and its
  test, PLAN.md's S7 and Tracy-version lines): frost-lint ok (280 files), licensecheck ok, featurecheck ok,
  `git diff --check` clean, and the tools tests 80/80 (`python -B -m unittest discover -s tests/tools`, with
  `tools\env.ps1` dot-sourced). No C++ changed, so nothing was built.

### Next
- `m0/longpath` (another workflow): the E1 fix, whose review is G3's comment-accuracy round over it. Then, in
  order: E4's outcome; I7's acceptance tests (E5); `verify -Full` on both platforms at `m0/foundation`'s final
  head; then G1 (tag `m0`, create `main`, push both).
- Merge `m0/decisions` after its review. PLAN.md's M0 status paragraph and next-step line are left for the final
  integration.
- M1, M2 and M3 work from the decisions: ADR 0011, Consequences.
- **Afternoon:** `m0/longpath` (E1, E4, E5) and `m0/decisions` are merged (2d824ce, 52809c6), and PLAN.md's two M0
  lines are updated. What remains is `-Full` at the final head, then G1 (entry "M0 complete").

### Blockers
- **Afternoon:** the E1 fix has landed (entry "M0 complete"). That resolves the second blocker below and the E1
  part of the first; the sign-off still waits on `-Full` at the final head.
- Sign-off waits on the E1 fix (`m0/longpath`) and on a green `verify -Full` (G1).
- With the switch on, frost started from a directory of 226–235 or 250+ units shows Qt's dialog or aborts, until
  the E1 fix lands. For the same reason `verify -Quick` on Windows fails two long-path tests on every branch here
  (Verification), so no branch meets CLAUDE.md's definition of done until then.

### Questions for the user
- None from this entry.

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

**All of them were answered on 2026-11-01**, on the user's instruction to follow the recommendation: each is
decided, except C3, which is noted with no decision. G1 decides the sign-off's conditions; M0 is not signed off
yet. See ADR 0011 and the entry "2026-11-01 (morning): M0 decisions" above. Each question below keeps its text,
with a pointer.

**A. Renderer and first frame (S1, ADR 0007)**
- **A1.** Approve QRhi as M1's editor renderer on Windows? Raster stays as the golden oracle and the RDP/WARP
  fallback; Linux keeps raster until a Linux S1 leg runs. Raster fails B6's 5 ms UI-CPU limit by its own cost in
  every round (p99 6.3-9.2 ms at 5120x1392). QRhi passes with p99 below 1 ms, but its start-up adds about 210 ms to
  the first show().
  - **Decided 2026-11-01** (ADR 0011, A1): approved.
- **A2.** How should B6's missed-refresh and interval criteria be gated?
  - (a) a quiet desktop with the absolute budget;
  - (b) the whole output (5120x1440) with the absolute budget: QRhi fill passed all three criteria there in one
    round;
  - (c) decide at M1, after a quiet-desktop control and PresentMon's present mode for QRhi. ADR 0007 proposes (c),
    with no budget change.
  - **Decided 2026-11-01** (ADR 0011, A2): option (c), with no budget change.
- **A3.** B1 warm already fails before any editor work exists: frost.exe p50 264.9 ms against 150 ms, and the max of
  20 launches is 360.1 ms against 250 ms. Keep 150/250 ms as M1's target, or re-baseline?
  - **Decided 2026-11-01** (ADR 0011, A3): keep 150/250 ms; no re-baseline.
- **A4.** A true cold B1 measurement needs either a reboot (first-frame steps run first after logon) or an elevated
  RAMMap -Et before each launch. Which, and when?
  - **Decided 2026-11-01** (ADR 0011, A4): the reboot method, through a logon harness that M1 builds.

**B. ProjectHost and B23 (S2/S6, ADRs 0005 and 0006)**
- **B1.** How is B23's "first project streamed" final value (R2b, p50 3 s) set, and with it when D3 becomes final?
  The spike's ProjectHost-side lower bound is p50 1.90-2.02 s, warm file cache.
  - (i) You take 10 cold samples for S6, each after a reboot or an elevated standby-list purge. D3 waits for them,
    which keeps PLAN's order.
  - (ii) M2's cold B23 test sets the value. Your part moves to M2. M2 builds the C# ProjectHost before B23 is fixed,
    which inverts PLAN's "S6 fixes B23 before D3 is final": a plan change to PLAN's S6 line and D3 row.
  - Under either option, 3 s stays the ceiling unless an ADR with your approval raises it.
  - **Decided 2026-11-01** (ADR 0011, B1): option (i), with the samples collected passively, one per ordinary
    reboot.
- **B2.** B23's trees target (R3, 1.5 s) stays provisional until M2's first trees measurement. That is also a change
  to PLAN's B23 row ("final value set by S6").
  - **Decided 2026-11-01** (ADR 0011, B2): accepted.
- **B3.** B23's proposed preconditions (ADR 0006 decision 6), to be written into budgets.json if you approve:
  - packages restored;
  - Debug|x64;
  - the first project chosen by decision 5's rule (Host.EXE);
  - trust granted at the open request;
  - the gating sample cold.
  - **Decided 2026-11-01** (ADR 0011, B3): all five, now in `budgets.json`.
- **B4.** Whether ProjectHost may start before L1 in an already-trusted root (ADR 0006 decision 2 gives three
  answers). It would put about 2 s of host work beside the tree fill and run project MSBuild logic before L1
  (PLAN's no-network-before-L1 rule).
  - **Decided 2026-11-01** (ADR 0011, B4): answer 2 is M2's target, answer 3 the fallback, answer 1 rejected.
- **B5.** Installing Visual Studio's "Universal Windows Platform development" workload, needed to evaluate R2b's
  TerminalAppLib (C++/WinRT).
  - **Decided 2026-11-01** (ADR 0011, B5): deferred to M2; the installer command is in the entry above.
- **B6.** About 552 MB of third-party downloads that vcpkg left on C: during the spikes: keep or delete?
  - **Decided 2026-11-01** (ADR 0011, B6): delete after a check. S2's `downloads\` and `registries\` were
    deleted at 09:55; `archives\`, which is not S2's, is kept (entry above).
- **B7 (new).** May PLAN.md section 4 and `tests/corpus/manifest.json` give R2b's Windows SDK prerequisite as
  10.0.26100.0 instead of 10.0.22621? The pinned R2b commit defaults to 10.0.26100.0, and `sdk/sdk.lock.json` pins
  it (ADR 0005, Consequences; ADR 0006, follow-ups). Neither file was changed.
  - **Decided 2026-11-01** (ADR 0011, B7): yes; both files now say 10.0.26100.0.

**C. clangd index (S7, ADR 0008)**
- **C1.** Confirm B24's M3 value of 18.4 s (quiet-machine baseline p50 15.31 s x 1.2; it was 20.93 s under your
  FrostEditor workload) and the requirement that the M3 gate runs on a quiet machine with heavy workloads closed.
  tests/perf/budgets.json already carries max 18.4 s for this target, which had no value before; its note marks it
  as proposed and waiting for you. If you choose another value, it changes.
  - **Decided 2026-11-01** (ADR 0011, C1): confirmed, with the quiet-machine requirement.
- **C2.** Should tests/corpus/manifest.json pin R3's configuration (LLVM_ENABLE_PROJECTS=clang, Release, Ninja,
  clang-cl) for all R3 budgets?
  - **Decided 2026-11-01** (ADR 0011, C2): yes; pinned.
- **C3.** Budget risks to note, no decision now:
  - B24's M13 target (p50 2 s) is about 10x faster than stock clangd on R3, which loads the whole index on one core
    before answering.
  - A warm clangd on R3 holds 3.16 GiB, most of B26's 4 GB idle budget.
  - Note (new): ADR 0008 says about 8×, from the quiet-machine p50 of 15.3 s; about 10× matches the 20.9 s under
    your workload.
  - **Noted 2026-11-01** (ADR 0011, C3): no decision.

**D. Tracy (ADR 0009)**
- **D1.** Tracy's listener is an unauthenticated 127.0.0.1 TCP port. While it runs, any local process (and, for WSL,
  any Windows process) can read and corrupt the IDE's memory. PLAN 2.2/5.2 require every listener to have a token
  or an ACL.
  - (a) Grant a dev-builds-only exception: started only by an explicit per-run opt-in; on-demand, ONLY_LOCALHOST,
    NO_BROADCAST, NO_CODE_TRANSFER, NO_CALLSTACK, NO_SYSTEM_TRACING (so no call stacks or context-switch capture);
    listener-enumeration and release-build tests.
  - (b) Decline: no Tracy; profiling uses ETW, Perfetto export and external sampling profilers.
  - (c) Defer to M1.
  - **Decided 2026-11-01** (ADR 0011, D1): option (a).
- **D2.** Confirm the rule that budget and perf runs (frost-bench, verify -Perf) never start Tracy. StartupProfiler
  costs about 215 ms on the first-frame path.
  - **Decided 2026-11-01** (ADR 0011, D2): confirmed.
- **D3.** Optional wording: change PLAN.md's "tracy (pinned in M0)" to "tracy (version chosen in M0: 0.14.1 from the
  vcpkg baseline; use pending the listener exception in ADR 0009)".
  - **Decided 2026-11-01** (ADR 0011, D3): applied, adjusted for D1 (a): "use under the dev-builds-only listener
    exception of ADR 0009".

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
  - **Decided 2026-11-01** (ADR 0011, E1): the user enabled the switch, and it does not lift the limit (measured,
    entry above). frost is fixed to start from every directory length, whether the switch is on or off
    (`m0/longpath`); the criterion stays.
  - **Done 2026-11-01 (afternoon)** (ADR 0012; merged in 2d824ce): the criterion is met through the launcher
    ("Use the launcher design") and the short-junction restart ("Restart via short link (Recommended)"). See
    the entry "2026-11-01 (afternoon): M0 complete".
- **E2.** By M2: should trusted user-script children (MSBuild Exec and build events, CMake configure, later
  Build/Run and the terminal) get Visual Studio-compatible cmd.exe behaviour, with
  NoDefaultCurrentDirectoryInExePath stripped? Or keep the hardened behaviour, where a tool in the current directory
  must be called as .\tool? Restricted children keep it either way.
  - **Decided 2026-11-01** (ADR 0011, E2): yes, in M2; restricted children keep the hardened behaviour.
- **E3.** On POSIX, saving over a read-only (0444) file replaces it silently, because rename needs only directory
  write access. Should frost::io or M1's SaveManager refuse or warn on non-writable POSIX targets?
  - **Decided 2026-11-01** (ADR 0011, E3): frost::io refuses with PermissionDenied on POSIX too (M1), and the
    SaveManager offers an explicit "overwrite read-only file" action.
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
  - **Decided 2026-11-01** (ADR 0011, E4): if the E1 fix makes the ASan case pass, or makes it moot, on its own,
    `m0/e4-asan-runtime` is not merged and stays pushed for the record; otherwise it is merged (option (d)).
  - **Closed 2026-11-01 (afternoon)** (ADR 0011, E4's outcome; ADR 0012, Measurements): the launcher makes the ASan
    case pass for a real reason, and 4c5533d replaced the test with stronger ones. `m0/e4-asan-runtime` is not
    merged and stays pushed at 19157a8. See the entry "M0 complete".

**F. Machine and tooling**
- **F1.** Logitech G HUB (lghub_agent.exe) holds about 43 GB of commit. Restarting it would give the builds
  headroom: an -j=8 clangd index ran out of commit tonight.
  - **Decided 2026-11-01** (ADR 0011, F1): no action; the 43 GB was transient.
- **F2.** Linux has no sccache: no pin in sdk.lock.json and bootstrap-wsl.sh installs none. Adding one needs a
  lock-file pin and an ADR. Do you want it?
  - **Decided 2026-11-01** (ADR 0011, F2): no; M1 caches or avoids the FindOpenGL searches instead.
- **F3 (new).** B28's "rebuilt and linked" (ADR 0010, question 1): should it be (a) the full default build after the
  change (proposed; core p50 3.09 s, main 3.33 s, sched 1.16 s), (b) `--target frost` (core 1.58 s, main 3.33 s), or
  (c) the executable or executables that cover the change, such as the changed module's test target? Should
  budgets.json's B28 method also fix the probe files and the job counts? Every reading meets the target at M0, so
  nothing waits on the answer.
  - **Decided 2026-11-01** (ADR 0011, F3): (a), and yes: the probe files and the job counts are fixed.

**G. Sign-off**
- **G1.** M0 sign-off: after it, tag the M0 build, create main from m0/foundation and push it, then start M1.
  - **Decided 2026-11-01** (ADR 0011, G1): signed off once `verify -Full` is green on both platforms at
    `m0/foundation`'s final head and every item is recorded; then tag `m0`, create `main` and push both. Starting
    M1 is a separate question. Not signed off yet.
  - **2026-11-01 (afternoon):** every item is recorded, and E1, E4, E5 and G3 are done. The sign-off runs `-Full`
    at the final head, then tags `m0` and creates `main`. The user answered the M1 question at about 14:35 ("go
    ahead finishe M1, start M2 when M1 is finished and so on"). See the entry "M0 complete".

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
