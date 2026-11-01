# ADR 0029: Thin path to a first usable build

- Status: accepted
- Date: 2026-11-01
- Plan reference: PLAN.md section 7 (roadmap, "First usable build (U1, the M4 exit)", M15–M17), Appendix A, section 4
  (B15, B23), section 1.4 (P0 rows after Preview 0.1), section 9 (D29); `tools/featurecheck.py`, `tools/frost_lint.py`

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
`m1/crash-diag`, `m1/verify-tooling` and `m1/keymap-de` are running and will land.

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
   the automation endpoint, a clean profile, the no-network recorder and the listener enumeration running throughout,
   a 2x-budget smoke on the samples it records, corpora restored and checked with `git status --porcelain`):
   - **CMake on R3:** open with the pinned `R3-build`, restricted mode, then trust; navigate `llvm/utils/not/not.cpp`;
     complete `llvm::outs()`; inject an error and see the diagnostic; kill frost and get the dirty buffer back from
     the hot-exit journal; build `not`, open the error from the Build window, fix it, build; run `not` and see the
     injected output and exit code 1; revert the file from the terminal and see the clean buffer reload; exit and
     reopen within B3 and B2. On Linux the same steps run on an R3 checkout on ext4.
   - **MSVC on R2a:** open `PowerEditor\visual.net\notepadPlus.sln` in restricted mode, trust, check clangd on
     `EncodingMapper.cpp`; select Debug|x64; inject an error, open MSVC's C2065 from the Build window, fix, build
     Notepad++, Scintilla and Lexilla; run Notepad++ with its settings in a scratch directory and stop it through the
     Job Object.
   - **Terminal:** the M4 Claude TUI probe in the same session. **Linux:** R2a's solution in approximate mode.
   The steps are in PLAN.md section 7.
3. **M1–M4 shrink to their thin cores, and their deferred halves move.** M1's go to the new milestone M15, M2's and
   M4's to M16, M3's to M17. Rider-parity polish rows that are P1 go to M11 (rulers, clipboard history, drag and
   drop, the TODO window). The LLVM SDK artifacts go to M6, where the debugger first needs them. The table below lists
   every move.
4. **Order.** M0, M1, M2, M3, M4 (U1), M5 (dogfood v0.1), M15, M16, M17, M6 (Preview 0.1), then M7–M14. M5 comes
   before the completion milestones because Git and the Claude C1 bridge are the next most valuable things to a U1
   user. M15–M17 come before M6, so every P0 row still lands by Preview 0.1 (PLAN.md section 1.4) and the five P0 rows
   that already finished after Preview 0.1 (31, 99, 118, 227, 242) stay the only ones.
5. **New milestone IDs M15, M16 and M17.** They are positions in the ladder only by section 7's order, not by number.
   `featurecheck.py` and `frost_lint.py` accept M0..M17 (`LAST_MILESTONE = 17`), and `tests/tools/test_budgets.py`
   accepts gates M0..M17. A to-do marker or a matrix owner M18 is still rejected (tests in `test_featurecheck.py`
   and the lint fixtures).
6. **Path (c) is U1's MSBuild evaluator; ProjectHost lands in M16.** `MSBuild.exe -getTargetResult` with the wrapper
   target of ADR 0005 runs the same MSBuild engine and the same design-time targets and global properties as
   ProjectHost (ADR 0005, decision 5), so M2's fidelity exit (every extracted command line compiles with `cl.exe /c`
   and matches msbuild-extractor-sample) applies unchanged, and M16 runs it again on ProjectHost. Path (c) was
   already required in M2 as the fallback, so no throwaway evaluator is built. It starts only after trust and after
   L1, which keeps the no-network-before-L1 rule without ADR 0011 B4's ProjectHost design. Its captured-environment
   fingerprint covers the whole environment block FrostIDE passes to MSBuild, a conservative superset that M16's
   EnvironmentVariableRead capture narrows. B3 (zero .NET processes on a warm reopen) is unaffected: the cache, not
   the evaluator, decides it.
7. **Interim behaviours in U1**, each replaced later and named in the milestone that replaces it:
   - a large-file guard (M1): files of 128 MB or more are refused with a notification, and a file whose longest line
     exceeds the view's cap opens read-only; large-file mode replaces it (M15);
   - settings edited as the JSON scope files, opened from Find Action, validated on save and reloaded live (M1); the
     Settings dialog replaces it (M15);
   - the native window frame (M1); the frameless window replaces it (M15);
   - build output parsed from the console by code page and locale-independent anchors (M4); the MSBuild logger adds
     structured events (M16);
   - Run builds the target first (M4); the before-launch task list generalises it (M16).
8. **Budgets.** Two gates move with the feature they measure; no target changes:
   - B15 (F1 first screen) from M1 to M15, with large-file mode;
   - B23's first-project-streamed variant from M2 to M16, with ProjectHost. Its definition names ProjectHost's cold
     start and S6 set its value (ADR 0006, ADR 0011 B1). M2 times path (c)'s first project under the same
     preconditions and reports it against the same 3 s; B23's trees variant stays gated at M2.
   Every other gate stays: B1, B5, B6, B19 (no project), B20 and B21 at M1; B2, B3, B4, B7, B13 (unindexed) and B23
   (trees) at M2; B8–B11, B19 (R3), B22, B24 (M3 value), B25 and B26 at M3; B17 at M4; B14, B18 and B27 at M5; B16 at
   M6; B12 at M7; B13 (indexed), B24 (final) and B26 (REF-L1) at M13; B28 at M0. `budgets.json` records both moves in
   the budgets' preconditions.
9. **Amended ADRs.** These decisions keep their content and move milestone: ADR 0006 decision 1 (path (b) as M2's
   plan) becomes M16's plan; ADR 0011 B1 (the S6 cold samples and the user's B23 first-project value are needed before
   M2) now applies before M16, and M2 no longer depends on D3; ADR 0011 B3 (B23's five preconditions) applies to M2's
   reported path (c) time and to M16's gate; ADR 0011 B4 (ProjectHost before L1) is M16's target. ADR 0011 B5 (the
   UWP workload in M2) is unchanged, because M2 still evaluates R2b's C++/WinRT projects through path (c). ADR 0016's
   Keymap page goes with the Settings dialog to M15; its Find Action popup stays in M1.

### Moved items
"Scope" is a PLAN.md section 7 scope item, "Exit" an exit criterion, "Gate" a budget gate, "Dep." a dependency.
Row numbers are Appendix A's. Every item is still required in its new milestone.

| # | Item | Kind | From | To | Notes |
|---|---|---|---|---|---|
| 1 | Multi-caret and column mode (row 38) | Scope | M1 | M15 | The caret and selection model stays multi-caret-capable (m1/undo-anchors); the commands and the UI move. |
| 2 | The column-mode status widget (part of row 242) | Scope | M1 | M15 | |
| 3 | Folding (row 40) and fold persistence (row 256), with the fold queries | Scope | M1 | M15 | |
| 4 | The display map's fold, inlay and block-decoration layers | Scope | M1 | M15 | M1 builds the layer stack with the soft-wrap layer. |
| 5 | Whitespace and indent guides (row 251) | Scope | M1 | M15 | |
| 6 | Ctrl+wheel editor zoom (row 253) | Scope | M1 | M15 | IDE zoom (row 244) stays in M1. |
| 7 | Extend selection (row 39) | Scope | M1 | M15 | |
| 8 | EditorConfig (row 59) | Scope | M1 | M15 | SaveManager preserves each file's encoding and EOL meanwhile. |
| 9 | Large-file mode (row 35's large-file part): mapped originals of 128 MB and more, parallel summaries, a first screen before the line index, the background release of big trees | Scope | M1 | M15 | M1 adds the large-file guard (decision 7). |
| 10 | The Structure view (row 87) | Scope | M1 | M15 | The outline queries stay in M1 for next/previous member (row 94). |
| 11 | Editor splits (part of row 55) | Scope | M1 | M15 | Tabs stay in M1. |
| 12 | The frameless window with snap layouts (part of row 239) | Scope | M1 | M15 | The native frame meanwhile. |
| 13 | Tool-window drag-docking and floating (part of row 240) | Scope | M1 | M15 | Stripes, show and hide, resizing stay in M1. |
| 14 | The Settings dialog (row 226), with ADR 0016's Keymap page | Scope | M1 | M15 | JSON scope files meanwhile (decision 7). |
| 15 | Keyboard navigation of stripes, toolbar, status bar and gutter (row 246's first part) | Scope | M1 | M15 | |
| 16 | The UIA TextPattern smoke | Scope | M1 | M15 | |
| 17 | Rulers (row 252) | Scope | M1 | M11 | Rider-parity polish, P1. |
| 18 | Clipboard history (row 254) | Scope | M1 | M11 | Rider-parity polish, P1. |
| 19 | Drag and drop (row 255) | Scope | M1 | M11 | Rider-parity polish, P1. |
| 20 | B15 met | Exit, Gate | M1 | M15 | Target unchanged. |
| 21 | F2 editable | Exit | M1 | M15 | M1 tests instead that F2 opens read-only without a stall over 50 ms. |
| 22 | Focus-order tests | Exit | M1 | M15 | |
| 23 | The display-map property test for the fold, inlay and block layers | Exit | M1 | M15 | M1 runs it for the soft-wrap layer. |
| 24 | ProjectHost (path (b)): 2–4 workers, EnvironmentVariableRead capture, the warm-up (ADR 0006), the launch before L1 (ADR 0011 B4) | Scope | M2 | M16 | Path (c) is M2's evaluator (decision 6). |
| 25 | `.sln`↔`.slnx` conversion (row 3) | Scope | M2 | M16 | Needs SolutionPersistence in ProjectHost. |
| 26 | New Project/Solution (part of row 26) | Scope | M2 | M16 | Writes through ProjectHost. |
| 27 | The first-run wizard (part of row 26), with the Dev Drive check (D19) | Scope | M2 | M16 | The Welcome window stays in M2. |
| 28 | C# projects in the tree (section 1.2; part of row 27) | Scope | M2 | M16 | |
| 29 | The Unload/Reload project and Properties actions (parts of row 31) | Scope | M2 | M16 | Set as Startup Project, Open in Explorer and Copy Path stay in M2. |
| 30 | Show All Files and nesting (row 30) | Scope | M2 | M16 | |
| 31 | `frost --wait`, `frost diff`, `frost merge`, file associations, the Jump List (part of I10) | Scope | M2 | M16 | Single instance and `frost <path>` stay in M2. |
| 32 | The TODO window (row 115) | Scope | M2 | M11 | Rider-parity polish, P1. |
| 33 | B23's first project streamed met | Exit, Gate | M2 | M16 | Target and preconditions unchanged; path (c)'s time reported at M2; B23 trees stays gated at M2. |
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
| 51 | The before-launch task list (row 162) | Scope | M4 | M16 | Build-before-run stays in M4. |
| 52 | Gutter run icons for `main()` (row 164) | Scope | M4 | M16 | Still completes in M6. |
| 53 | The CMakeLists language service (row 66) | Scope | M4 | M16 | |
| 54 | The CMake cache viewer (row 13) | Scope | M4 | M16 | |
| 55 | LLVM SDK artifacts where S5 found gaps | Scope | M4 | M6 | Merged with M6's Windows lldb-dap open item. |
| 56 | R5 builds from inside the IDE | Exit | M4 | M16 | R2b and R3 still build in M4. |
| 57 | CMake completion and navigation goldens on R3's CMakeLists | Exit | M4 | M16 | |
| 58 | Built SDK artifacts stored under E:\sdk and `~/sdk` and pinned in `sdk/sdk.lock.json` | Exit | M4 | M6 | |
| 59 | D27 (ProjectHost runtime) | Dep. | M5 | M16 | M5's packages carry no ProjectHost yet. |
| 60 | D17's conversion part | Dep. | M2 | M16 | Solution editing stays in M11. |

**Added, not moved:** the U1 definition and acceptance scenario (M4); the large-file guard and its tests (M1); the
scope files opened from Find Action (M1); path (c) as the evaluator, with its ProjectModelCache fingerprints and the
reported B23 first-project time (M2); M4 depends on M3; M15's check that B5 and B6 hold with folds, inlays and several
carets; M16's malicious-repository test for imported run configurations and before-launch tasks; M17's suppression
and resolve-context tests; M11's tests for rows 115, 252, 254 and 255; the dependencies of M6 on M15–M17, of M7 on
M17, of M9 on M16 and of M11 on M15.

### Units
| Milestone | Before | After | Ends around month |
|---|---|---|---|
| M1 | 11–13 | 7–8 | 3–4 (was 4–5) |
| M2 | 12–14 | 8–9 | 5–6 (was 7–8) |
| M3 | 10–12 | 7–8 | 7–8 (was 10–12) |
| M4 (U1) | 9–11 | 5–7 | 8–10 (was 12–15) |
| M15 | — | 5–6 | 12–15 |
| M16 | — | 7–9 | 14–17 |
| M17 | — | 5–6 | 15–19 |
| M6 | 11–13 | 12–14 | 18–23 (was 17–21) |
| M11 | 9–11 | 10–12 | 30–37 (was 29–35) |
| All milestones | 120–145 | 124–150 | M14 at 34–41 (was 33–40) |

With the upgrade track's 9–11 units and 10% contingency the roadmap is 146–177 weeks, about 2.8–3.4 years (was
142–172 weeks, 2.7–3.3 years). Months follow PLAN.md's model: cumulative units with contingency and the upgrade
track's share, month 1 being November 2026.

### M1 slices
M1's remaining slices change as follows; the running ones keep their scope.
- `m1/large-file` moves to M15 whole. Its guard goes to `m1/shell-nav` (the open path) with the cap from
  `m1/editor-view`.
- `m1/settings` loses EditorConfig (M15) and gains the scope files opened from Find Action with validation on save.
- `m1/syntax` keeps the highlight and outline queries; the fold queries go with folding to M15.
- `m1/display-map` builds the layer stack with the soft-wrap layer only, and no longer waits for `m1/syntax`.
- `m1/editor-model` loses the multi-caret and column-mode commands, extend selection and the clipboard-history model.
- `m1/editor-view` loses whitespace and indent guides, rulers, Ctrl+wheel zoom, drag and drop and "F2 editable".
- `m1/shell-frame` loses the frameless window, splits, drag-docking and floating, the column-mode widget and keyboard
  navigation with its focus-order tests; it has no on-screen step left.
- `m1/shell-nav` loses the Settings dialog and the Structure window.
- `m1/automation-bench` loses B15 and the UIA TextPattern smoke.
- New: `m1/startup-b1` (B1 warm still misses 150 ms, ADR 0011 A3; the logon harness of A4 that also collects the S6
  cold samples of B1) and `m1/keymap-fuzz` (the `input_keymap` fuzz target, ADR 0016).

## Measurements
No new measurement. The estimates are judgement, made the same way as PLAN.md's. Path (c)'s cost is from ADR 0005: a
fresh MSBuild process per project and configuration has a floor of about 0.3–0.4 s (p50 331–405 ms across three R2b
projects and R2a, warm file cache, REF-W1), against ProjectHost's warm workers.

## Consequences
- **Sooner:** U1 around month 8–10, 4–5 months before the old M4 exit and 2 months before the old "first usable
  checkpoint" (M3), which had no build, run or terminal. M2 no longer waits for D3 or the S6 cold samples.
- **Costlier:** 4–5 more units: three more milestone exits (demo packs, `-Full` and nightly runs, comment-accuracy
  reviews, sign-offs) and the interim behaviours of decision 7, which later milestones replace. The display map's
  layer design (PLAN.md section 2, "Editor rendering") is what keeps the fold and inlay layers from forcing a rewrite.
- **Slower until M16:** a cold open of an MSVC solution starts one MSBuild process per project and configuration
  instead of ProjectHost's workers, and the whole-environment fingerprint can invalidate more records than
  EnvironmentVariableRead capture would. R2b's cold open is reported, not gated, in M2 as before.
- **Unchanged:** data safety, the trust model, listener hardening and the no-network test are all in U1; no budget
  target, test or warning is weakened; every P0 row still lands by Preview 0.1; CLAUDE.md does not change.
- **Done in this change:** PLAN.md (status, key decisions, the ladder, sections 1.1, 1.2, 1.4, 2, 2.2, 3.6, 4, 7, 8, 9
  and Appendix A); `tests/features/matrix.json` regenerated (41 rows change owner or completion; statuses kept);
  `tests/perf/budgets.json` (B15, B23); the milestone range in `featurecheck.py`, `frost_lint.py` and
  `test_budgets.py`, with tests; the to-do markers whose work moved: `src/text/CMakeLists.txt` and
  `src/text/include/frost/text/snapshot.hpp` (large-file mode, now `TODO(M15)`) and `tools/featurecheck.py` (the
  IntelliJ editor-settings checklist, now `TODO(M15)`). Every other `TODO(M1)`–`TODO(M4)` in `src`, `tests`, `tools`
  and `cmake` names work that stays in its milestone.
- **Merging the running slices:** expect conflicts in `tests/features/matrix.json` (rows whose owner moved and whose
  status a slice changed, such as row 38 on `m1/undo-anchors`), in the to-do line of `src/text/CMakeLists.txt`
  (`m1/undo-anchors`, `m1/encodings`) and possibly in `tools/frost_lint.py`'s header (`m1/theme-icons`). Resolve them
  by keeping both sides' intent: take the slice's status and acceptance fields, then run `featurecheck.py generate`;
  keep the `TODO(M15)` large-file line. A slice that adds a `TODO(M1)` for work this ADR moved retags it to M15, M16
  or M17 before it merges.
- **Hand-off:** this branch does not edit `docs/progress.md`; the session that merges it records it there.

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
- **Keep ProjectHost in M2.** About 2–3 more units before U1 (the C# host, StreamJsonRpc, its NuGet and licence
  checks, a supervisor protocol and its fake) for the same command lines path (c) yields. Kept as a question for the
  user: the reorder takes path (c) for U1.
- **Defer data safety or security past U1.** Excluded by the constraints: U1 is for real work on real files and on
  untrusted repositories.
