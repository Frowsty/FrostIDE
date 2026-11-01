# ADR 0029: Thin path to a first usable build

- Status: accepted (2026-11-01, evening). D29 itself is the user's decision, quoted below. The sub-decisions that
  went beyond it were open questions Q1–Q5, which the user then answered ("Answers", below): Q1, Q2 and Q3 as
  recommended, Q4 reversed (a minimal debugger in U1, decision 12) and Q5's thinner U1 declined.
- Date: 2026-11-01; revised 2026-11-01 after the independent review (B23's path (c) gate, the U1 scenario's
  setup, recorders and steps, trust for Build, Run and the terminal, the MSBuild environment, the display-map
  layers, the estimates, the P1 rows kept before Preview 0.1, the checks); revised again 2026-11-01 with the user's
  answers (decision 12, the units, the scenario's debug steps)
- Plan reference: PLAN.md section 7 (roadmap, "First usable build (U1, the M4 exit)", M4, M6, M15–M17), Appendix A,
  section 4 (B15, B16, B23), section 1.4 (P0 rows after Preview 0.1), section 2 (Debuggers), sections 3.5 and 3.6
  (trust, settings), section 9 (D29); ADR 0001 (lldb-dap); `tools/featurecheck.py`, `tools/frost_lint.py`,
  `tests/tools/test_budgets.py`

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
(merge 3163d0b) and `m1/theme-icons` (merge 94ed47d) have been merged into `main`, and later `m1/storage`,
`m1/io-reactor-watch`, `m1/encodings`, `m1/display-map`, `m1/keymap-de` and `w1/project-sln`; this branch merged
`main` at dcfd9ae (Consequences, "Merging with `main`").

Two decisions of the user came after this ADR and build on it: ADR 0031 (a feature-first phase until U1, with a
hardening phase after it) and ADR 0032 (W1, a work-ready build for the user's MSVC solutions, first inside M1–M4,
then clangd diagnostics, navigation and completion, then the rest of U1). They change the order of the work inside
M1–M4 and the process until U1, not U1's content, the milestone units or the budgets (PLAN.md section 7, "W1 first"
and "Feature-first phase").

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
   build errors from the Build window, run, debug with a minimal debugger (decision 12), and use a terminal, which is
   also how Claude Code runs until the C1 bridge (M5). PLAN.md section 7, "First usable build", lists what U1
   contains under Open, Browse, Edit, Data safety, Intelligence, Build and run, Debug, Terminal, Fast and Safe.
2. **U1 has a scripted end-to-end acceptance scenario** on the existing corpora (`tests/e2e/u1`, frost-bench through
   the automation endpoint, a clean profile). PLAN.md section 7 gives the steps; their shape:
   - **Setup outside the scenario,** once per corpus pin: R3's build directory `E:\corpus\R3-build` is reconfigured
     from `E:\corpus\R3\llvm` with a toolchain file outside every worktree (its cache names a removed worktree's),
     with FrostIDE's `client-frostide` query, and `not` is built with its dependencies (`bin\not.exe` is absent
     today); on Linux an ext4 checkout `~/corpus/R3` gets a pinned Linux configuration (none is pinned today), a
     Debug build so that the Linux debug step's breakpoints bind and its locals are readable. The clean profile is
     seeded with an application-scope CMake profile naming the build directory (llvm's CMakePresets.json gives none)
     and R3's persisted clangd index, so Find Usages and Switch Header/Source do not depend on a cold index.
   - **Recorders:** the no-network recorder and the listener enumeration run throughout and assert on FrostIDE's own
     process tree (frost, its crash handler, the engines and tools it supervises, lldb-dap included). The trees that
     Build, Run, Debug and the terminal start are the user's code (MSBuild nodes, `mspdbsrv.exe`, `not`, Notepad++,
     the shell): recorded, not asserted.
   - **CMake on R3:** open `R3\llvm` in restricted mode (the target tree from the existing File API replies), trust;
     navigate `utils/not/not.cpp`; Find Usages and Switch Header/Source once L2 is reached; complete `llvm::outs()`
     at the top of `main`, before the early `return 1` of a run without arguments; inject an error; kill frost and
     get the dirty buffer back from the hot-exit journal; build (Save All writes the buffer first), open the error
     from the Build window, fix, build; run `not` and see the injected output and exit code 1; on Linux, debug it (a
     line breakpoint on the injected line, Step Into `llvm::outs()` and Step Out, Step Over past `--argc;`, a watch
     and Evaluate, Continue to exit code 1); revert the file from the terminal and see the clean buffer reload; exit
     and reopen within B3 and B2.
   - **MSVC on R2a:** open `PowerEditor\visual.net\notepadPlus.sln` in restricted mode, trust the root (the git work
     tree, which holds Scintilla and Lexilla), check clangd on `EncodingMapper.cpp`; select Debug|x64; inject an
     error, open MSVC's C2065 from the Build window, fix, build Notepad++, Scintilla and Lexilla; run Notepad++ with
     its settings in a scratch directory and stop it through the Job Object; debug the cl.exe-built Notepad++ through
     its PDB (a line breakpoint in `wWinMain`, the stack above the CRT start-up frames, stepping, a watch and
     Evaluate, Continue, Pause to list its threads, Stop).
   - **Terminal:** the M4 Claude TUI probe, with its pre-seeded probe configuration and no prompt submitted.
     **Linux:** R2a's solution in approximate mode.
   - It records B1, B2, B3, B8, B9, B10, B11, B17 and B20 samples (2x-budget smoke; B16's corpus is R1, so the
     B-suite gates it) and ends with `git status --porcelain --untracked-files=no` empty for each corpus and only the
     root's `.frost/` untracked.
3. **M1–M4 shrink to their thin cores, and their deferred halves move.** M1's go to the new milestone M15, M2's and
   M4's to M16 (the TODO window to M15), M3's to M17. One block moves the other way: U1's minimal debugger comes
   from M6 into M4 (decision 12), and the LLVM SDK artifacts, from which M4 builds lldb-dap, stay in M4. No P0 or P1
   row moves past Preview 0.1: the Rider-parity P1 rows that U1 does not need (rulers 252, clipboard history 254,
   drag and drop 255, the TODO window 115) go to M15, not to M11. The table below lists every move.
4. **Order.** M0, M1, M2, M3, M4 (U1), M5 (dogfood v0.1), M15, M16, M17, M6 (Preview 0.1), then M7–M14. M5 comes
   before the completion milestones because Git and the Claude C1 bridge are the next most valuable things to a U1
   user (Q3, accepted). M15–M17 come before M6, so every P0 row still lands by Preview 0.1 (PLAN.md section 1.4)
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
6. **Path (c) is U1's MSBuild evaluator; ProjectHost lands in M16** (Q1, accepted). `MSBuild.exe -getTargetResult`
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
   - Run and Debug build the target first, after Save All (M4); the before-launch task list generalises it (M16);
   - debugger values as lldb-dap's default formatters show them (M4); FrostIDE's LLDB STL formatters (M6) and natvis
     through the DbgEng adapter (M10) replace them.
8. **Budgets.** No target changes:
   - B15 (F1 first screen) moves from M1 to M15, with large-file mode, which U1 does not contain (it has the guard).
   - B16 (F5 → breakpoint / step, R1) moves from M6 to M4, with U1's minimal debugger (decision 12).
   - B23's first project streamed is gated twice (Q2, accepted). At M2 on path (c), U1's evaluator, under the
     five preconditions of ADR 0011 B3, with `MSBuild.exe`'s cold start in place of ProjectHost's and the target in
     force at the M2 exit (3 s, or the final value if the user has set it from the S6 samples by then). At M16 on
     ProjectHost, as defined (ADR 0006, ADR 0011 B1), against the final value. This changes the M2 gate's method,
     which ADR 0006 decision 2 (A) says needs an ADR and the user's approval. ADR 0005 and ADR 0006 measured path
     (c)'s Host.EXE at p50 331 ms with a warm file cache, against 1.90 s for a cold host, so 3 s looks reachable; a
     cold cache was not measured.
   Every other gate stays: B1, B5, B6, B19 (no project), B20 and B21 at M1; B2, B3, B4, B7, B13 (unindexed) and B23
   (trees) at M2; B8–B11, B19 (R3), B22, B24 (M3 value), B25 and B26 at M3; B17 at M4; B14, B18 and B27 at M5; B12
   at M7; B13 (indexed), B24 (final) and B26 (REF-L1) at M13; B28 at M0. `budgets.json` records both gates of B23
   (`"evaluator": "path (c)"` and `"ProjectHost"`) and B15's and B16's moves in the preconditions.
9. **The display map keeps its final shape from M1.** M1 builds all four layers in their order (folds → soft wraps →
   inlays → block decorations, PLAN.md section 2): the soft-wrap and block-decoration layers work, and the fold and
   inlay layers are identity layers. The block-decoration layer stays in M1 because the compare/merge view for a
   dirty buffer's external change (M1) and the diff viewer (M5) need its alignment fillers; the identity layers mean
   soft wrap's input does not change when M15 fills them, and M1's property test already runs on the final stack.
10. **Trust covers U1's new ways to run code.** M4 refuses Build, Rebuild, Clean, Run, Debug and the build before Run
    or Debug in an untrusted root, and starts no lldb-dap there (debugger init files such as the planted `.lldbinit`
    were already blocked); a planted File API reply that points an executable target at a binary in the repository,
    and a `.frost/runConfigurations` command, never run before trust; the terminal in an untrusted root runs the
    application-scope shell at its absolute path with NoDefaultCurrentDirectoryInExePath kept (ADR 0011, E2), so a
    command typed there never resolves to a planted binary. M4's exit extends the malicious-repository test to all of
    this. PLAN.md section 3.5 now names "MSBuild design-time builds (ProjectHost or path (c))" as blocked, defines a
    solution's root (its git work tree, or its directory outside one) and lets a CMake build directory outside the
    root be used only when the user chose it in an application-scope profile, never when a repository file names it.
11. **Amended ADRs.** These carry an "Amended" header line that points here: ADR 0001 (M4, not M6, sources
    lldb-dap, by building it from the pinned LLVM sources on both operating systems; decision 12), ADR 0004
    (milestone range, closed M0, the run-order and gate checks), ADR 0006 (decision 1 is M16's plan; B23's two
    gates), ADR 0011 (B1's samples are needed before M16, not M2, and M2 no longer depends on D3; B3's preconditions
    also apply to the path (c) gate; B4 is M16's target; B5 and E2 unchanged), ADR 0013 (`m1/large-file` is
    `m15/large-file`, its `TODO(M1)` a `TODO(M15)`) and ADR 0016 (the Keymap page goes with the Settings dialog to
    M15; the Find Action popup stays in M1). ADRs 0015, 0017, 0018 and 0033 came from `main`: the merge retags
    their large-file to-dos to M15 (0015, 0017 and 0018) and names ProjectHost as M16's (0033), each with an
    "Amended" line (Consequences).
12. **A minimal debugger is part of U1** (Q4, the recommendation reversed by the user). M4 gets, from M6:
    - the DAP client (`IDebugAdapterClient`, its fake-dap double, the DAP 1.71 types from frost-codegen) and one
      engine on Windows and Linux, lldb-dap, started under the supervisor at its absolute path. The official LLVM
      23.1.4 packages give no usable lldb-dap (none on Windows; on Linux it links a libpython that Ubuntu 24.04 lacks,
      S5), so M4 builds it from the pinned LLVM sources into E:\sdk and `~/sdk` and pins it in `sdk/sdk.lock.json`:
      the LLVM SDK artifacts where S5 found gaps, which therefore stay in M4 (decision 3; ADR 0001 amended);
    - Debug from a run configuration, after Save All and the build, as Run does; line breakpoints in the gutter,
      kept per root in `.frost/workspace.json`; step over, into and out, continue, pause and stop; the Debug window
      with the call stack, threads, variables, watches, Evaluate and the program's output; a stop at the faulting
      frame on a crash or an unhandled exception. MSVC-built programs are debugged through their PDBs, DWARF
      programs on Linux; values show as lldb-dap's default formatters give them (decision 7);
    - matrix rows 175 (Debug window) and 178 (Evaluate, watches), owned by M4; rows 167 (debugger engine), 169 (line
      breakpoints) and 173 (stepping), owned by M4 and completed in M6 (the GDB DAP profile; conditions, hit counts
      and log messages; Run to Cursor and the other stepping actions); B16 at M4 (decision 8); FrostIDE debugging
      itself (R1) and DAP conformance scenarios for every request and event M4 uses (M6 grows the suite to ≥60);
    - **spike S10, first in M4:** lldb-dap 23.1.4 on R2a's cl.exe-built Notepad++ (Debug|x64, with PDBs) and on each
      OS whether its default formatters need lldb's Python scripting, which settles ADR 0001's choice for the Linux
      build. If S10 shows that lldb-dap cannot carry the scenario's Windows debug step, M4 stops and the user decides:
      the alternative, the DbgEng adapter (M10), first needs the ComponentDownloader, network consent and a legal
      review (D12, L5);
    - no symbol servers or debuginfod: lldb-dap starts without `_NT_SYMBOL_PATH`, `_NT_ALT_SYMBOL_PATH` and
      `DEBUGINFOD_URLS`, and the no-network test and the listener enumeration cover a debug session on each OS.

    M6 keeps the rest of debugger v1 (the GDB DAP profile, function and exception breakpoints, the Breakpoints
    dialog, conditions and log messages, Run to Cursor, Set Value, inline values, the debugger console, attach, LLDB
    STL formatters, the CMake debugger, ptrace_scope detection), the test runner, installers and the updater; M10
    keeps the DbgEng adapter. M5's packages carry M4's lldb-dap build.

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
| 55 | The DAP client and lldb-dap on Windows and Linux (row 167's lldb part), with the Windows lldb-dap source that S5 left open, now built from the pinned LLVM sources | Scope | M6 | M4 | Decision 12 (Q4). The LLVM SDK artifacts it is built from stay in M4; this ADR's first draft had moved them to M6. Row 167's GDB DAP profile stays in M6. |
| 56 | R5 builds from inside the IDE | Exit | M4 | M16 | R2b and R3 still build in M4. |
| 57 | CMake completion and navigation goldens on R3's CMakeLists | Exit | M4 | M16 | |
| 58 | FrostIDE debugs itself (R1) | Exit | M6 | M4 | Decision 12. The built SDK artifacts (lldb-dap) are stored under E:\sdk and `~/sdk` and pinned in `sdk/sdk.lock.json` at M4's exit, where they always were. |
| 59 | D27 (ProjectHost runtime) | Dep. | M5 | M16 | M5's packages carry no ProjectHost yet. |
| 60 | D17's conversion part | Dep. | M2 | M16 | Solution editing stays in M11. |
| 61 | Line breakpoints (row 169) | Scope | M6 | M4 | Decision 12. Conditions, hit counts and log messages stay in M6, where the row completes. |
| 62 | Stepping (row 173): over, into, out; continue, pause, stop | Scope | M6 | M4 | Run to Cursor and the other stepping actions stay in M6, where the row completes. |
| 63 | The Debug window (row 175): call stack, threads, variables, the program's output | Scope | M6 | M4 | |
| 64 | Evaluate and watches (row 178) | Scope | M6 | M4 | Set Value (row 179) stays in M6. |
| 65 | B16 met | Exit, Gate | M6 | M4 | Target and corpus (R1) unchanged (decision 8). |
| 66 | DAP conformance scenarios for the requests and events M4 uses | Exit | M6 | M4 | M6 grows the suite to ≥60 scenarios, M4's included. |

**Added, not moved:**
- M1: the identity fold and inlay layers and the property test on the full stack (decision 9); the large-file guard in
  the load service with its open, reload, merge and restore tests (decision 7); the scope files opened from Find
  Action; `m1/startup-b1` and `m1/keymap-fuzz` as slices (B1 and the fuzz target were already M1 work).
- M2: path (c) as the evaluator, with ProjectModelCache fingerprints, the deterministic environment and its relaunch
  test, and live re-evaluation (decision 6); B23's first-project gate on path (c) (decision 8); a C# test-only
  SolutionPersistence oracle for the S50 equality test, which ProjectHost carried before; the solution-root and
  out-of-root build-directory rules and their malicious-repository cases (decision 10).
- M4: Save All before Build, Run and Debug; the trust rules and tests for Build, Run, Debug and the terminal
  (decision 10); the U1 definition, the acceptance scenario with its setup and its two debug steps (decision 2), the
  probe configuration of the TUI probe; for the debugger (decision 12): spike S10, the crash fixtures built with
  cl.exe and clang, variable goldens per OS for built-in types, pointers, arrays, structs and classes, the stop on
  a crash or an unhandled exception, the symbol-server variables kept out of lldb-dap's environment, and a debug
  session under the no-network test and the listener enumeration; R3's Linux configuration as a Debug build; M4
  depends on M3.
- M5: its packages carry M4's lldb-dap build.
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
| M4 (U1) | 9–11 | 9–12 | 10–12 (was 12–15) |
| M5 | 8–10 | 8–10 | 12–14 (was 14–17) |
| M15 | — | 6–7 | 14–16 |
| M16 | — | 9–11 | 16–19 |
| M17 | — | 5–6 | 17–21 |
| M6 | 11–13 | 9–11 | 20–24 (was 17–21) |
| M11 | 9–11 | 9–11 | 31–38 (was 29–35) |
| All milestones | 120–145 | 129–156 | M14 at 35–43 (was 33–40) |

The user's answer to Q4 moved U1's minimal debugger into M4 (decision 12): about 3 units from M6 (the DAP client,
lldb-dap, line breakpoints, stepping, the Debug window, Evaluate and watches, B16 and FrostIDE debugging itself,
with the LLVM SDK artifacts that this ADR's first draft had moved to M6), and up to 2 more for what U1 adds to it
(spike S10, the trust and no-network cases of a debug session, the scenario's two debug steps and R3's Linux Debug
configuration). M4 goes from 6–7 to 9–12 and M6 from 12–14 to 9–11; the milestones total 129–156, not 129–154.
The paragraph below gives the estimates as the review left them, before that answer.

The estimates changed in review. M16 takes in ProjectHost (about 3k lines of C#, StreamJsonRpc, workers and its fake),
the MSBuild logger, conversion, New Project, the first-run wizard, C# with R5, the shell-integration commands, four
importers, before-launch tasks, gutter icons, the CMake language service and the cache viewer, which M2 and M4
shed (about 8–9 units between them) while gaining work; with a milestone's exit overhead it is 9–11, not 7–9. M1
keeps the block-decoration layer and gains `m1/startup-b1`, `m1/keymap-fuzz`, the guard and the scope-file
stand-in (8–9, not 7–8). M2 gains the oracle, the environment work and the path (c) gate (9–10). M4 gains the
trust work and the scenario setup (6–7). M15 gains rows 115, 252, 254 and 255 and the settings pages, and loses
the block layer (6–7); M11 returns to 9–11. The longer roadmap also spans more upgrades (5–7 LLVM majors and Qt
minors instead of 5–6), so the upgrade track is 9–12 units, not 9–11. With it and 10% contingency the roadmap was
152–183 weeks after the review and is 152–185 weeks with the debugger, about 2.9–3.6 years (was 142–172 weeks,
2.7–3.3 years). Months follow PLAN.md's model: cumulative units, plus the upgrade track's share and 10%
contingency, at 4.33 weeks a month, rounded, month 1 being November 2026 (it reproduces the old table exactly).

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
`WithColor.cpp` instead. For the debug steps, also checked on disk on 2026-11-01: `not.cpp`'s `main` starts with
`bool ExpectCrash = false;`, then `++argv; --argc;`, and returns 1 at `if (argc == 0)` for a run without
arguments; `R2a\PowerEditor\src\winmain.cpp`'s `wWinMain` calls `parseCommandLine(pCmdLine, params);` and then
sets `isMultiInst`, `doFunctionListExport` and `doPrintAndQuit` from `params`. lldb-dap on cl.exe-built programs
is not measured yet; that is spike S10's job (decision 12).

## Consequences
- **Sooner:** U1 around month 10–12, 2–3 months before the old M4 exit (12–15), with a debugger that the old plan
  gave only at M6 (month 17–21); the old "first usable checkpoint" (M3, 10–12) had no build, run, debugger or
  terminal. M2 no longer waits for D3 or the S6 cold samples.
- **Costlier:** about 9–11 more units (129–156 against 120–145): three more milestone exits (demo packs, `-Full` and
  nightly runs, comment-accuracy reviews, sign-offs), the interim behaviours of decision 7 that later milestones
  replace, work added for U1 (the scenario, the path (c) gate, the trust rules, the environment work, spike S10 and
  the debug session's checks) and work done twice (the settings pages for keys added before the dialog). Preview
  0.1 moves from month 17–21 to 20–24 and v1.0 from 33–40 to 35–43. The display map's layer design (PLAN.md section
  2, "Editor rendering") and decision 9 keep the fold and inlay layers from forcing a rewrite.
- **Riskier for M4:** lldb-dap's PDB and MSVC STL support is below Visual Studio's, and building lldb-dap is new
  SDK work on both operating systems. Spike S10 runs first and stops M4 for the user's decision if it fails
  (decision 12; PLAN.md section 8, T22).
- **Slower until M16:** a cold open of an MSVC solution starts one MSBuild process per project and configuration
  instead of ProjectHost's workers, and the whole-environment fingerprint can invalidate more records than
  EnvironmentVariableRead capture would when the user's own environment changes. R2b's cold open is reported, not
  gated, in M2 as before.
- **Unchanged:** data safety, the trust model, listener hardening and the no-network test are all in U1; no budget
  target, test or warning is weakened; every P0 and P1 row that was due by Preview 0.1 still lands by then; CLAUDE.md
  does not change.
- **Done in this change:** PLAN.md (status, key decisions, the ladder, sections 1.1, 1.2, 1.4, 2, 2.2, 2.3, 3.4,
  3.5, 3.6, 4, 6.4, 7, 8, 9 and Appendix A); `tests/features/matrix.json` regenerated (46 rows change owner or
  completion, five of them the debugger rows 167, 169, 173, 175 and 178; statuses kept); `tests/perf/budgets.json`
  (B15, B16, B23); the milestone range, the run-order and completion checks and the closed M0 in
  `featurecheck.py` and `frost_lint.py`, the stricter gate checks in `test_budgets.py`, with tests
  and a ladder in featurecheck's fixture plan; the amended ADRs of decision 11; the to-do markers and comments whose
  work moved: `src/text/CMakeLists.txt` and `src/text/include/frost/text/snapshot.hpp` (large-file mode, now
  `TODO(M15)`), `tools/featurecheck.py` (the IntelliJ editor-settings checklist, now `TODO(M15)`),
  `src/app/src/main.cpp` (file associations and the AppUserModelID with the Jump List, now `TODO(M16)`),
  `tests/text/perf_test.cpp` (B15 is gated by M15's large-file mode) and the `TODO(M2)` in
  `src/io/include/frost/io/process.hpp` (path (c)'s clean environment). Every other `TODO(M1)`–`TODO(M4)` in `src`,
  `tests`, `tools` and `cmake` names work that stays in its milestone.
- **Merging with `main`:** the merge commit after this ADR's answers brings in `main` at dcfd9ae (ADRs 0014,
  0015, 0017, 0018, 0020, 0026, 0030, 0031, 0032 and 0033 with their slices). `git merge` reported two conflicts,
  resolved so that both sides survive: `src/text/CMakeLists.txt` (main's header comment and sources; `m1/encodings`
  is merged, so the to-do line is only the large-file one, as `TODO(M15)`) and `tests/features/matrix.json` (main's
  `status` and `acceptance_test` for every row, rows 38, 60, 227, 243 and 244 among them, the owners from this
  ADR, then `featurecheck.py generate`). PLAN.md (main's section 6.7 "Feature-first phase" row), `tools/frost_lint.py`
  and the rest merged cleanly. Retagged in the merge, comments and ADR prose only: the `TODO(M1)` markers for
  `m1/large-file` (now `TODO(M15)`, `m15/large-file`) in `text_file.hpp`, `text_file.cpp`, `line_separator.cpp` and
  `icon_atlas_set.hpp`, and the matching lines of ADRs 0015, 0017 and 0018; ProjectHost as M2's evaluator in
  `src/project/CMakeLists.txt`, `vcxproj.hpp` and ADR 0033 (path (c) is M2's, ProjectHost M16's). A slice that
  merges after this branch and adds a `TODO(M1)` for work this ADR moved retags it to M15, M16 or M17 before it
  merges; one that adds debugger work tags it M4 (decision 12).
- **Hand-off:** this branch does not edit `docs/progress.md`; the session that merges it records it there.

## Answers
The user answered the open questions below on 2026-11-01, in the evening: **"I agree with everything except 4 and
5. I don't care for those savings"**. So:
- **Q1 accepted:** path (c) is U1's MSBuild evaluator and ProjectHost lands in M16 (decision 6).
- **Q2 accepted:** B23's first project is gated at M2 on path (c) and at M16 on ProjectHost (decision 8). This is
  the user's approval that ADR 0006 decision 2 (A) asks for to change the M2 gate's method.
- **Q3 accepted:** M5 runs right after U1, then M15, M16 and M17 (decision 4).
- **Q4 reversed:** U1 has a minimal debugger: line breakpoints, stepping, the call stack, threads, variables and
  watches, for MSVC-built programs through their PDBs, on Windows and Linux. It moves from M6 into M4 (decision 12),
  at the cost the question named (M4 9–12 units instead of 6–7, U1 around month 10–12 instead of 9–10).
- **Q5 declined:** U1 is not thinned further. Every item Q5 listed stays in U1, as the branch already had it.

The questions as they were asked:

## Open questions (answered)
Asked before this ADR was accepted. Each had a recommendation, which the branch implemented.
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
  about 1–2 units sooner: approximate mode for solutions on Linux (D16, scenario step 12, now step 14), the .slnf
  reader, compile_commands and open folder, Search Everywhere v1 (Go to File, Class and Symbol and Find Action
  stay), terminal IME and the ja VS-output goldens. Not done unless the user asks.

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
- **Keep ProjectHost in M2.** Q1; the user accepted path (c) instead.
- **Report B23's first project at M2 without a gate** (this ADR's first draft). It leaves a feature U1 ships without
  its budget, against the constraint that U1's budgets stay gated where the feature lands; Q2, answered with the
  gate.
- **Keep the debugger in M6** (Q4's recommendation). Rejected by the user ("Answers").
- **The DbgEng adapter as U1's Windows debugger,** for natvis and full PDB fidelity. It needs the ComponentDownloader,
  network consent and a legal review first (D12, L5), all later than M4; it stays M10's, and S10 decides whether
  lldb-dap carries U1 (decision 12).
- **A debugger in U1 on Linux only.** The user's daily work is MSVC solutions on Windows (ADR 0032), so U1 debugs
  cl.exe-built programs through their PDBs.
- **A thinner U1** (Q5). Declined by the user: "I don't care for those savings".
- **Move the block-decoration layer to M15 too,** with the M1 merge view and M5's diff viewer aligning their panes
  some other way. That is a second alignment mechanism to build and later replace, and soft wrap's input would
  change when the fold layer arrives (decision 9).
- **Defer data safety or security past U1.** Excluded by the constraints: U1 is for real work on real files and on
  untrusted repositories.
