# ADR 0029: Thin path to a first usable build

- Status: proposed. D29 itself is the user's decision, quoted below. The sub-decisions under "Open questions" go
  beyond the user's answer, and PLAN.md section 6.7 asks before any row moves or slice is redefined, so they go to
  the user before this ADR is accepted and its branch merges.
- Date: 2026-11-01; revised 2026-11-01 after the independent review (B23's path (c) gate, the U1 scenario's
  setup, recorders and steps, trust for Build, Run and the terminal, the MSBuild environment, the display-map
  layers, the estimates, the P1 rows kept before Preview 0.1, the checks)
- Plan reference: PLAN.md section 7 (roadmap, "First usable build (U1, the M4 exit)", M15–M17), Appendix A, section 4
  (B15, B23), section 1.4 (P0 rows after Preview 0.1), sections 3.5 and 3.6 (trust, settings), section 9 (D29);
  `tools/featurecheck.py`, `tools/frost_lint.py`, `tests/tools/test_budgets.py`

## Context
The user was asked: "Do you want a thin 'usable for real C++ work sooner' path (basic editor, CMake, clangd, build
and run) ahead of the full M1 polish?" The answer (2026-11-01, evening) was: **"Yes I want something usable before a
fully polished version"**. It is recorded as D29.

The user's wider goals stay as PLAN.md section 1 states them: a Rider-like C++ IDE with full MSVC .sln/.slnx/.vcxproj
support and CLion-grade CMake, on Windows 11 and Linux, very fast, with Claude Code integration.

The plan before this ADR built each layer completely before the next: M1 (editor, shell and foundations, 11–13 units),
M2 (project models, first run and trust, 12–14), M3 (C++ intelligence, 10–12, marked "first usable checkpoint"
without build, run or a terminal) and M4 (build, run, terminal and the CMake language, 9–11). Real C++ work, which
needs build, run and a terminal, became possible only at the M4 exit, around month 12–15, after 45–54 units.

State when this was decided: M0 is signed off; M1's `m1/piece-tree` and `m1/input-keymap` are merged into `main`;
`m1/storage`, `m1/theme-icons`, `m1/undo-anchors`, `m1/encodings`, `m1/diff-merge`, `m1/io-reactor-watch`,
`m1/crash-diag`, `m1/verify-tooling` and `m1/keymap-de` are running and will land. Since then `m1/undo-anchors`
(merge 3163d0b) and `m1/theme-icons` (merge 94ed47d) have been merged into `main`.

Constraints set with the decision:
- Scope items and exit criteria move between milestones; none is dropped. No test, budget or warning is weakened: a
  criterion moved to a later milestone is still required there.
- Data safety (SaveManager, the hot-exit journal and their fault injection) and security (the trust model before any
  project code runs, listener hardening, the no-network test) stay in or before the first usable build.
- The speed budgets of everything the usable build includes stay gated where the feature lands.
- Milestone IDs are identities: `TODO(Mn)` markers, ADRs, `matrix.json` and `budgets.json` use them, so they are not
  renumbered. CLAUDE.md does not change.

## Decision
1. **The first usable build (U1) is the M4 exit.** It is the smallest FrostIDE with which the user does real C++ work
   on a CMake project and on an MSVC .sln/.slnx/.vcxproj solution: open the project, browse it, edit with
   highlighting, data safety and the IntelliJ keymap, get clangd completion, diagnostics and navigation, build, open
   build errors from the Build window, run, and use a terminal, which is also how Claude Code runs until the C1 bridge
   (M5). PLAN.md section 7, "First usable build", lists what U1 contains under Open, Browse, Edit, Data safety,
   Intelligence, Build and run, Terminal, Fast and Safe.
2. **U1 has a scripted end-to-end acceptance scenario** on the existing corpora (`tests/e2e/u1`, frost-bench through
   the automation endpoint, a clean profile). PLAN.md section 7 gives the steps; their shape:
   - **Setup outside the scenario,** once per corpus pin: R3's build directory `E:\corpus\R3-build` is reconfigured
     from `E:\corpus\R3\llvm` with a toolchain file outside every worktree (its cache names a removed worktree's),
     with FrostIDE's `client-frostide` query, and `not` is built with its dependencies (`bin\not.exe` is absent
     today); on Linux an ext4 checkout `~/corpus/R3` gets a pinned Linux configuration (none is pinned today). The
     clean profile is seeded with an application-scope CMake profile naming the build directory (llvm's
     CMakePresets.json gives none) and R3's persisted clangd index, so Find Usages and Switch Header/Source do not
     depend on a cold index.
   - **Recorders:** the no-network recorder and the listener enumeration run throughout and assert on FrostIDE's own
     process tree (frost, its crash handler, the engines and tools it supervises). The trees that Build, Run and the
     terminal start are the user's code (MSBuild nodes, `mspdbsrv.exe`, Notepad++, the shell): recorded, not
     asserted.
   - **CMake on R3:** open `R3\llvm` in restricted mode (the target tree from the existing File API replies), trust;
     navigate `utils/not/not.cpp`; Find Usages and Switch Header/Source once L2 is reached; complete `llvm::outs()`
     at the top of `main`, before the early `return 1` of a run without arguments; inject an error; kill frost and
     get the dirty buffer back from the hot-exit journal; build (Save All writes the buffer first), open the error
     from the Build window, fix, build; run `not` and see the injected output and exit code 1; revert the file from
     the terminal and see the clean buffer reload; exit and reopen within B3 and B2.
   - **MSVC on R2a:** open `PowerEditor\visual.net\notepadPlus.sln` in restricted mode, trust the root (the git work
     tree, which holds Scintilla and Lexilla), check clangd on `EncodingMapper.cpp`; select Debug|x64; inject an
     error, open MSVC's C2065 from the Build window, fix, build Notepad++, Scintilla and Lexilla; run Notepad++ with
     its settings in a scratch directory and stop it through the Job Object.
   - **Terminal:** the M4 Claude TUI probe, with its pre-seeded probe configuration and no prompt submitted.
     **Linux:** R2a's solution in approximate mode.
   - It records B1, B2, B3, B8, B9, B10, B11, B17 and B20 samples (2x-budget smoke) and ends with
     `git status --porcelain --untracked-files=no` empty for each corpus and only the root's `.frost/` untracked.
3. **M1–M4 shrink to their thin cores, and their deferred halves move.** M1's go to the new milestone M15, M2's and
   M4's to M16 (the TODO window to M15), M3's to M17. The LLVM SDK artifacts go to M6, where the debugger first needs
   them. No P0 or P1 row moves past Preview 0.1: the Rider-parity P1 rows that U1 does not need (rulers 252,
   clipboard history 254, drag and drop 255, the TODO window 115) go to M15, not to M11. The table below lists every
   move.
4. **Order.** M0, M1, M2, M3, M4 (U1), M5 (dogfood v0.1), M15, M16, M17, M6 (Preview 0.1), then M7–M14. M5 comes
   before the completion milestones because Git and the Claude C1 bridge are the next most valuable things to a U1
   user (open question Q3). M15–M17 come before M6, so every P0 row still lands by Preview 0.1 (PLAN.md section 1.4)
   and the five P0 rows that already finished after Preview 0.1 (31, 99, 118, 227, 242) stay the only ones. With two
   agent streams (PLAN.md section 7, Levers), M15 and then M17 can run alongside M5; M16 needs both M5 and M15.
5. **New milestone IDs M15, M16 and M17.** They are positions in the ladder only by section 7's order, not by number.
   `featurecheck.py` and `frost_lint.py` accept M0..M17 (`LAST_MILESTONE = 17`), and `tests/tools/test_budgets.py`
   accepts gates M0..M17; a to-do marker or a matrix owner M18 is still rejected. featurecheck now reads the run order
   from PLAN.md's milestone ladder (whose rows are in run order), requires it to list each of M0..M17 once, and fails
   a row whose "(+Mx)" names no milestone or one that does not run after the row's owner (so M17 → M7 passes and
   M17 → M15 fails). The budgets test compares gate milestones with the "Gate from" cell as whole IDs in both
   directions (a gate M1 no longer passes against a cell naming M15) and checks that every variant a gate or target
   names is declared. M0 joins frost-lint's `CLOSED_MILESTONES` after its sign-off.
6. **Path (c) is U1's MSBuild evaluator; ProjectHost lands in M16** (open question Q1). `MSBuild.exe -getTargetResult`
   with the wrapper target of ADR 0005 runs the same MSBuild engine and the same design-time targets and global
   properties as ProjectHost (ADR 0005, decision 5), so M2's fidelity exit (every extracted command line compiles with
   `cl.exe /c` and matches msbuild-extractor-sample) applies unchanged, and M16 runs it again on ProjectHost. Path (c)
   was already required in M2 as the fallback, so no throwaway evaluator is built. It starts only after trust and
   after L1, which keeps the no-network-before-L1 rule without ADR 0011 B4's ProjectHost design. Two additions keep it
   usable for daily MSVC work:
   - **A deterministic environment.** Its `MSBuild.exe` starts from an empty environment
     (`ProcessSpec::cleanEnvironment`): the user's default block (`CreateEnvironmentBlock` without inheritance), the
     cached toolchain environment and the ADR 0011 E2 policy, never frost's own environment. The captured-environment
     fingerprint covers that whole block until M16's EnvironmentVariableRead capture narrows it, so how frost was
     started (WT_SESSION, TEMP, shell variables) cannot turn a warm reopen into a cold re-evaluation. M2 tests that a
     relaunch from a different parent environment keeps every fingerprint valid with zero .NET processes.
   - **Live re-evaluation.** A change to a solution, project or filters file, or to a .props or .targets file in a
     record's import closure, invalidates the affected records, and path (c) re-evaluates them in the background (the
     MSBuild counterpart of CMake auto-reload, row 11).
   B3 (zero .NET processes on a warm reopen) is unaffected: the cache, not the evaluator, decides it.
7. **Interim behaviours in U1**, each replaced later and named in the milestone that replaces it:
   - a large-file guard (M1) in the Qt-free document load service, so it covers every open path, reload after an
     external change, the merge view and hot-exit restore: files of 128 MB or more are refused with a notification,
     a file whose longest line exceeds the view's cap opens read-only, and a reload or merge that would cross the
     threshold keeps the buffer and its journal entry, marked out of date; large-file mode replaces it (M15);
   - settings edited as the JSON scope files, opened from Find Action, validated on save and reloaded live (M1); the
     Settings dialog replaces it (M15), with pages for every key M1–M5 added;
   - the native window frame (M1); the frameless window replaces it (M15);
   - build output parsed from the console by code page and locale-independent anchors (M4); the MSBuild logger adds
     structured events (M16);
   - Run builds the target first, after Save All (M4); the before-launch task list generalises it (M16).
8. **Budgets.** No target changes:
   - B15 (F1 first screen) moves from M1 to M15, with large-file mode, which U1 does not contain (it has the guard).
   - B23's first project streamed is gated twice (open question Q2). At M2 on path (c), U1's evaluator, under the
     five preconditions of ADR 0011 B3, with `MSBuild.exe`'s cold start in place of ProjectHost's and the target in
     force at the M2 exit (3 s, or the final value if the user has set it from the S6 samples by then). At M16 on
     ProjectHost, as defined (ADR 0006, ADR 0011 B1), against the final value. This changes the M2 gate's method,
     which ADR 0006 decision 2 (A) says needs an ADR and the user's approval. ADR 0005 and ADR 0006 measured path
     (c)'s Host.EXE at p50 331 ms with a warm file cache, against 1.90 s for a cold host, so 3 s looks reachable; a
     cold cache was not measured.
   Every other gate stays: B1, B5, B6, B19 (no project), B20 and B21 at M1; B2, B3, B4, B7, B13 (unindexed) and B23
   (trees) at M2; B8–B11, B19 (R3), B22, B24 (M3 value), B25 and B26 at M3; B17 at M4; B14, B18 and B27 at M5; B16 at
   M6; B12 at M7; B13 (indexed), B24 (final) and B26 (REF-L1) at M13; B28 at M0. `budgets.json` records both gates of
   B23 (`"evaluator": "path (c)"` and `"ProjectHost"`) and B15's move in the preconditions.
9. **The display map keeps its final shape from M1.** M1 builds all four layers in their order (folds → soft wraps →
   inlays → block decorations, PLAN.md section 2): the soft-wrap and block-decoration layers work, and the fold and
   inlay layers are identity layers. The block-decoration layer stays in M1 because the compare/merge view for a
   dirty buffer's external change (M1) and the diff viewer (M5) need its alignment fillers; the identity layers mean
   soft wrap's input does not change when M15 fills them, and M1's property test already runs on the final stack.
10. **Trust covers U1's new ways to run code.** M4 refuses Build, Rebuild, Clean, Run and the build before Run in an
    untrusted root; a planted File API reply that points an executable target at a binary in the repository, and a
    `.frost/runConfigurations` command, never run before trust; the terminal in an untrusted root runs the
    application-scope shell at its absolute path with NoDefaultCurrentDirectoryInExePath kept (ADR 0011, E2), so a
    command typed there never resolves to a planted binary. M4's exit extends the malicious-repository test to all of
    this. PLAN.md section 3.5 now names "MSBuild design-time builds (ProjectHost or path (c))" as blocked, defines a
    solution's root (its git work tree, or its directory outside one) and lets a CMake build directory outside the
    root be used only when the user chose it in an application-scope profile, never when a repository file names it.
11. **Amended ADRs.** These carry an "Amended" header line that points here: ADR 0004 (milestone range, closed M0,
    the run-order and gate checks), ADR 0006 (decision 1 is M16's plan; B23's two gates), ADR 0011 (B1's samples are
    needed before M16, not M2, and M2 no longer depends on D3; B3's preconditions also apply to the path (c) gate; B4
    is M16's target; B5 and E2 unchanged), ADR 0013 (`m1/large-file` is `m15/large-file`, its `TODO(M1)` a
    `TODO(M15)`) and ADR 0016 (the Keymap page goes with the Settings dialog to M15; the Find Action popup stays in
    M1). ADR 0017 is on `main` only: the merge updates its line about the `TODO(M1)` in `src/text/CMakeLists.txt`
    (Consequences).

### Moved items
"Scope" is a PLAN.md section 7 scope item, "Exit" an exit criterion, "Gate" a budget gate, "Dep." a dependency.
Row numbers are Appendix A's. Every item is still required in its new milestone.

| # | Item | Kind | From | To | Notes |
|---|---|---|---|---|---|
| 1 | Multi-caret and column mode (row 38) | Scope | M1 | M15 | The caret and selection model stays multi-caret-capable (m1/undo-anchors); the commands and the UI move. |
| 2 | The column-mode status widget (part of row 242) | Scope | M1 | M15 | |
| 3 | Folding (row 40) and fold persistence (row 256), with the fold queries | Scope | M1 | M15 | |
| 4 | The contents of the display map's fold and inlay layers | Scope | M1 | M15 | M1 builds all four layers, these two as identities, and the block-decoration layer working (decision 9). |
| 5 | Whitespace and indent guides (row 251) | Scope | M1 | M15 | |
| 6 | Ctrl+wheel editor zoom (row 253) | Scope | M1 | M15 | IDE zoom (row 244) stays in M1. |
| 7 | Extend selection (row 39) | Scope | M1 | M15 | |
| 8 | EditorConfig (row 59) | Scope | M1 | M15 | SaveManager preserves each file's encoding and EOL meanwhile; M15 maps it onto M3's `.clang-format` handling. |
| 9 | Large-file mode (row 35's large-file part): mapped originals of 128 MB and more, parallel summaries, a first screen before the line index, the background release of big trees | Scope | M1 | M15 | M1 adds the large-file guard (decision 7). |
| 10 | The Structure view (row 87) | Scope | M1 | M15 | The outline queries stay in M1 for next/previous member (row 94). |
| 11 | Editor splits (part of row 55) | Scope | M1 | M15 | Tabs stay in M1. |
| 12 | The frameless window with snap layouts (part of row 239) | Scope | M1 | M15 | The native frame meanwhile. |
| 13 | Tool-window drag-docking and floating (part of row 240) | Scope | M1 | M15 | Stripes, show and hide, resizing stay in M1. |
| 14 | The Settings dialog (row 226), with ADR 0016's Keymap page | Scope | M1 | M15 | JSON scope files meanwhile (decision 7). |
| 15 | Keyboard navigation of stripes, toolbar, status bar and gutter (row 246's first part) | Scope | M1 | M15 | |
| 16 | The UIA TextPattern smoke | Scope | M1 | M15 | |
| 17 | Rulers (row 252) | Scope | M1 | M15 | Rider-parity polish, P1; stays before Preview 0.1. |
| 18 | Clipboard history (row 254) | Scope | M1 | M15 | Rider-parity polish, P1; stays before Preview 0.1. |
| 19 | Drag and drop (row 255) | Scope | M1 | M15 | Rider-parity polish, P1; stays before Preview 0.1. |
| 20 | B15 met | Exit, Gate | M1 | M15 | Target unchanged. |
| 21 | F2 editable | Exit | M1 | M15 | M1 tests instead that F2 opens read-only without a stall over 50 ms. |
| 22 | Focus-order tests | Exit | M1 | M15 | |
| 23 | The display-map property test with folds and inlays in use | Exit | M1 | M15 | M1 runs it on the full four-layer stack with identity fold and inlay layers. |
| 24 | ProjectHost (path (b)): 2–4 workers, EnvironmentVariableRead capture, the warm-up (ADR 0006), the launch before L1 (ADR 0011 B4) | Scope | M2 | M16 | Path (c) is M2's evaluator (decision 6). |
| 25 | `.sln`↔`.slnx` conversion (row 3) | Scope | M2 | M16 | Needs SolutionPersistence in ProjectHost. |
| 26 | New Project/Solution (part of row 26) | Scope | M2 | M16 | Writes through ProjectHost. |
| 27 | The first-run wizard (part of row 26), with the Dev Drive check (D19) | Scope | M2 | M16 | The Welcome window stays in M2. |
| 28 | C# projects in the tree (section 1.2; part of row 27) | Scope | M2 | M16 | |
| 29 | The Unload/Reload project and Properties actions (parts of row 31) | Scope | M2 | M16 | Set as Startup Project, Open in Explorer and Copy Path stay in M2. |
| 30 | Show All Files and nesting (row 30) | Scope | M2 | M16 | |
| 31 | `frost --wait`, `frost diff`, `frost merge`, file associations, the Jump List (part of I10) | Scope | M2 | M16 | Single instance and `frost <path>` stay in M2. |
| 32 | The TODO window (row 115) | Scope | M2 | M15 | Rider-parity polish, P1; stays before Preview 0.1. |
| 33 | B23's first project streamed met on ProjectHost | Exit, Gate | M2 | M16 | Target and preconditions unchanged; M2 gates path (c)'s first project against the same target (decision 8). |
| 34 | The scripted clean-profile first run, with screenshot goldens | Exit | M2 | M16 | Goes with the wizard; U1's scenario also runs under a clean profile. |
| 35 | `git -c core.editor="frost --wait" commit` round-trips | Exit | M2 | M16 | Goes with `--wait`. |
| 36 | D3 and its finalisation through the S6 cold samples (ADR 0011 B1), D6, D19 | Dep. | M2 | M16 | M2 now depends on M1 and D16. |
| 37 | Inspection framework v1: severities and suppression comments (row 99's first part) | Scope | M3 | M17 | P0; M17 runs before Preview 0.1. |
| 38 | Inlay hints (row 42) | Scope | M3 | M17 | On M15's inlay layer. |
| 39 | Quick Definition (row 52) | Scope | M3 | M17 | |
| 40 | Macro expansion (row 53) | Scope | M3 | M17 | |
| 41 | Smart completion (row 70) | Scope | M3 | M17 | Basic, #include and auto-import completion stay in M3. |
| 42 | Navigate To (row 83) | Scope | M3 | M17 | |
| 43 | Type hierarchy (row 88) and call hierarchy (row 89) | Scope | M3 | M17 | |
| 44 | Select In and the navigation bar (row 95), with the navigation bar's status-bar part (row 242) | Scope | M3 | M17 | The error indicator stays in M3. |
| 45 | Intention preview (row 101) | Scope | M3 | M17 | Alt+Enter (row 100) stays in M3. |
| 46 | include-cleaner: unused and missing includes (row 106), optimize includes (row 125) | Scope | M3 | M17 | Both still complete in M7. |
| 47 | Resolve context (row 21) | Scope | M3 | M17 | |
| 48 | The dotnet build driver and C# solution citizenship: build and run R5, navigation to C# build errors | Scope | M4 | M16 | With C# in the tree (item 28). |
| 49 | FrostIDE.MSBuildLogger | Scope | M4 | M16 | Parsed console output meanwhile (decision 7). |
| 50 | Importers for `launch.vs.json`, `.vcxproj.user`, `.idea` and `launchSettings.json` | Scope | M4 | M16 | M16's malicious-repository test covers them. |
| 51 | The before-launch task list (row 162) | Scope | M4 | M16 | Build-before-run, after Save All, stays in M4. |
| 52 | Gutter run icons for `main()` (row 164) | Scope | M4 | M16 | Still completes in M6. |
| 53 | The CMakeLists language service (row 66) | Scope | M4 | M16 | |
| 54 | The CMake cache viewer (row 13) | Scope | M4 | M16 | |
| 55 | LLVM SDK artifacts where S5 found gaps | Scope | M4 | M6 | Merged with M6's Windows lldb-dap open item. |
| 56 | R5 builds from inside the IDE | Exit | M4 | M16 | R2b and R3 still build in M4. |
| 57 | CMake completion and navigation goldens on R3's CMakeLists | Exit | M4 | M16 | |
| 58 | Built SDK artifacts stored under E:\sdk and `~/sdk` and pinned in `sdk/sdk.lock.json` | Exit | M4 | M6 | |
| 59 | D27 (ProjectHost runtime) | Dep. | M5 | M16 | M5's packages carry no ProjectHost yet. |
| 60 | D17's conversion part | Dep. | M2 | M16 | Solution editing stays in M11. |

**Added, not moved:**
- M1: the identity fold and inlay layers and the property test on the full stack (decision 9); the large-file guard in
  the load service with its open, reload, merge and restore tests (decision 7); the scope files opened from Find
  Action; `m1/startup-b1` and `m1/keymap-fuzz` as slices (B1 and the fuzz target were already M1 work).
- M2: path (c) as the evaluator, with ProjectModelCache fingerprints, the deterministic environment and its relaunch
  test, and live re-evaluation (decision 6); B23's first-project gate on path (c) (decision 8); a C# test-only
  SolutionPersistence oracle for the S50 equality test, which ProjectHost carried before; the solution-root and
  out-of-root build-directory rules and their malicious-repository cases (decision 10).
- M4: Save All before Build and Run; the trust rules and tests for Build, Run and the terminal (decision 10); the U1
  definition, the acceptance scenario with its setup (decision 2), the probe configuration of the TUI probe; M4
  depends on M3.
- M15: the Settings pages for every key M1–M5 added, with a registry test; B5 and B6 rechecked with folds, inlays and
  several carets; the tests for rows 115, 252, 254 and 255.
- M16: the malicious-repository test for imported run configurations and before-launch tasks.
- M17: the suppression and resolve-context tests.
- Dependencies: M6 on M15–M17, M7 on M17, M9 on M16, M11 on M15, M15 on M2 and M3, M16 on M5 and M15.

### Units
| Milestone | Before | After | Ends around month |
|---|---|---|---|
| M1 | 11–13 | 8–9 | 3–4 (was 4–5) |
| M2 | 12–14 | 9–10 | 5–6 (was 7–8) |
| M3 | 10–12 | 7–8 | 7–8 (was 10–12) |
| M4 (U1) | 9–11 | 6–7 | 9–10 (was 12–15) |
| M15 | — | 6–7 | 13–15 |
| M16 | — | 9–11 | 15–18 |
| M17 | — | 5–6 | 17–20 |
| M6 | 11–13 | 12–14 | 20–24 (was 17–21) |
| M11 | 9–11 | 9–11 | 31–38 (was 29–35) |
| All milestones | 120–145 | 129–154 | M14 at 35–42 (was 33–40) |

The estimates changed in review. M16 takes in ProjectHost (about 3k lines of C#, StreamJsonRpc, workers and its fake),
the MSBuild logger, conversion, New Project, the first-run wizard, C# with R5, the shell-integration commands, four
importers, before-launch tasks, gutter icons, the CMake language service and the cache viewer, which M2 and M4
shed (about 8–9 units between them) while gaining work; with a milestone's exit overhead it is 9–11, not 7–9. M1
keeps the block-decoration layer and gains `m1/startup-b1`, `m1/keymap-fuzz`, the guard and the scope-file
stand-in (8–9, not 7–8). M2 gains the oracle, the environment work and the path (c) gate (9–10). M4 gains the
trust work and the scenario setup (6–7). M15 gains rows 115, 252, 254 and 255 and the settings pages, and loses
the block layer (6–7); M11 returns to 9–11. The longer roadmap also spans more upgrades (5–7 LLVM majors and Qt
minors instead of 5–6), so the upgrade track is 9–12 units, not 9–11. With it and 10% contingency the roadmap is
152–183 weeks, about 2.9–3.5 years (was 142–172 weeks, 2.7–3.3 years). Months follow PLAN.md's model: cumulative
units, plus the upgrade track's share and 10% contingency, at 4.33 weeks a month, rounded, month 1 being November
2026 (it reproduces the old table exactly).

### M1 slices
M1's remaining slices change as follows; the running ones keep their scope (the slice plan has the details).
- `m1/large-file` moves to M15 whole as `m15/large-file`. Its guard goes into `m1/save-manager`'s document load
  service (open, reload, merge, restore), with the long-line cap from `m1/editor-view` and the notification in
  `m1/shell-nav`.
- `m1/settings` loses EditorConfig (M15) and gains the scope files opened from Find Action with validation on save.
- `m1/syntax` keeps the highlight and outline queries; the fold queries go with folding to M15.
- `m1/display-map` builds all four layers in their final order: soft wrap and block decorations working, fold and
  inlay as identity layers; it no longer waits for `m1/syntax`.
- `m1/editor-model` loses the multi-caret and column-mode commands, extend selection and the clipboard-history model.
- `m1/editor-view` loses whitespace and indent guides, rulers, Ctrl+wheel zoom, drag and drop and "F2 editable", and
  names the painting of tree-sitter highlight captures, which no slice named before.
- `m1/shell-frame` loses the frameless window, splits, drag-docking and floating, the column-mode widget and keyboard
  navigation with its focus-order tests; it has no on-screen step left.
- `m1/shell-nav` loses the Settings dialog and the Structure window; it gets core-tier review, as `m1/diff-merge`
  does, because both carry the data-safety merge of external changes.
- `m1/save-manager` also depends on `m1/settings` (autosave and local-history retention keys).
- `m1/automation-bench` loses B15 and the UIA TextPattern smoke.
- New: `m1/startup-b1` (B1 warm still misses 150 ms, ADR 0011 A3; the logon harness of A4 that also collects the S6
  cold samples of B1) and `m1/keymap-fuzz` (the `input_keymap` fuzz target, ADR 0016).

## Measurements
No new measurement. The estimates are judgement, made the same way as PLAN.md's. Path (c)'s cost is from ADR 0005: a
fresh MSBuild process per project and configuration has a floor of about 0.3–0.4 s (p50 331–405 ms across three R2b
projects and R2a, warm file cache, REF-W1), against ProjectHost's warm workers. ADR 0006 decision 2 (A) compares
Host.EXE through path (c) (p50 331 ms) with a cold ProjectHost (1.90 s) on the same day. The R3 facts in decision 2
were checked on disk on 2026-11-01: `E:\corpus\R3` has no CMakeLists.txt (the sources and CMakePresets.json are in
`R3\llvm`); `E:\corpus\R3-build` has `CMAKE_HOME_DIRECTORY=E:/corpus/R3/llvm`, Ninja, Release, no `.cmake/api`
directory, no `bin\not.exe`, a toolchain file path in a worktree that no longer exists, and S7's clangd index
(12,364 shards) in `.cache\clangd\index`. `ExecuteAndWait` is called from `utils/not/not.cpp`,
`lib/Support/GraphWriter.cpp` and `lib/Support/Signals.cpp`. `lib/Support/Program.cpp` includes the platform's
`Program.inc`, which holds most definitions of `Program.h`, so the scenario switches between `WithColor.h` and
`WithColor.cpp` instead.

## Consequences
- **Sooner:** U1 around month 9–10, 3–5 months before the old M4 exit (12–15) and 1–2 months before the old
  "first usable checkpoint" (M3, 10–12), which had no build, run or terminal. M2 no longer waits for D3 or the S6 cold
  samples.
- **Costlier:** about 9 more units (129–154 against 120–145): three more milestone exits (demo packs, `-Full` and
  nightly runs, comment-accuracy reviews, sign-offs), the interim behaviours of decision 7 that later milestones
  replace, work added for U1 (the scenario, the path (c) gate, the trust rules, the environment work) and work done
  twice (the settings pages for keys added before the dialog). Preview 0.1 moves from month 17–21 to 20–24 and v1.0
  from 33–40 to 35–42. The display map's layer design (PLAN.md section 2, "Editor rendering") and decision 9 keep
  the fold and inlay layers from forcing a rewrite.
- **Slower until M16:** a cold open of an MSVC solution starts one MSBuild process per project and configuration
  instead of ProjectHost's workers, and the whole-environment fingerprint can invalidate more records than
  EnvironmentVariableRead capture would when the user's own environment changes. R2b's cold open is reported, not
  gated, in M2 as before.
- **Unchanged:** data safety, the trust model, listener hardening and the no-network test are all in U1; no budget
  target, test or warning is weakened; every P0 and P1 row that was due by Preview 0.1 still lands by then; CLAUDE.md
  does not change.
- **Done in this change:** PLAN.md (status, key decisions, the ladder, sections 1.1, 1.2, 1.4, 2, 2.2, 2.3, 3.4,
  3.5, 3.6, 4, 7, 8, 9 and Appendix A); `tests/features/matrix.json` regenerated (41 rows change owner or completion;
  statuses kept); `tests/perf/budgets.json` (B15, B23); the milestone range, the run-order and completion checks and
  the closed M0 in `featurecheck.py` and `frost_lint.py`, the stricter gate checks in `test_budgets.py`, with tests
  and a ladder in featurecheck's fixture plan; the amended ADRs of decision 11; the to-do markers and comments whose
  work moved: `src/text/CMakeLists.txt` and `src/text/include/frost/text/snapshot.hpp` (large-file mode, now
  `TODO(M15)`), `tools/featurecheck.py` (the IntelliJ editor-settings checklist, now `TODO(M15)`),
  `src/app/src/main.cpp` (file associations and the AppUserModelID with the Jump List, now `TODO(M16)`),
  `tests/text/perf_test.cpp` (B15 is gated by M15's large-file mode) and the `TODO(M2)` in
  `src/io/include/frost/io/process.hpp` (path (c)'s clean environment). Every other `TODO(M1)`–`TODO(M4)` in `src`,
  `tests`, `tools` and `cmake` names work that stays in its milestone.
- **Merging with `main`:** `m1/undo-anchors` and `m1/theme-icons` are merged there now, so the conflicts are concrete.
  `git merge-tree` of this branch against `main` (94ed47d) reports two: `src/text/CMakeLists.txt` (the to-do line:
  keep main's text without the anchors and undo item, with the large-file part as `TODO(M15)`) and
  `tests/features/matrix.json` (row 38, now owned by M15, has main's `in-progress` status; rows 243 and 244 have
  main's acceptance tests; take main's status and acceptance fields, then run `featurecheck.py generate`).
  `tools/frost_lint.py` merges cleanly. ADR 0017's line "`TODO(M1)` in src/text/CMakeLists.txt keeps m1/encodings
  and m1/large-file only" becomes "keeps m1/encodings only; large-file mode is `TODO(M15)` (ADR 0029)". The running
  slices meet the same kind of conflict when they merge after this branch (`m1/encodings` in
  `src/text/CMakeLists.txt`); a slice that adds a `TODO(M1)` for work this ADR moved retags it to M15, M16 or M17
  before it merges.
- **Hand-off:** this branch does not edit `docs/progress.md`; the session that merges it records it there.

## Open questions
For the user before this ADR is accepted. Each has a recommendation; the branch implements the recommendation.
- **Q1. Path (c) as U1's MSBuild evaluator, ProjectHost in M16.** Recommended: yes. The alternative keeps ProjectHost
  in M2: about 2–3 more units before U1 (the C# host, StreamJsonRpc, its NuGet and licence checks, a supervisor
  protocol and its fake) for the same command lines that path (c) yields; U1 would then reopen solutions with warm
  workers and narrower environment fingerprints.
- **Q2. B23's first project gated at M2 on path (c).** Recommended: yes. It changes the M2 gate's method from
  ProjectHost's cold start to `MSBuild.exe`'s (ADR 0006 decision 2 (A) asks for the user's approval); the alternative,
  reporting path (c)'s time without a gate until M16, leaves U1's first open of an MSVC solution without a budget.
- **Q3. M5 (Git, Claude C1, packages) right after U1, then M15–M17.** Recommended: yes, with M15 and M17 alongside M5
  if two agent streams are wanted. The alternative runs M15–M17 first, which brings the editor and shell polish, the
  Settings dialog and ProjectHost about 2–3 months earlier and Git and the bridge as much later.
- **Q4. No debugger in U1.** Recommended: keep it in M6 (Preview 0.1, around month 20–24), as the question to the user
  listed "basic editor, CMake, clangd, build and run". A minimal debugger in U1 (lldb-dap launch, line breakpoints,
  stepping, the Debug window) would cost roughly 3–5 units before U1, more on Windows, where the official LLVM package
  has no lldb-dap (S5) and one has to be built first.
- **Q5. A thinner U1.** Optional. These U1 items could move to M15 or M16, still before Preview 0.1, to bring U1
  about 1–2 units sooner: approximate mode for solutions on Linux (D16, scenario step 12), the .slnf reader,
  compile_commands and open folder, Search Everywhere v1 (Go to File, Class and Symbol and Find Action stay), terminal
  IME and the ja VS-output goldens. Not done unless the user asks.

## Alternatives considered
- **Keep the plan.** Real C++ work at the M4 exit, around month 12–15. Rejected by the user's answer.
- **Call M3 the usable build.** Without build, run and a terminal it does not meet the user's "usable for real C++
  work".
- **Renumber the milestones.** Breaks every `TODO(Mn)`, ADR, matrix owner and budget gate that names a milestone, and
  their history.
- **Sub-IDs such as `M1b`.** featurecheck, frost-lint and the budgets test parse only `M` plus a number; every regex
  and the matrix schema would change, for no gain over M15–M17.
- **Spread the deferred halves over M5, M6 and M11 only.** M6 would grow past 20 units, and P0 rows put into M11
  would slip past Preview 0.1, which needs the user's approval row by row (PLAN.md section 1.4).
- **Rows 115, 252, 254 and 255 in M11** (this ADR's first draft). They would move from month 4–8 to 31–38, past
  Preview 0.1, which PLAN.md section 6.7 asks the user about; M15 keeps them before Preview 0.1 for the same work.
- **Keep ProjectHost in M2.** Open question Q1.
- **Report B23's first project at M2 without a gate** (this ADR's first draft). It leaves a feature U1 ships without
  its budget, against the constraint that U1's budgets stay gated where the feature lands; open question Q2.
- **Move the block-decoration layer to M15 too,** with the M1 merge view and M5's diff viewer aligning their panes
  some other way. That is a second alignment mechanism to build and later replace, and soft wrap's input would
  change when the fold layer arrives (decision 9).
- **Defer data safety or security past U1.** Excluded by the constraints: U1 is for real work on real files and on
  untrusted repositories.
