# FrostIDE progress log

Session hand-off log (CLAUDE.md, PLAN.md section 6.7). Newest entry first. Each entry lists what was done,
what is next, blockers and open questions for the user.

## 2026-11-03: Unity-controls probe preference preservation issue

- The accepted staged-window bridge evidence below remains valid. A subsequent, separate Unity-controls
  worktree probe failed before announcing its session; it sent no control command and is not acceptance.
- That later probe detected one global EditorPrefs value outside its six-value restoration list:
  `UnityConnectUrlConfiguration_h3045770047`. Its exact typed prelaunch value was retained only in the
  exited Python process, so it cannot now be restored reliably. Older exports are not an exact baseline and
  were not applied. The current value is retained locally under current-user DPAPI encryption, without printing
  its contents. The user was informed. Do not claim all preferences from this controls probe were restored.
- Further live Unity probes are on hold pending review of the new harness's durable encrypted full-key
  snapshots and conservative restoration. It fsyncs the typed baseline before launch, preserves unrecognized
  or concurrently changed values, and checks the complete key afterward. A no-Unity DPAPI self-test passed.
  Harness: `E:\FrostBuilds\unity-controls-scratch-20261103\run-control-durable.py`.
  This does not recover the missing earlier value. Source work and builds continue; the full goal stays active.

## 2026-11-03: Staged Unity receiving-window acceptance and follow-up findings

- Verified Git/Unity checkpoint `c7231b7` was committed and pushed to `origin/main`. The whole project goal
  remains active; development continues in three isolated Terra worktrees with Astra review.
- A subsequent real Unity request reused the actual staged FrostIDE application and existing clean tab on a
  private Windows desktop. The app exited normally and persisted one tab at line 3, display column 39,
  matching Unity's UTF-16 column 40 after the fixture's astral character. Source/meta bytes stayed unchanged;
  six EditorPrefs values were restored with zero whole-key differences. The stage and source are unchanged.
  Evidence: `E:\FrostBuilds\unity-stage-window-20261103\final-output.log` and
  `trusted-final/result.json`, with application/Unity logs and isolated profile alongside them.
- Probe corrections are explicit: the initial fresh untrusted profile stopped at a trust dialog, before broker
  registration. The successful probe trusts only its owned scratch root and waits for the recorded first frame.
  A second oracle incorrectly expected a byte column in the session JSON, whose column is the display/grapheme
  count; the corrected fixture-specific oracle checks that coordinate. Python's private-desktop wrapper needed
  explicit UTF-8 mode to retain Unicode diagnostics. No product change or trust decision in the user's profile
  was made. Dirty-buffer handoff continues to have separate Workbench-rig evidence.
- Source inspection found a likely existing restoration/navigation mismatch: `DocumentEditorView::viewState`
  saves a display column, but `restoreViewState` calls the byte-column `goToLine` API. Navigation history has
  the same pattern. Terra's editor agent is testing and correcting this as functional session work.
- The actual GUI probe also retained a 2678 ms main-thread stall during project scanning on the loaded Debug
  host. Symbolized FrostIDE frames lead through `ui::Workbench::pollScan` and navigation-path indexing; this
  is a diagnostic observation, not a controlled B5 measurement. Retained trace:
  `build/unity-stage-ui-freeze-symbolized.log`, with the source freeze JSON under the probe's `trusted-run`
  profile. Incremental/background scan publication needs follow-up before full product completion.
- Astra corrected an earlier folding-review premise: new ADR 0069 introduced the eager O(log f + k) metadata
  formula; it was not a baseline B5 requirement. The baseline requires logarithmic coordinate mapping, local
  display patches, unchanged-line reuse and the unchanged numerical latency targets. The agent now addresses
  real local-update fallbacks and repeated endpoint-group reconstruction, documenting actual metadata cost.
  No user budget was weakened. Folding, Lua debugging and Unity controls remain unaccepted work in progress.

## 2026-11-03: Verified Git workbench and Unity bridge checkpoint

- Git worker tree `8d2097b8d6403e3ab3972e97914cbbdea9bc0c7a`, its exact test-label inventory
  correction `2996b4ca40e82b6202e7c80c6030e7165d1e3707`, and Unity worker tree
  `f9122c32ba44ea52f1c3d2318935fe69d81ddbbf` received Astra review. Astra approved the combined
  join at `50ee225d221b6eb6b654b22c99541d4977d89b98`. A final clang-format-only correction produced
  source tree `e295c5a0497a2a6e1db194bfe305749b812c83cb`; Astra independently verified this narrow
  delta and the evidence boundary. Subsequent changes are documentation only.
- Combined native ext4 Linux verification passed 2551/2574 (23 skipped), including real LuaLS and C#
  integration. The first combined Windows run passed functional tests but found one formatting violation.
  After correction and rebuild, the full Windows gate passed 2677/2696 (19 skipped, 1 disabled), format,
  frost-lint (2371 files), licence checks (103 Windows / 99 Linux packages) and feature-matrix consistency.
  The formatting correction is entirely in the Windows conditional branch; Linux behavior is unchanged.
  Logs: `build/verify-main-integration-20261103-003948.log`,
  `build/verify-git-unity-windows-20261103-final-tests.log` and
  `build/verify-git-unity-native-20261103-tests.log`. Skipped real-engine/debugger scenarios retain their
  earlier separate evidence; neither skips nor matrix consistency are counted as completed features.
- Development stage `E:\FrostBuilds\preview\codex-20261103-git-unity` passes waited startup,
  actual Windows QRhi/D3D11 rendering and the staged ConPTY Shift+Enter test. Its application DLL matches
  the tested build: SHA-256 `AE7FAD1148F7453AC521EBB65A802A59583C85528982AE7E752A96EC7269A824`.
  All 13 staged Unity package files match main source. Logs/profile are in the sibling `-smoke` directory.
- An actual Unity 6000.6.4f1 request through the unchanged main client and Workbench test rig passed 1/1:
  dirty-tab reuse, the exact UTF-16 location with astral and unsaved non-ASCII text, and source/meta preservation.
  Six EditorPrefs values changed and were restored, leaving zero whole-key differences. The built executable
  and DLL match the stage. This does not establish a staged receiving application window. Evidence lives in
  `E:\FrostBuilds\unity-stage-acceptance-20261103`. The first new harness profile hid Unity's existing
  entitlement (exit 198); inheriting the existing HOME/USERPROFILE while isolating application data resolved
  it without activation or sign-in. Existing package meta files were preserved; Unity's trailing YAML spaces
  are ignored only when checking their equivalence in the scratch harness.
- Git now connects discovery, trust, status/markers, dirty-buffer diff, selected commits and branch/push
  controls. Broader M5 operations remain. Unity now supplies an original external-editor package and exact
  generation-based managed context; full compiler options, project/package UI, Linux Unity Editor acceptance,
  controls, logs, tests, debugging and wider Unity workflows remain. Language tools remain external to the stage.
- Continue the entire agreed project. Folding's functional draft is under Astra architectural review because
  its strict incremental update bound is still unmet. Lua debugging remains under development in isolated IDE
  and engine worktrees; the first engine test build is not yet linked. Unity controls have started in
  `codex-unity-controls`, with inherited staged baseline `e295c5a0497a2a6e1db194bfe305749b812c83cb`.
  No budget was weakened and no milestone checkpoint is full-project completion. No user answer blocks work.

## 2026-11-03: Reviewed Git and Unity candidates integrated for final gates

- Last verified commit remains `3e5df23`. Git tree `8d2097b8d6403e3ab3972e97914cbbdea9bc0c7a` and
  Unity tree `f9122c32ba44ea52f1c3d2318935fe69d81ddbbf` have Astra approval for their respective slices.
  Both source deltas are now staged together. Three merge conflicts retained all existing Find/Git shutdown
  cleanup and advanced Lua tests while adding Unity tests and the context-revision check.
- The first Git-only combined run built successfully on both platforms and failed only the test-label inventory:
  Windows 2670/2689 (18 skipped, 1 disabled, 1 failed), native 2542/2565 (22 skipped, 1 failed).
  Product tests passed. Terra added the exact `WorkbenchGit.` prefix to the expected Git suites without weakening
  kind/tag/lock assertions; Astra approved the one-file correction in tree
  `2996b4ca40e82b6202e7c80c6030e7165d1e3707`. It is now included before final combined gates.
- Unity publisher reads use bounded verified regular-file handles; validation precedes the unchanged-content
  return. POSIX publication uses anchored directory descriptors. Windows pins ancestors with GENERIC_READ and
  read-only sharing: a real FSCTL test proved attribute-only handles insufficient, while the stronger pin rejected
  junction conversion. Actual Unity Mono synchronization, live-junction rejection, failed-generation preservation,
  fresh player/editor semantics and Windows dirty-generation replay pass. Linux portable publisher tests cover
  FIFOs, live links, aliases and ancestor swaps. EditorPrefs changes were restored and compared after each run.
- Apparent source-path and Unicode-definition failures were probe interpretation errors: real compiler execution
  preserves distinct percent/space source files, and decoded URI identity preserves the original Unicode path.
  MSBuild DLL-reference escaping does require the reviewed design-time reference handling. A final-file swap at
  File.Replace was not directly reproduced; Astra found no confirmed defect and recorded this validation limit.
  Actual Linux Unity Editor, unsupported compiler arguments/response files/analyzers and wider Unity tracks remain.
- Folding continues in its isolated worktree with Astra's indexed-projection/nested-range design. Functional
  milestones are passing, but the complete incremental contract and final application/platform acceptance remain.
- Lua debugging now has a Terra implementation agent and Astra design assistance. IDE worktree `codex-lua-debug`
  has staged baseline `bda856487baf348ebfbf3d6ef74400c7c092b67a`; isolated engine worktree
  `D:\dev\frostide-lua-debug-engine` branches from `8237e1ecfeb9456c231ea30e41c23c75cd426a2d`.
  Transport, cooperative hooks, watchdog preservation and source identity are under development; no Lua debugger
  is accepted yet. The canonical engine checkout remains untouched. The whole agreed project goal remains active.

## 2026-11-02: Reviewed Git candidate staged; Unity and folding continue

- Last verified commit is `3e5df23` (advanced Lua editing). Astra approved final Git worker tree
  `8d2097b8d6403e3ab3972e97914cbbdea9bc0c7a` against baseline
  `6e48fa0ea4f1cc18c75586327c654d87b2dd7af9`. Its source/ADR delta applied cleanly to main, without
  replacing the progress log. Combined-tree gates have not yet run; this is not an accepted new checkpoint.
- The integrated candidate connects repository discovery, trust, Commit/diff/branch controls, local status
  refresh, editor/tree colours and dirty-buffer workflows. Review corrections prevent pre-trust filter execution,
  stale queued operations and hunk rollback, lost recovery indexes and same-size edits hidden by changed index
  timestamps. Timestamped atomic writes preserve the conservative timestamp before publication.
- A failed/interrupted commit with changed HEAD leaves the real index untouched and retains private recovery
  files: an independent ref update means provenance cannot safely be inferred. Windows exceptional replacement
  failures retain original and replacement at explicit reported paths. Final linked Windows/Linux binaries each
  pass seven real-Git recovery/provenance cases and two filesystem cases; Astra independently reran the external
  ref regression and filesystem tests. Exceptional Windows OS errors were source-reviewed and their decisions
  simulated, not induced in the OS. Ambiguous recovery remains manual.
- Unity's real fresh generation and player language-server checks preserve distinct literal-percent and space
  DLLs. Actual compiler execution proved the apparent source-path problem was MSBuild display unescaping;
  original Compile item generation is retained. Fresh editor-profile and extended-path checks/re-review remain.
- Folding has a functional draft and native mapping/input evidence, but its vector-based per-edit rebuild
  violates the incremental complexity contract. Astra design assistance will precede replacement with indexed
  projection/anchor structures; latest Windows verification and complete workflow acceptance remain.
- Next: complete Unity review, combined Windows/Linux gates and staged smoke checks; continue the entire PLAN.
  Another engine checkout's CPU-heavy tests have slowed builds. No unrelated process was changed, and no user
  answer blocks progress.

## 2026-11-02: Verified advanced Lua editing checkpoint; Git, Unity and folding continue

- Advanced Lua editing is integrated and independently approved (Astra reviewed worker tree
  `0cadc6a33cc9d324a3de8437ec6f60fcbd47eac1`). The combined source snapshot
  `a671d05418f450da59cccb86830409d47db42c07` passed Windows 2636/2654 (18 skipped, 1 disabled) and
  native ext4 Linux 2508/2530 (22 skipped), with real LuaLS 3.19.1, generated FrostEngine API and the reviewed
  C# server enabled. Format, frost-lint (2345 files), licence checks (103 Windows / 99 Linux packages) and
  feature-matrix consistency pass. Only documentation was updated after these gates.
- Effective formatting distinguishes indentation width from tab width and rejects stale settings/document
  responses; real document/range formatting, undo/redo, snippets and keyboard parameter hints pass.
  Lua-specific UTF-16 negotiation addresses the independently reproduced LuaLS legacy UTF-8 failure.
  Non-BMP characters before actual edit/signature positions are tested; C++/C# encoding offers are unchanged.
- Development stage: `E:\FrostBuilds\preview\codex-20261102-lua-editing`. Its application DLL hash matches
  the tested build. Waited startup and real Windows QRhi/D3D11 render selftests report PASSED; the staged
  ConPTY Shift+Enter/input test passes 1/1. Logs/profile are in the sibling `-smoke` directory. Language
  tools remain external; this is not a bundled release or full-project completion.
- Git remains isolated. Astra approved the pending-index recovery and hunk-generation fixes, but a broader
  real-Git run then exposed a production racy-index defect: copying the index advances its timestamp and can
  hide same-size edits. Deterministic reproductions confirm it; conservative timestamp handling and atomic
  publication are being corrected and require fresh review/platform evidence before integration.
- Unity remains isolated. Actual Unity and managed-semantic evidence exists, but MSBuild reference resolution
  decodes literal percent escapes again after item evaluation. End-to-end metacharacter handling and the
  trust-gated, no-follow/nonblocking metadata reader are still being completed and reviewed.
- Interactive folding is assigned to Terra in `codex-editor-folding`, baseline
  `a671d05418f450da59cccb86830409d47db42c07`. The source audit clarified that the fold display layer was
  an identity placeholder and only Lua supplied syntax ranges. Initial C++ ranges and mapping tests are
  development evidence only; mapping, editor UI, persistence and review remain required.
- Continue the whole agreed product scope. Broader Lua semantics/debugging, remaining Git/Unity workflows,
  editor/navigation parity and the wider PLAN tracks are unfinished. No user answer blocks current work.

## 2026-11-02: Git, Unity and advanced Lua editing in progress

- Last verified commit is `12fb20f`; the approved advanced Lua delta is now staged on main for combined gates.
  The user reiterated that work continues until the whole agreed project is complete; checkpoints do not replace
  the full scope or completion audit.
  Three Terra agents implement isolated slices, followed by Astra review and combined Windows/Linux gates.
- Git (`codex-git-workbench`, baseline `6e48fa0ea4f1cc18c75586327c654d87b2dd7af9`): initial real application
  checks cover dirty-buffer/HEAD/index diff, selected commit, hook failure, project switching, explicit trust,
  branches and local bare-remote push. Further cancellation/concurrent-edit cases and native verification are
  passed on both platforms. Astra exposed clean-filter execution before trust, stale queued preflight, cancellation
  cleanup and request-metadata defects. Corrections use restricted no-filter plumbing and a private commit index;
  re-review found post-commit cancellation/publication recovery and displayed-hunk identity defects still requiring
  correction. No Git delta is accepted for integration yet.
- Unity (`codex-unity-bridge`, baseline `a54162283eab2111c4d94a46fba00f45b1b7e2a3`): the original editor package
  compiles in installed Unity 6000.6.4f1. Real scratch probes check asmdef/source/Unity reference generation,
  explicit resync after source/assembly additions, unchanged source metadata, exact Unicode/spaces file and
  caret arguments, and failed-publication preservation of the prior generated context. A fresh generation
  loads in the reviewed csharp-ls and supplies Unity Vector3 completion without manual restore. This is direct
  component evidence. Windows/Linux synthetic Workbench reload and late-response tests now pass, as does actual
  Windows Unity-to-FrostIDE reuse of a dirty tab with an astral/unsaved prefix. New explicit UTF-16 CLI/IPC locations
  convert against the live buffer and are rejected by legacy owners. Actual compiler comparisons verify full
  non-project reference paths (301 player / 307 editor); real editor-to-player navigation and invalid-API diagnostics
  pass. Generated context uses Unity's own API profiles, root workspace plus explicit solution and versioned
  publication. Astra requires MSBuild literal escaping and safe, trust-gated regular-file metadata reads before
  approval. Scratch runs restore each changed Unity registry preference and compare values.
  Managed debugging, Unity control/test integration and broader Unity semantics remain required later work.
- Lua (`codex-lua-editing`, baseline `6e48fa0ea4f1cc18c75586327c654d87b2dd7af9`): Astra approved corrected tree
  `0cadc6a33cc9d324a3de8437ec6f60fcbd47eac1`. Effective formatting retains independent indentation/tab widths
  and rejects stale responses after either changes. Completion handles only the local parameter-hint command.
  An independently replayed LuaLS 3.19.1 legacy UTF-8 negotiation failure required Lua-specific UTF-16 offers;
  C++/C# offers remain unchanged, conflicting/unoffered selections are rejected. Astra caught a width mismatch
  and accidental ASCII question marks in intended Unicode fixtures; final regressions use explicit non-BMP UTF-8
  before actual edit/signature positions. Windows focused checks plus shutdown and final native 29/29 pass,
  with no skipped native cases. Source applied cleanly to main; full combined verification remains.
- PLAN Appendix B.2 now records a source-grounded editor/navigation acceptance inventory against `12fb20f`,
  using public Rider behavior references. The original generated feature matrix is consistent but has incomplete
  acceptance fields; its green consistency check is not a full-project completion claim. Inventory review remains.
- Next: finish each slice's real application and platform evidence, Astra review/corrections, then integrate
  only baseline-relative deltas and rerun combined gates. Preserve inherited worktree indexes. No user answer
  blocks current work; the broad goal remains active.

## 2026-11-02: Verified debugger, engine controls, managed services and Find checkpoint

- Base: verified checkpoint `281a258`. Debugger, FrostEngine automation, managed-language and Find deltas are
  integrated on main and passed the final full functional gates below. GPT-6.1 Terra implemented the slices;
  GPT-6 Astra approved each correction and the combined lifecycle wiring. Merge conflicts in Workbench teardown
  retained every controller's cleanup. Canonical project/window identity fixes and original worktrees are preserved.
- Debugger: Terra connected DAP, breakpoints, editor markers, Debug UI, MSBuild and CMake build-before-debug,
  stepping, inspection and stopping. Shared JSON/framing now lives in `frost::rpc` with `frost::lang` aliases.
  Astra required fixes for queued launches after Stop, startup callback reentrancy, stale variable/frame replies,
  model reset during a context menu, Run/Debug console ownership, rejected step handling, pending rerun across
  project changes/modal save prompts, and editor teardown. All corrections passed focused platform tests and
  Astra approved the final source. A native backtrace identified the null editor access; no assertion was weakened.
- Actual pinned R2a Notepad++ debugger acceptance passed through the Workbench in private profile
  `E:\FrostBuilds\debugger-r2a-acceptance\run-20261102-210426`: verified `wWinMain` breakpoint, CRT stack,
  arguments in Locals, step into/out/over, watch, Evaluate, visible window, Pause and Stop. Both owned processes
  exited and temporary corpus settings were removed. This is the debugger acceptance step, not all of U1.
  An earlier Globals expansion took over 60 seconds; fast Locals inspection does not establish formatter parity.
- Engine automation: Astra approved the contained helper that performs discovery, authentication, live identity
  checks, commands and physical navigation-path validation. Retained task handles, reactor/pool shutdown order,
  permanent shutdown gating, alias record discovery, result clearing and bounded retention address review findings.
  The 4 MiB budget measures retained UTF-8 display text, not widget/metadata overhead. Cancellation does not undo
  an engine action; the engine's separate identity-check/action requests cannot atomically bind project identity.
- Actual packaged FrostEditor acceptance uses scratch projects/configuration and isolated IDE profiles: engine
  script opening reuses the staged IDE window at line 3/column 5; IDE controls exercise Edit/Play/Edit, validate
  unsaved invalid Lua without writing it, refuse mismatched projects, run a real failing Lua test and navigate
  real failure/log locations. Stalled-helper/socket shutdown and in-flight trust revocation also passed. Native
  focused tests include the two-record symlink alias case. No target engine checkout or normal preferences changed.
- Managed services passed Astra's final review and are integrated from `codex-managed-language`. Real csharp-ls/.NET service
  and editor tests pass on Windows/Linux, including two-project unsaved UTF-16 navigation/rename, diagnostics,
  completion, restart, trust and shutdown. Astra caught an installer path escape, omitted build-host dependencies,
  late startup after shutdown and environment-isolation assumptions. Archive/host provenance corrections were
  independently verified; preserving Unix NuGet configuration and guarding Windows test migration are the latest
  corrections. The development stage does not bundle the SDK/server. No Unity semantic/build/debug claim is made.
- Git components remain separate from the current Workbench. Its next integration must handle live trust,
  cancellation of reads, enclosing/linked worktrees and dirty-document safety across direct widget actions.
  Component test counts are not application acceptance.
- First combined gates: Windows **2604/2625 (18 skipped, one disabled)**; native WSL **2476/2501 (22 skipped)**.
  Each reported two engine application test failures and a managed application teardown assertion. The engine
  tests counted unrelated language-server process launches; the teardown failure was diagnosed as described below.
  Windows format, licence checks (103 packages) and featurecheck passed; frost-lint found ten engine-file
  purpose-header violations. Native licence checks passed (99 packages). These are failed gates, not acceptance.
  Terra corrected engine assertions to select helper requests by JSON method/path and added the missing purpose
  comments: formerly failing Windows tests pass 2/2; native engine suite passes six with five external-fixture
  skips; frost-lint reports zero violations across 2341 files. Astra approved those corrections and an additional
  trust-switch assertion fix that filters engine requests after trust while retaining the no-child-before-trust check.
  The teardown assertion reproduced when LuaLS was on PATH, as in the full gates: the trust timer restarted Lua
  after shutdown while C# already guarded against it. Terra stopped/gated the timer and guarded startLua, preserving
  the reactor assertion and removing temporary ownership probes. Windows focused checks pass for both real
  services, a deterministic queued-shutdown regression and the registered managed installer suite. Native checks
  also pass the same four tests, and Astra approved the correction. Earlier retries without LuaLS had not
  reproduced the actual environment. The reactor assertion and I/O/LSP implementation are unchanged.
- Combined main also passed the opt-in real Notepad++ R2a debugger acceptance, independently of the above
  suites, in `E:\FrostBuilds\debugger-r2a-acceptance\main-20261102-reviewed` (one test, 7.3 seconds).
  The test verified the actual debuggee and adapter exited. The earlier feature-worktree evidence remains above.
  The combined main application's actual FrostEditor acceptance also passes 3/3 using scratch configuration,
  profiles and projects: unsaved Lua validation/live state, mismatched-project refusal, and real failing test/log
  navigation. The harness checks the saved script remains unchanged and terminates its engine process.
- Find/Replace is integrated; its merge conflict was shutdown, resolved by retaining both the Lua timer stop and
  Find cleanup. Astra approved the mechanical join at source tree `58385999e6c3004b0a330674771177e239877de6`.
  Find focused checks pass on both Windows/native Linux: 11 workbench cases, 26 search cases, six Find UI cases
  and one real clangd diagnostic-clearance case. Astra approved staged tree
  `c585a574ab7c1606f9f0f450b36aa862ca66d38d` against baseline `6da69bb4bfb5b92a44d2b300c1b3a7c146bd03b5`.
  Review corrections destroy the Find feature on shutdown, use replacement text entered after results arrive,
  and immediately reject stale-result replacement during query/scope debounce.
- Final full gates on frozen tree `6e48fa0ea4f1cc18c75586327c654d87b2dd7af9`: Windows **2625/2643
  (18 skipped, one disabled)**; native WSL **2497/2519 (22 skipped)**. Windows format, frost-lint (2344 files),
  licence checks (103 packages) and featurecheck pass; native licence checks pass (99 packages). Both legs used
  actual LuaLS 3.19.1 with the target engine API and the reviewed csharp-ls/.NET runtime. Skips are not passes.
  Tidy, sanitizers and stress remain deferred; full U1 and complete product parity are not established.
- Debug preview `E:\FrostBuilds\preview\codex-20261102-debug-engine-managed-find` contains 62 files (258.9 MB).
  Its app DLL matches the tested build. Explicitly waited startup and actual Direct3D11 RTX 4070 SUPER render
  checks pass; the staged ConPTY DLL/host and Shift+Enter/surrogate input check passes. Initial PowerShell GUI
  invocation produced empty reports, so the recorded evidence comes from the subsequent waited Python subprocess
  checks (`startup-waited.log` and `render-waited.log` in the adjacent `-smoke` directory). LuaLS and the managed
  SDK/server are external tools in this development stage, not bundled delivery acceptance.
  With Find included, actual R2a debugger acceptance passes again (9.0 seconds) in
  `E:\FrostBuilds\debugger-r2a-acceptance\main-20261102-find-reviewed`; corpus settings are cleaned and both
  owned processes exit. Actual FrostEditor opens/reuses this new staged IDE at line 3/column 5, and all three
  engine-control/navigation tests pass again against the final main build with scratch projects/configuration.
- Next: Git, the original Unity bridge and advanced Lua editing are active in separate Terra worktrees.
  Git (`codex-git-workbench`) and Lua (`codex-lua-editing`) use staged baseline
  `6e48fa0ea4f1cc18c75586327c654d87b2dd7af9`. Unity (`codex-unity-bridge`) uses staged source
  baseline `a54162283eab2111c4d94a46fba00f45b1b7e2a3` (transfer only its later delta). Installed editor
  `C:\Program Files\Unity\Hub\Editor\6000.6.4f1` is available for scratch-project acceptance.
  The original editor package compiles and a real scratch-editor probe verifies generated asmdef/source/Unity
  reference context. Actual C# semantics and open-window reuse are not yet established; Unity preferences changed
  during scratch launches were individually restored from prelaunch values and compared. Public API references are Unity's
  [IExternalCodeEditor](https://docs.unity.com/en-us/engine/6000.6/script-reference/unity/codeeditor/iexternalcodeeditor)
  and [CompilationPipeline.GetAssemblies](https://docs.unity.com/en-us/engine/6000.6/script-reference/unityeditor/compilation/compilationpipeline/getassemblies).
  No Unity integration from that worktree is accepted or integrated yet.
  Debugger/automation worktree baseline is `c99d689f4413d29934f465bcf2bce1689dfc075b`; managed baseline is
  `59548a6120f8a0bf9663b40b05a4686f012233da`. Transfer only baseline-relative feature deltas. No user answer blocks work.

## 2026-11-02: Verified terminal, CMake, Lua syntax and engine-opening checkpoint

- Base: verified checkpoint `84ae6ba` below. Four individually Astra-reviewed slices are integrated on main.
  Astra also approved the final combined tree. Mechanical unions retain Lua project/session behavior, terminal and
  CMake lifecycle ownership, both native IPC and CMake application sources, and all dependency notices.
- Terminal (ADR 0051): current project/trust context, bounded output retention, complete UTF-16 input,
  queued-launch shutdown guards and pinned bundled ConPTY. Actual Windows console records verify
  Shift+Enter and surrogate input; native Linux verifies kitty Shift+Enter. Claude prompt compatibility,
  real IME input and the complete keyboard-layout matrix remain unverified.
- CMake (ADR 0057): configure/profile/target UI, build diagnostics, compilation database and Run in the
  current Workbench. Astra approved per-request completion callbacks, completion-time artifact/model
  snapshots, profile generations and shutdown gating. Focused Windows tests passed 13/13; combined native
  application tests also pass. One earlier interrupted wait remains unexplained; later passes are not a
  diagnosis. Separate deterministic coverage for every shutdown phase remains outstanding.
- Shared trust dialog: cancellation now respects its purpose, so Lua Cancel/close/Escape cannot cancel a
  pending Build trust decision. Six focused tests passed and Astra approved. The staged ConPTY DLL also
  required updating old loader-report expectations: both self-tests already passed, but lazy loading now
  reports ConPTY before QtNetwork. Corrected tests retain extended-path and successful-self-test checks.
- Lua syntax (ADR 0061): pinned MIT Lua grammar with original highlight/outline/fold queries and bounded
  indentation. Astra required fixes for nested closing blocks, inline closed blocks, string continuation
  and quadratic retained indentation storage. Corrected model/UI tests pass, including a 63 KiB adversarial
  prefix. These are fold ranges; an interactive folding UI is still absent.
- Engine opening (ADR 0060): project/location CLI and native per-user, per-project IPC with bounded frames,
  peer identity checks, reservation before the first frame and listening afterward. Existing real-process
  tests cover Unicode/spaces, dirty-tab reuse, wrong projects, owner death and symlink targets. Astra's
  final combined review found an additional ordinary-open symlink identity case; Terra corrected root
  ownership and existing dirty-editor lookup by physical identity. New Windows symlink regressions skip
  without privilege; all six native Linux process tests pass, including both new ordinary-open regressions.
  Astra approved the correction and the final integration.
- Combined gates before that correction: Windows **2517/2527 (nine skipped, one disabled)** and native WSL
  **2389/2407 (17 skipped)**, each with one failure: the vendor test expected nine packages after two separate
  additions produced ten. Actual licence gates passed (Windows 42 / Linux 38 packages, zero violations),
  as did Windows format, frost-lint (2257 files) and featurecheck. The merged expectation is now ten with
  explicit Lua/libvterm presence checks; licence unit tests pass 36/36 on both platforms and Astra approved.
- Final complete gates passed: Windows `verify.ps1 -Quick -WindowsOnly -Jobs 2 -TestJobs 4`, **2518/2529
  (11 skipped, one disabled probe)**; native WSL `verify.sh --quick --no-checks` with two jobs, **2392/2409
  (17 skipped)**. Windows format, frost-lint (2257 files), featurecheck and licence gates passed; Linux's
  licence gate passed too. Both environments used real LuaLS 3.19.1 and the target engine's generated API.
  Skips are reported, not counted as passed. Tidy, sanitizers and stress remain deferred.
- Debug preview `E:\FrostBuilds\preview\codex-20261102-workbench-debug` passed startup and actual Direct3D11
  rendering on RTX 4070 SUPER. A temporary probe verified that ConPTY DLL and OpenConsole loaded from this
  stage and carried Shift+Enter/surrogates; the probe was removed afterward. This preview includes the final
  symlink correction. LuaLS is not bundled; it remains a development preview, not a complete release.
- Next work is isolated: `codex-debugger-workbench` ports the inherited debugger into the current app and
  CMake lifecycle; `codex-engine-automation` is prepared for authenticated engine controls/logs/validation/tests
  after the opening fix. Both inherit staged baseline tree `c99d689f4413d29934f465bcf2bce1689dfc075b`; transfer
  only baseline-relative deltas. The original `m4-debugger` and other worktrees remain preserved.
- PLAN B.1 now records public-reference coverage families and visible gaps. `codex-managed-language` inherits
  staged baseline `59548a6120f8a0bf9663b40b05a4686f012233da`. The official Roslyn server's root MIT licence did
  not establish permission for all bundled components; its downloaded packages remain inspection-only.
  Terra is auditing an alternative open-source service before installation/integration. No C#, Unity,
  remote, web/database or complete Rider-parity claim is made. Next: implement, review and verify the
  debugger, authenticated FrostEngine automation and managed semantics. No user answer is blocking progress.

## 2026-11-02: Codex continuation, verified workbench/Lua checkpoint

### Scope and ownership
- The user requested continuation toward independently implemented Rider feature/workflow parity with original
  assets, plus CMake, first-class Lua, Unity and FrostEngine; GPT-6.1 Terra implements and GPT-6 Astra reviews.
  ADR 0054, PLAN.md's scope update and Appendix B supersede the old Unity exclusion and C# scope ceiling.
  The original milestone estimates and 191-row tracked matrix do not cover the expanded scope.
- The user selected `D:\Dev\FrostEngine-AI-Claude` (actual directory spelling) as the engine. Its
  `Docs/ScriptEditor.md` v1/v1.1 contract already provides `.frostproj`, `.luarc.json`, generated
  `.frost/lua-api/frost.lua`, custom-editor arguments and authenticated per-project loopback automation.
  The other local engine repositories are not integration targets. Unity 6000.6.4f1 is installed locally.
- This checkpoint completes the inherited merge of `w1/unify` into `main` from 9e6b86b, with reviewed
  renderer and Lua integration plus verification fixes. Existing worktrees are preserved. It is a development
  checkpoint, not a completed Rider-equivalent product or a new product release.

### Implemented and reviewed in this batch
- W1: resolved all nine unmerged index paths; fixed the conflicting editor/build `DiagnosticSource` types;
  attached editor insight and maintained semantic tokens/inactive regions through configuration restart;
  reject project switching while a build/program is active. Astra caught a worker/request lifetime race:
  the corrected StableLspRequester captures synchronized shared server ownership before lazy execution,
  detached bindings cancel their work, and workspace-edit encoding changes occur on the GUI thread.
  Astra approved the reviewed paths after deterministic delayed replacement/close/cancellation tests.
- QRhi parity: preserved Git gutter stripes; implemented diagnostic decorations, error stripe and chrome;
  bounded large shapes to viewport-clipped tiles; skip distant invisible stroke chunks without losing phase;
  verify translucent tile seams at fractional DPR. Qt's opaque raster glyph-run path retained ClearType
  despite the font flag, so raster text now uses bounded transparent scratch before composition. Astra's
  independent probe reproduced the issue and verified the correction. Actual unchanged GPU comparison
  passes on RTX 4070 SUPER with zero fallback; allocation churn remains a performance follow-up.
- Lua backend: trusted absolute-path LuaLS discovery and supervision, document sync, diagnostics and shared
  LSP requests; restart/replay and safe delayed-request ownership. Preserve the engine-owned configuration
  and definitions. Astra approved after adding bounded completion-test requests and a silent-server test.
  The application now routes Lua and C++ documents to their respective retained servers, cancels stale
  editor requests, presents Lua diagnostics, and exposes executable/runtime settings. Real API completion,
  hover, definition navigation and rename work through the editor. Astra caught lost `.frostproj` intent
  in session/recents: exact project identity now survives reconstruction and reordered recents despite a
  sibling solution, and the trust test operates the actual Lua-specific dialog. Astra approved the correction.
  Lua syntax, deployment and debugging remain separate unfinished features.
- SDK debugger tests: the first full Windows run exposed cached debuggee source paths from Claude's old
  worktree, so breakpoints in this checkout never bound. Direct PDB/LLDB inspection confirmed the cause.
  The tiny debuggee oracle now bypasses the compiler launcher and tests require `verified: true`.
  Both real LLDB tests pass after rebuild; Astra approved. Application objects showed the same cache
  source-identity problem. ADR 0056's approved launcher now separates canonical source/build roots while
  preserving runtime cache busters, argument boundaries, explicit launchers and same-tree hits.
- WSL verification: an older source timestamp could leave callers compiled against a prior class layout.
  A debugger watchpoint proved the resulting stack overwrite. ADR 0059 changes mirroring to content
  checksums and fresh timestamps for changed files, with an actual sync regression. Astra approved it;
  a one-time clean rebuilt all native targets. No renderer assertions or skip rules were weakened.

### Verification
- Final integrated tree: Windows `verify.ps1 -Quick -WindowsOnly -Jobs 2 -TestJobs 4` passed,
  **2428/2435 (seven skipped)**. WSL `verify.sh --quick --no-checks`, after the native clean, passed,
  **2304/2321 (17 skipped)**. Windows ran the shared format, frost-lint (2180 files), featurecheck and
  licence gates; Linux ran its licence gate too. All passed. Tidy, sanitizers and stress remain deferred.
- Both native platforms ran the real LuaLS backend tests (3/3) and Lua workbench tests (2/2), including
  FrostEngine-generated definitions, syntax diagnostics, editor completion/hover/navigation/rename, trust
  and project identity. LuaLS 3.19.1 matches the target engine's Windows bundle. The native Linux official
  release was hash-verified and isolated at `~/sdk/frost-luals-test-3.19.1`; its URL/hash are in `TEST-PIN.txt`.
  PATH/API fixture changes were process-local; engine sources and user projects were not modified.
- W1 Windows Debug build; initial focused 46/46, then final lifetime/binding/real-clangd/MSBuild cases 9/9.
- Renderer worktree: full offscreen editor suite 55 passed, one manual hardware skip; hardware case separately
  passed on the private desktop; WARP focused comparison 14 passed, one manual skip. No tolerances relaxed.
- Lua worktree: language units 103/103; Lua integration 3/3 with the engine-packaged Windows LuaLS, including
  real generated `AI.Wake` completion, invalid-member diagnostics, no false undefined `AI`, and a simulated
  unanswered-request deadline. Test projects are scratch copies; engine/project sources were not modified.
- SDK targeted hidden-desktop LLDB tests 2/2 after the cache bypass; source path confirmed in rebuilt object.
- First whole-tree gate was interrupted to fix Astra's lifetime finding. The restarted baseline ended with
  Windows 2405/2413 (six skipped, the two LLDB failures above) and one formatting failure;
  WSL 2291/2300 (nine skipped), all executed Linux tests passed. Both licence checks, frost-lint and
  featurecheck passed. The formatter and SDK fixes are applied. Renderer/Lua integration and the SDK fix
  occurred after that WSL snapshot; the final verification above supersedes it.
- The combined gate completed with Windows **2425/2432 (seven skipped, no failures)** and WSL
  **2295/2317 (19 skipped, three failures)**. The Linux failures are SIGSEGVs in
  `RhiGlyphPainter.FontSizeAndPixelRatioChangesRebuildTheAtlas`, `DrawsTheChangeStripesAsSolidQuads`
  and `TheRendererForwardsFontChangesToTheViewport`. These were the stale caller-layout failures
  explained and corrected by ADR 0059, not product renderer failures. The final clean verification passed.
- The latest staged Debug preview at `E:\FrostBuilds\preview\codex-20261102-lua-workbench-debug`
  passes startup and render self-tests. The integrated renderer's unchanged manual hardware parity test
  separately passes on the RTX 4070 SUPER with zero fallbacks. LuaLS is not bundled with this preview.
- `featurecheck` passes after the expanded-scope documentation; the old matrix is explicitly the original
  C/C++ baseline. All imported patches pass the staged whitespace check; three parity-test enum references
  were mechanically adapted to W1's `EditorDiagnosticSource` rename without changing assertions.

### Next, blockers and deferred work
- The general compiler-cache correction (ADR 0056) is implemented and Astra-approved: canonical source/build
  identity plus the runtime user cache buster, argument boundaries preserved by numeric CMAKE_ARGV references.
  Real two-tree `/Z7` paths, same-tree hits, changing user busters, explicit launchers and unusual argv are
  tested. The initial list-based wrapper corrupted arguments; review caught it and the replacement passes the
  unchanged probe and the final gates above. One CMake process per compile remains a B28 measurement item.
- Connect CMake's existing service/tree/tool window, compilation database and build/run behavior to the app.
  The existing BuildController and RunService are MSBuild-specific; do not fake CMake as a SolutionModel.
  Terra implemented the initial integration in `codex/cmake-integration`, based at main
  9e6b86b plus a staged snapshot of this batch. Its baseline index tree is
  `10fe563b397a2fcf01d6e0de39550c0ed1333595`; transfer only its delta against that tree, not its inherited
  staged work. That snapshot excludes ADR 0056 and its cache changes. Focused Windows tests pass, but
  Astra found stale build completion, profile/artifact identity and shutdown-launch races. Terra is fixing
  them with deterministic regressions. One earlier interrupted build-outcome wait remains unexplained.
- Implement Lua syntax in `codex/lua-syntax` and FrostEngine command-line opening/current-window reuse
  in `codex/engine-open`, both baseline tree `088c0b03a2cf4dfcda92475e3a8cfdd100cd91ed`; transfer only
  their deltas. Native pipe/AF_UNIX transport follows the existing IPC plan, without Qt Network. These
  source slices are unverified. Engine automation controls/logs/tests and managed/Unity work still follow.
- Astra found unbounded retained output and dropped UTF-16 units in the terminal; targeted fixes now pass.
  Its whole pending slice has been imported into separate `codex/terminal-workbench` on baseline tree
  `938b5b18a828aa7689d94c838a6c9a4945fc8660`, preserving the original worktree. Astra approved the
  final Workbench lifecycle/trust/actions and staged ConPTY deployment after Windows app, PTY and
  package tests, including actual loaded DLL and host paths. The latest app slice still needs integration
  and Linux verification. Patch: `E:\FrostBuilds\terminal-spike\terminal-workbench-final.patch`.
  The real Claude startup probe reached only its workspace trust screen; prompt compatibility is unproven.
- Debugger UI remains uncommitted in its inherited worktree; CMake/search/Git integration, W1/U1 end-to-end
  scenarios and packaging remain unfinished. Windows abort-stop behavior and Notepad++ acceptance remain gaps.
- No blocking question for the user. Extensive hardening remains deferred as before; missing product behavior
  and requested Astra reviews are not deferred. The goal is active well beyond this batch.

## 2026-11-02: Lua service backend (unmerged worktree)

- Done: LuaLS discovery and trust-gated supervised lifecycle, Lua document synchronization and
  diagnostics, `ILspRequester` adapter, configurable client defaults and FrostEngine API directory.
  Engine-owned `.luarc.json` and generated definitions are not modified. ADR 0055 records the boundary.
- Verification: targeted Windows `lang_unit_tests` 103/103 and LuaLS integration 3/3 with the
  FrostEngine packaged server, including completion from generated API definitions, an invalid field
  and no false undefined global, plus a simulated unanswered-request deadline; no WSL or full verification yet.
- Next: application routing and settings, navigation integration, independent review and full
  verification before merge.
- Blockers/questions: no LuaLS bundled with FrostIDE; installation and distribution policy remains open.

## 2026-11-02: M1 in progress

`main` has the thirteenth, the twelfth, the eleventh, the tenth, the ninth, the eighth, the seventh and the sixth
feature-first batches (ADR 0031), merged with `git merge --no-ff` in the main worktree under D23. The thirteenth:
`m5/git-core` at f98193c (merge d67f420), then one follow-up of the merged tree (25d83a8). The twelfth, in this order:
`m3/editor-insight` at eb033ab (merge a2626b4), one fix of the merged tree (63aefbc), `m2/cmake` at ce467e4 (merge
bcc3f32) and one fix of the merged tree (96adc58). The eleventh: `m2/find-in-files` at 2ce9819 (merge aca0ea8), then
two follow-ups of the merged tree (3b0025f, 4ecb9ab). The tenth: `m1/shell-nav` at 588ddfe (merge
9d00def), then one fix of the merged tree (cfbd46e). The ninth, in this order: `w1/run-service` at 82c6c56 (merge
fcc0a46), `m1/editor-rhi` at 9438b85 (merge 97e490e) and `w2/clangd-nav` at 660a1e9 (merge 29869f9, with the
regenerated keymap golden). The eighth, in this order: `m1/editor-view` at 3acd71c (merge bde77a5), the long-line
follow-up (4a9954b), `w1/tool-windows` at c81008c (merge e574965) and an ADR-reference fix (791fa63). The seventh, in
this order: `w1/build-service` at 8d77400 (merge eb82b95), `m1/shell-frame` at 9576c24 (merge b1d05bc),
`w2/clangd-diagnostics` at e7d9ccc (merge db9d24e) and `m1/save-manager` at e2b2362 (merge 5fbd262), then the label
follow-up (8ee7a1d). The sixth, in this order: `m1/verify-tooling` at 2e286e0 (merge 08b92fd), `m1/syntax` at 681c749
(merge b706290), `m1/editor-model` at 0b4aac1 (merge b2a6b49), `m1/diff-merge` at 7851f95 (merge 88d75ac), `m1/settings`
at 481fc26 (merge 8a5768e) and `plan/thin-path` at 6315d86 (merge 414155a), then one fix of the merged tree (358ffd3).
Each slice passed its own `-Quick`, and each batch's merged tree was verified once (the Verification sections below).
The earlier batches, their slice records and the open questions are in the entry "2026-11-01: M1 in progress". Its
"Deferred to the hardening phase (ADR 0031)" list is carried forward into this entry's, which is now the running one.
This entry's slice records follow its sections, newest first, written from the hand-off notes (none of the twenty
branches kept a record in this log).

### Done

#### Merge of `m5/git-core` (d67f420, thirteenth batch)
- **The merge** (d67f420) of `m5/git-core` (f98193c, on d34334c) into `main` at dc76d7e: the Qt-free frost::vcs
  (layer vcs: git found from the setting, Git for Windows' install paths or PATH; GitRunner with an English,
  prompt-free environment, time limits and cancellation of the process tree; repository discovery; the porcelain v2
  status parser and the VcsStatusMap snapshot behind IVcsStatusProvider; file versions from HEAD and the index;
  commit of selected files, rollback, branches, fetch, pull and push; restricted mode in untrusted folders; the
  line-change markers and the side-by-side diff model on frost::diff's histogram diff; GitService on two serial
  queues with the debounced refresh) and frost::ui_vcs (GitController, the VCS file colours, the Commit window, the
  side-by-side DiffViewer with F7/Shift+F7, EditorChangeMarkers with their popup and Rollback Lines, the branch
  widget and the Branches popup, the git console, and `registerGitActions` for the `vcs.*` actions with Show Diff's
  Ctrl+D). In the editor, DisplayChangeStripe in the display list and `EditorView::setLineChanges`; the colour seams
  `SolutionTreeModel::setFileColorProvider` and `EditorTabs::setTitleColorProvider`; ContextKeys `CommitViewFocus`
  and `DiffViewFocus` (appended); 14 `Vcs.*`, change-marker and `Diff.*` colour keys in the four Glacier themes.
  Nothing is wired into the workbench yet (Next). The slice record below says more.
- **ADR 0048** (`docs/adr/0048-git-core.md`): the number is free on `main` (0047 is `m3/editor-insight`'s). Its
  references are right: 0016 (input), 0019 (frost::diff, D6 and Q3), 0031 (feature-first), 0037 (editor view) and
  0038 (shell). Its Deferred item on QRhi stripes now records that they were done at the merge (the follow-up below).
- **The base** (the pre-state): d34334c, older than `main`, before the tenth to twelfth batches (`m1/shell-nav`,
  `m2/find-in-files`, `m3/editor-insight`, `m2/cmake`), so fourteen files conflicted, each resolved so that both
  sides' intent survives:
  - The four Glacier theme files: `main`'s `Semantic` group, then the slice's `Vcs` and `Diff` groups (each file
    still parses as JSON).
  - `theme_registry.hpp`: `main`'s 28 insight and `Semantic.*` keys, then the slice's 14 `Vcs.*`,
    `Editor.change*Color` and `Diff.*` keys, in the enum and in the table; both appended, so `main`'s order stands.
  - `tests/ui/model/registry_test.cpp` and `theme_blob_test.cpp` (the pre-state): both pins recomputed from the
    merged tree for the union of keys, as the tests' comments say. `kExpectedRegistryHash` is 0xe49cf7c3154b6a1e, the
    value `Registry.TablesConsistent` printed on the merged tree (`main` had 0x31cfb245a8dcffe0, the slice
    0x01f21b52f86152e7). The blob's body arrays are 772 bytes and the blob 844 (604 at the common base, plus 4 x 28
    and 4 x 14). The checks are unchanged, only their values; `ui_model_tests` 46/46 after the update.
  - `display_list.hpp`: `main`'s DisplayDecoration, DisplayStripeMark, DisplayShape, DisplayInspection and
    DisplayBalloon beside the slice's ChangeStripeKind and DisplayChangeStripe; `changeStripes` after `markers`. The
    paint-order note keeps `main`'s order and adds `changeStripes` to the gutter; the slice's "a future QRhi backend"
    sentence became one that says what the QRhi backend draws (updated by the follow-up below).
  - `editor_view.hpp`: both includes (`QPointer`; `QPoint`, `QRectF`), `main`'s insight API and the slice's
    `setLineChanges`, `lineChanges`, `lineChangeAt` and `changeStripeColumn`; the signals keep `main`'s
    `rendererChanged`, `insightChanged`, `documentEdited` and `problemsViewRequested` and add the slice's
    `textChanged` and `lineChangeClicked`.
  - `editor_view_input.cpp`: a press goes to `clickInsight` (the error stripe, the inspection widget) first, then to
    a change stripe (`lineChangeClicked`), then to the text; the areas do not overlap.
  - `src/CMakeLists.txt` and `src/ui/CMakeLists.txt`: `add_subdirectory(vcs)` after `main`'s `search` inside the
    fuzz guard, and `ui/vcs` after `main`'s nav, workbench, find and cmake (fuzz trees build no UI); the ui_vcs
    sentence joins `main`'s header note.
  - `tests/ui/CMakeLists.txt`: `main`'s `ui_run_tests` and `ui_lang_tests`, then the slice's `ui_vcs_tests` and
    `ui_vcs_git_tests`, with every header line (the slice's include one for `ui_toolwindows_tests`).
  - `tests/tools/test_verify_support.py` (the slice's git tag rules): `main`'s msbuild, clangd, cmake and msvc
    rules plus the slice's "git" rule (`GitE2E.`, `GitUiE2E.`, kind "integration" with the `frost_desktop` lock)
    and its `seen["git"]` check; the note names all of them. Only the list grows.
- **Auto-merged:** `action.hpp` (the two ContextKeys after `main`'s), `editor_colors`, `editor_view.cpp`,
  `editor_view_paint.cpp` (the stripe rows beside the insight's decorations), `raster_renderer.cpp`,
  `shell_window.cpp` (`CommitViewFocus` from the active tool window), `editor_tabs`, `solution_tree_model`,
  `tests/CMakeLists.txt` and the editor, tab and Solution Explorer tests.
- **The keymap goldens** (the pre-state): `input_tests` with `FROST_UPDATE_GOLDENS=1` on the hidden desktop rewrote
  its six goldens byte for byte as committed (`git status` clean afterwards), so there was nothing to add. The
  slice's Ctrl+D, F7 and Shift+F7 are ui_vcs module defaults, which `ui_vcs_tests` checks, and the IntelliJ keymap's
  `vcs.*` bindings (`vcs.commit = Ctrl+K`, ...) were in the goldens already.
- **The format check** (the pre-state): `clang-format --dry-run --Werror` (23.1.4) over the 64 C/C++ files the merge
  adds or changes, right after the merge: clean.
- **The fixture check** (the pre-state, after the twelfth batch lost an ignored fixture): `git status --ignored
  --short` in the slice's worktree (read only) lists only `build/` and `tools/__pycache__/`, so every fixture its
  tests read is committed (the six `tests/vcs/fixtures/diff` pairs; the git tests build their repositories in
  temporary folders through `tests/vcs/git_fixture.hpp`). REUSE.toml's `tests/**/fixtures/**` covers them.
- **The build of the merged tree:** `night.py build-gate` (33.9 GB commit free), `cmake --preset win-clangcl` and
  `cmake --build --preset win-clangcl-debug -j 8`: exit 0, 272 steps (29 after the pin update). Then ctest
  `-L "mod.(ui|vcs|input|tools|app|diff)"` on the hidden desktop: 837/837, with the slice's `GitE2E` and `GitUiE2E`
  cases (tag "git", 20 tests) and `tools.verify_support` with the git rule.

#### QRhi stripe follow-up (25d83a8, thirteenth batch)
- **Why** (the pre-state): the slice drew `changeStripes` in the raster backend only, because its tree had no QRhi
  backend (ADR 0048, Deferred); `main` has ADR 0043's, and the addition is small.
- **The change** (25d83a8): RhiGlyphPainter adds each stripe as a solid quad in the gutter range, after the gutter
  text and before the markers, where the raster backend fills it. The new
  `RhiGlyphPainter.DrawsTheChangeStripesAsSolidQuads` checks one solid quad per stripe at its device pixels and in
  its colour, at 100 % and 200 %. `display_list.hpp`'s note says what the QRhi backend draws (not yet the
  decorations, the error stripe or the chrome layer), and ADR 0048's Deferred item records the change. No assertion
  changes.
- **Checked:** the Debug build (exit 0, 59 steps), ctest `-L "mod.(ui|app|vcs)"` on the hidden desktop (478/478) and
  `RhiGlyphPainter.*` (4/4).

#### Verification (thirteenth batch, merged tree 25d83a8)
- Before the batch run: the format check right after the merge (clean), the Windows Debug build after the merge, the
  pin update and the follow-up, and the touched suites through ctest or the test executables on the hidden desktop
  (above).
- **The run that counts**, and the only one: `tools\verify.ps1 -Quick -Jobs 8` on 25d83a8 (clean tree; this record
  not yet written) through `run_hidden_nobox.py` (a hidden desktop; no window opened on it), after `night.py
  build-gate` (37.0 GB commit free): 668.3 s, exit code 0. Windows ran its Debug leg (ctest `-j 8`) and the checks;
  WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`): `Verified: win debug 2376/2382 (6
  skipped), wsl debug 2261/2270 (9 skipped), wsl tsan-quick not-run (off by default), wsl tidy not-run (off by
  default), wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run (off by
  default)` (frost-lint 2130 files; licensecheck 39 packages on Windows, 35 on WSL; featurecheck 0 problems). The test
  steps took 277.9 s (Windows, parallel at `-j 8`) and 266.8 s (WSL). The `git` suites (`GitE2E`, `GitUiE2E`), which
  fail rather than skip without git, ran and passed on both OSes.
- **The skips:** the twelfth batch's, unchanged. Windows: the four `WatcherPerf` tests,
  `PollingWatcher.DirectorySymlinksAreNotFollowed` and `SafeWrite.SymlinksStayLinks`. WSL: the four `WatcherPerf`
  tests, `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale`,
  `CrashpadReporter.PipeThatNeverAppearsTimesOutAndStopsTheHandler`,
  `MsBuildEvaluation.MapsEvaluatedMetadataToClangClFlags`,
  `MsBuildProvider.FailedEvaluationFallsBackToTheStaticModel` and
  `RhiGlyphPainter.RendersTheDisplayListWhereTheRasterBackendDoes`. The new
  `RhiGlyphPainter.DrawsTheChangeStripesAsSolidQuads` needs no device and ran on both.
- **No test was lost:** against the twelfth batch (win debug 2327, wsl debug 2215) the tree adds 55 tests on both
  OSes, the `TEST` cases of the batch: `m5/git-core` 54 (`tests/vcs` 31: `git_e2e_test.cpp` 14, `status_test.cpp`
  8, `line_changes_test.cpp` 5, `runner_test.cpp` 4; `tests/ui/vcs` 20: `vcs_ui_test.cpp` 9, `vcs_ui_git_test.cpp`
  6, `change_markers_test.cpp` 5; one each in the editor view, editor tabs and Solution Explorer tests) and the
  follow-up's one. No `TEST` line and no assertion line is removed; the two pins change value only, and the label
  check only gains a tag.
- **The pre-state's checks:** `main` was clean at dc76d7e and equal to `origin/main` at the start; no untracked
  `Testing/` folder appeared in the repository root (the integrator's ctest runs used `--test-dir` on the build
  tree), and the golden regeneration left the tree clean.
- **Processes:** the runner ended every process tree it started; no night.py lock was taken (build-gate takes none)
  and no background loop is left.
- `-Quick` builds no ASan, TSan or fuzz tree. The slice's sanitizer, fuzz, stress and `-Full` runs are in "Deferred
  to the hardening phase (ADR 0031)", with `-Full` of `main`.

#### Merge of `m3/editor-insight` (a2626b4, twelfth batch)
- **The merge** (a2626b4) of `m3/editor-insight` (eb033ab) into `main` at ebd4b0d: in frost::lang LSP semantic tokens
  (`semantic_tokens.hpp`: the legend read once at initialize, the relative stream decoded into byte ranges, full and
  delta requests with a full fallback) and clangd's `textDocument/inactiveRegions`, kept per open file by
  ClangdService with listeners; in frost::ui_editor an LSP-free insight vocabulary (`editor_insight.hpp`) and
  EditorView's seams `setDecorations(owner, ...)`, `setSemanticTokens`, `setInactiveRanges`, `setDiagnostics` (build
  and live), `setUsageHighlights` and `setAnalyzing`, with squiggles and tooltips, the error stripe, the inspection
  widget, InsightStatusIndicator for the status bar and F2/Shift+F2 (`navigate.nextError`/`previousError`) with the
  balloon, all in the display list; EditorInsightBinding in frost::ui_lang; and 28 `Semantic.*` and marker colour
  keys in the four Glacier themes. Feature rows 36 (semantic highlighting), 37 (inactive regions) and 58 (error
  stripe, inspection widget) are `in-progress` with acceptance tests, and row 94 adds the F2 test. Nothing is wired
  into `src/app` (the W1 unify slice does that; the pre-state, Next). The slice record below says more.
- **ADR 0047** (`docs/adr/0047-editor-insight.md`): the number is free on `main` (0046 is `m2/cmake`'s, merged after
  it in this batch). Its references are right: 0031 (feature-first), 0036 (ClangdService and the diagnostics store),
  0037 (EditorView and the display list) and 0041 (the language features); the code's 0027 (labels and locks) too.
- **The branch's own merges** (the pre-state): 7d2fab1 of `m1/editor-view` 3acd71c and d969d15 of `w2/clangd-nav`
  660a1e9 are on `main`, and a1fd262 is byte for byte the same diff as `main`'s label-check fix 76d5e7f, so it
  resolves to `main`'s. git found two merge bases (660a1e9 and 3acd71c) and conflicted against its virtual base, so
  the result was built as `main` plus the slice's own commit: the merge changes exactly the 43 files that eb033ab
  changes against a1fd262, and every one of them outside the three hand-resolved files below equals the three-way
  merge of `main`, a1fd262 and eb033ab.
- **The theme pins** (the pre-state): `kExpectedRegistryHash` 0x31cfb245a8dcffe0 and the theme blob's 716/788 stand
  as the slice set them. `main`'s `theme_registry.hpp`, the four theme files and the two `tests/ui/model` tests were
  unchanged from the slice's base (a1fd262), and `m2/cmake` adds no theme key, so nothing had to be recomputed; no
  check is loosened. `ui_model_tests` passed on the merged tree and in the batch run.
- **Seven conflicts**, each resolved so that both sides' intent survives:
  - `src/ui/editor/CMakeLists.txt`: both header notes (the slice's insight sentence, then `main`'s QRhi backend note,
    ADR 0043, and its pinned GuiPrivate package); the slice's `editor_insight.cpp`, `editor_view_insight.cpp` and
    `insight_status.cpp` beside `main`'s `editor_backend.cpp`, `editor_view_renderer.cpp`, `glyph_atlas.cpp` and the
    rhi sources; `insight_status.hpp` beside `main`'s `rhi_renderer.hpp` under AUTOMOC.
  - `src/ui/editor/include/frost/ui/editor/editor_view.hpp`: the class note keeps `main`'s Renderers paragraph
    (ADR 0043) and adds the slice's Insight paragraph (ADR 0047); the signals keep `main`'s `rendererChanged()` and
    add `insightChanged()`, `documentEdited(version)` and `problemsViewRequested()`.
  - `tests/ui/CMakeLists.txt`: `ui_editor_tests` builds `editor/editor_insight_test.cpp` beside `main`'s
    `editor/editor_rhi_test.cpp`; the header list keeps `main`'s QRhi, `ui_nav`, `ui_workbench`, `ui_find` and
    `ui_run` lines and adds the slice's insight and EditorInsightBinding text. `ui_lang_tests` took the slice's
    extra dependencies and `_frost_ui_test_wiring` without a conflict.
  - `src/CMakeLists.txt`, `src/ui/CMakeLists.txt`, `tests/features/matrix.json` and `tests/lang/CMakeLists.txt`
    conflicted only against the virtual base: the first two are `main`'s text (the slice does not change them), the
    other two `main`'s text plus the slice's rows 36, 37, 58 and 94 and its `semantic_tokens_test.cpp` and notes.
- **Auto-merged:** the slice's changes to `editor_view.cpp`, `editor_view_input.cpp` and `editor_view_paint.cpp`
  beside `m1/editor-rhi`'s renderer switch. `m1/editor-rhi`'s QRhi backend does not draw the new display-list parts
  (decorations, the stripe, chrome and chromeText) yet; the raster backend does (the slice's Next).
- **The format check** (the pre-state): `clang-format --dry-run --Werror` (23.1.4) over the 31 C/C++ files the merge
  adds or changes, right after the merge: clean.
- **The label check:** the slice adds no tagged suite (its real-clangd case is
  `ClangdE2E.SemanticTokensAndInactiveRegionsOnAFixture` in `lang_clangd_tests`, which the check already tags
  "clangd" by the `ClangdE2E.` prefix), so `tests/tools/test_verify_support.py` needed no change.
- **The build of the merged tree** (a2626b4): `night.py build-gate` (34.1 GB commit free), `cmake --preset
  win-clangcl` and `cmake --build --preset win-clangcl-debug -j 8`: exit 1, one compile error in
  `tests/ui/editor/editor_rhi_test.cpp` (the follow-up below).

#### QRhi test follow-up (63aefbc, twelfth batch)
- **The failure:** the slice adds two fields to DisplayText, `semantic` and `inactive`, and was based before
  `m1/editor-rhi` (ninth batch), so its own run never compiled `editor_rhi_test.cpp`. On the merged tree that test's
  sample-run lambda brace-initialised DisplayText without them, which `-Wmissing-field-initializers` under `/WX`
  rejects.
- **The fix** (63aefbc): the lambda passes `std::nullopt, false`, the values every DisplayText in the slice's own
  code gets for text without a semantic kind that is not inactive, so the sample display list (and what
  `RhiGlyphPainter.RendersTheDisplayListWhereTheRasterBackendDoes` compares) is unchanged; clang-format laid the
  initializer out over two lines. No assertion moves. A fix forward, not a revert: the cause was clear and the
  change one initializer.
- **Checked:** the Debug build (exit 0, 51 steps) and ctest `-L "mod.(ui|lang)"` on the hidden desktop: 428/428
  (the slice's `EditorInsight`, `EditorInsightBinding`, semantic-token and `ClangdE2E` cases, and `main`'s
  `RhiGlyphPainter` and `ui_model_tests` among them).

#### Merge of `m2/cmake` (bcc3f32, twelfth batch)
- **The merge** (bcc3f32) of `m2/cmake` (ce467e4, on 52eb3d6) into `main` at 63aefbc: Qt-free CMake support in
  frost::project (CMakePresets.json and CMakeUserPresets.json with includes, inherits, conditions and macros;
  profiles from the presets, the `cmake.profiles` setting or one Debug default; the File API reply model and the view
  model) and frost::build (cmake and ninja discovery, the vcvarsall.bat environment capture and its cache,
  CMakeService with the trust gate, background configure, reconfigure on change and `cmake --build`, the GNU/Clang
  and CMake output parsers, and `BuildSessionOptions::parser`, a parser factory that keeps MSBuild's parser as the
  default); the `cmake.profiles` (an `executes` record list) and `tools.ninja.path` settings;
  BuildDirectoryCompileCommandsProvider in frost::lang, which follows the active profile's build directory for
  clangd; and the new frost::ui_cmake (`src/ui/cmake`: CMakeTreeModel, CMakeController behind ICMakeBackend, and
  CMakeToolWindow), which the workbench does not host yet (Next). The slice record below says more.
- **ADR 0046** (`docs/adr/0046-cmake-projects.md`): the number is free on `main` (0047 is `m3/editor-insight`'s).
  Its references are right: 0011 (process launching, E2), 0020 (the watcher), 0021 (settings, yyjson), 0029 (M16),
  0031 (feature-first), 0033 (pugixml), 0035 (build service) and 0036 (compile commands).
- **Five conflicts**, each resolved so that both sides' intent survives:
  - `src/build/CMakeLists.txt`: `main`'s RunService note (ADR 0040) and the slice's CMake note; the TODO(M4) keeps
    `main`'s "full run configurations" and drops "the CMake driver", which this slice adds; `output_parsers.cpp`
    beside `main`'s `run_service.cpp`, `run_session.cpp` and `run_target.cpp`; the slice's DEPS (settings and
    sched added to project and io).
  - `src/lang/src/compile_commands.cpp`: `BuildDirectoryCompileCommandsProvider::locate()` after
    `ExistingCompileCommandsProvider::locate()` and before `w2/clangd-nav`'s `resolveConfigurationIndex`; neither
    side's code changes.
  - `src/ui/CMakeLists.txt`: `main`'s header note plus the ui_cmake sentence; `add_subdirectory(cmake)` after
    `main`'s editor, toolwindows, the guarded lang, nav, workbench and find (fuzz trees build no UI).
  - `tests/build/CMakeLists.txt`: `build_unit_tests` has the slice's `cmake_service_test.cpp`,
    `cmake_toolchain_test.cpp` and `output_parsers_test.cpp` beside `main`'s `run_service_test.cpp` and
    `run_target_test.cpp`; `build_cmake_tests` and the Windows-only `build_msvc_env_tests` merged after `main`'s
    suites.
  - `tests/ui/CMakeLists.txt`: `ui_cmake_tests` after `ui_shell_tests`, where the slice put it, with its header line,
    beside `main`'s suites.
- **Auto-merged:** `src/settings` `keys.hpp` and `builtin_registry.cpp` (the slice's two keys beside `main`'s
  `editor.renderer`), `tests/settings/registry_test.cpp` (both typed-key checks), `build_session.hpp`, `session.cpp`
  and `compile_commands.hpp`. The slice's extension of `StoreTest.ExecutesKeysOfUntrustedProjectsAreIgnored` (a
  planted `cmake.profiles` record with a toolchain file) is a strengthening (the orchestrator's note).
- **The label check** (orchestrator decision; the slice left it out to avoid a conflict with `main`'s 76d5e7f): in
  the merge, `tests/tools/test_verify_support.py`'s tagged-suite list gains "cmake" for `build_cmake_tests`
  (`CMakeRealBuild.`) and "msvc" for `build_msvc_env_tests` (`MsvcEnvironmentCapture.`, Windows only), both of kind
  "integration" with the `frost_desktop` lock, and its note names them. Only the list grows; the prefixes do not catch
  the unit suites `CMakeTools.`, `MsvcEnvironment.` or `CMakeServiceTest.`.
- **The format check** (the pre-state): `clang-format --dry-run --Werror` over the 43 C/C++ files the merge adds or
  changes (the tiny CMake project's three C fixtures excluded, as `verify.ps1` excludes fixtures): clean.
- **The build of the merged tree** (bcc3f32): `night.py build-gate` (34.5 GB commit free) and the Debug build: exit
  0, 214 steps. Then ctest `-L "mod.(build|project|settings|lang|ui|tools)"` on the hidden desktop: 648/650, the two
  failures of the follow-up below (`CMakeRealBuild` and `MsvcEnvironmentCapture` passed, and `tools.verify_support`
  with the new tag rules).

#### Presets fixture follow-up (96adc58, twelfth batch)
- **The failure:** `CMakePresets.ReadsTheFixtureFolder` (2 presets files instead of 3) and
  `CMakeProfiles.FromTheFixturePresets` (no "mine" profile). Both read `tests/project/fixtures/cmake/presets`, whose
  third file, `CMakeUserPresets.json`, the repository's `.gitignore` ignores everywhere; the slice's commit left it
  out, and it existed only untracked in the slice's worktree, where its own `-Quick` passed.
- **The fix** (96adc58): the fixture is added byte for byte as the slice wrote it (a "mine" configure preset that
  inherits "debug", with an environment macro and a cache variable), and `.gitignore` gains one exception for that
  path only; the rule for users' own `CMakeUserPresets.json` stays. No test changes. A fix forward, not a revert.
- **Checked:** `project_unit_tests` 53/53 and `build_unit_tests` 105/105 on the hidden desktop.

#### Verification (twelfth batch, merged tree 96adc58)
- Before the batch run: the format check right after each merge (both clean), the Windows Debug build after each
  merge and follow-up, and the touched suites through ctest or the test executables on the hidden desktop (above).
  The format check is a standing step of the integrator from this batch on (the pre-state, after the eleventh batch's
  slice had committed unformatted files): `clang-format --dry-run --Werror` over each slice's added or changed C/C++
  files right after its merge, fixed forward before the batch run, so the format check does not cost a second run.
- **The run that counts**, and the only one: `tools\verify.ps1 -Quick -Jobs 8` on 96adc58 (clean tree; this record
  not yet written) through `run_hidden_nobox.py` (a hidden desktop; no window opened on it), after `night.py
  build-gate` (38.7 GB commit free): 560.4 s, exit code 0. Windows ran its Debug leg (ctest `-j 8`) and the checks;
  WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`): `Verified: win debug 2321/2327 (6
  skipped), wsl debug 2206/2215 (9 skipped), wsl tsan-quick not-run (off by default), wsl tidy not-run (off by
  default), wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run (off by
  default)` (frost-lint 2070 files; licensecheck 39 packages on Windows, 35 on WSL; featurecheck 0 problems). The test
  steps took 167.6 s (Windows, parallel at `-j 8`) and 257.8 s (WSL). On Windows the 11 `msbuild`, 17 `screenshot`
  and 1 `msvc` tests (`MsvcEnvironmentCapture.RunsVcvarsallAndCaches`) ran and passed; the `cmake` test
  `CMakeRealBuild.ConfigureBuildCleanReconfigure` ran and passed on both OSes (clang-cl through the captured MSVC
  environment on Windows, clang on WSL). The format check passed at the first attempt, so it cost no second run.
- **The skips:** the eleventh batch's, unchanged. Windows: the four `WatcherPerf` tests,
  `PollingWatcher.DirectorySymlinksAreNotFollowed` and `SafeWrite.SymlinksStayLinks`. WSL: the four `WatcherPerf`
  tests, `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale`,
  `CrashpadReporter.PipeThatNeverAppearsTimesOutAndStopsTheHandler`,
  `MsBuildEvaluation.MapsEvaluatedMetadataToClangClFlags`,
  `MsBuildProvider.FailedEvaluationFallsBackToTheStaticModel` and
  `RhiGlyphPainter.RendersTheDisplayListWhereTheRasterBackendDoes`.
- **No test was lost:** against the eleventh batch (win debug 2275, wsl debug 2164) the tree adds 52 tests on Windows
  and 51 on WSL, the `TEST` cases the slices add: `m3/editor-insight` 16 (`semantic_tokens_test.cpp` 6,
  `editor_insight_test.cpp` 7, `editor_insight_binding_test.cpp` 2 and one `ClangdE2E` case) and `m2/cmake` 36
  (`tests/build` 19, `tests/project` 13, `ui_cmake_tests` 3 and one compile-commands case), of which
  `MsvcEnvironmentCapture.RunsVcvarsallAndCaches` builds only on Windows. The follow-ups add two initializer values
  and one fixture file; no `TEST` line and no assertion line is removed, and the label check only gains two tags.
- **The pre-state's checks:** `main` was clean at ebd4b0d and equal to `origin/main` at the start; no untracked
  `Testing/` folder appeared in the repository root during this batch (the integrator's ctest runs used
  `--test-dir` on the build tree).
- **Processes:** the runner ended every process tree it started; no night.py lock was taken (build-gate takes none)
  and no background loop is left.
- `-Quick` builds no ASan, TSan or fuzz tree. The slices' sanitizer, fuzz, stress and `-Full` runs are in "Deferred
  to the hardening phase (ADR 0031)", with `-Full` of `main`.

#### Merge of `m2/find-in-files` (aca0ea8, eleventh batch)
- **The merge** (aca0ea8) of `m2/find-in-files` (2ce9819, on 52eb3d6) into `main` at b04a621: the Qt-free Find in
  Files engine in frost::search (file masks with `!` excludes; the Solution, Project, Directory and Open files scopes;
  every encoding the editor opens, through the load service and its large-file guard; open documents searched in their
  committed text; cancelling; the 10,000-match limit), Replace in Files with frost::document's new
  `SaveManager::rewriteClosedFile` (`src/document/src/rewrite.cpp`), and the new frost::ui_find (`src/ui/find`:
  FindSession, the Find/Replace popup, the Find tool window and FindFeature's actions), with `search.nextOccurrence`
  (Ctrl+Alt+Down) and `search.previousOccurrence` (Ctrl+Alt+Up) in the IntelliJ keymap and the Edit menu, and
  `window.find` under View > Tool Windows. FindFeature is not installed in MainWindow yet: the W1 unify slice does
  that (the pre-state; Next). The slice record below says more.
- **ADR 0045** (`docs/adr/0045-find-in-files.md`): the number is free on `main` (no 0044 on `main`; 0043 is
  `m1/editor-rhi`'s). Its references are right: 0018 (encodings), 0031 (feature-first) and 0038 (shell); the code's
  0019 (diff, D1), 0025 (the editor model's SearchPattern) and 0028 (save manager) are right too.
- **The module name clash** (the pre-state): `m1/shell-nav` (on `main`) and this slice both created frost::search in
  `src/search`, both in layer "search" at rank 7, which `cmake/FrostLayers.cmake` already had (neither side changed
  it). They are one library now:
  - `src/search/CMakeLists.txt`: one `frost_add_library(search LAYER search)` with both sets of sources
    (`fuzzy_path.cpp` beside `file_mask.cpp`, `find_in_files.cpp`, `replace_in_files.cpp` and `search_scope.cpp`) and
    the union of the dependencies (the slice's document, project, text_editor, text, storage, io and sched, and
    shell-nav's core). Rank 7 satisfies both sides: the slice links document and project (rank 6), shell-nav links
    core (rank 1), and the one consumer on `main`, frost::ui_workbench, is in layer ui (rank 9). The configure's layer
    check passed. Both public APIs (`fuzzy_path.hpp`; `file_mask.hpp`, `find_in_files.hpp`, `replace_in_files.hpp`
    and `search_scope.hpp`) are unchanged and share no name. The header note's TODO(M13) now names only the trigram
    index, since the fuzzy matcher has joined.
  - `tests/search/CMakeLists.txt`: one `search_tests` with `fuzzy_path_test.cpp`, `find_in_files_test.cpp` and
    `replace_in_files_test.cpp`, the slice's include path and `FROST_TINY_SLN_DIR`; label "unit" (`mod.search`). No
    test is dropped: 5 FuzzyPath and 17 Find/Replace in Files cases, 22 in all.
  - `src/CMakeLists.txt`: search is added once, inside the fuzz guard, since it now links document, project and RE2,
    which fuzz trees do not build; `main`'s unguarded `add_subdirectory(search)` (the tenth batch's resolution) is
    gone. Fuzz trees build no UI and no tests (`CMakePresets.json`), and no fuzz harness uses frost::search.
- **Three other conflicts**, each resolved so that both sides' intent survives:
  - `tests/CMakeLists.txt`: `main`'s list; one note for `search` naming ADR 0042 and ADR 0045.
  - `src/ui/CMakeLists.txt`: both header notes; `find` after `main`'s `shell`, `editor`, `toolwindows`, the guarded
    `lang`, `nav` and `workbench`.
  - `tests/ui/CMakeLists.txt`: `ui_find_tests` after `main`'s `ui_editor_tests`, `ui_nav_tests` and
    `ui_workbench_tests`, with its line in the header list.
- **Auto-merged:** `src/input/src/keymap_intellij.cpp` and `src/ui/shell/src/shell_menus.cpp` (the slice's lines
  beside `main`'s) and the three `intellij-*.txt` goldens (two added lines each). `keymap-reachability.txt` merged to
  `main`'s 213 actions; the follow-up below regenerates it.
- **The Directory scope's TODO(M2)** (the pre-state) stays: the engine's `io::scanTree` call sets
  `readGitignore = false`, clears `skipNames` and walks with the scope's own excludes on purpose, so switching to
  shell-nav's gitignore-aware scan changes the scope's behaviour and tests. It is not a one-line switch (Next).
- **The label check:** the slice adds no tagged suite (`search_tests` and `ui_find_tests` are "unit"), so
  `tests/tools/test_verify_support.py` needed no change.
- **The build of the merged tree** (aca0ea8): `night.py build-gate` (39.0 GB commit free), `cmake --preset
  win-clangcl` and `cmake --build --preset win-clangcl-debug -j 8`: exit 0, 119 steps. Then ctest
  `-L "mod.(input|search|ui)"` on the hidden desktop: 589/591, the two failures of the follow-up below
  (`ui_find_tests`' 6 cases passed).

#### Fixture and golden follow-up (3b0025f, eleventh batch)
- **The failure:** `TinySolution.SolutionScopeSearchesEveryProjectFile` and
  `TinySolution.ProjectScopeSearchesTheCurrentFilesProject` expected one `tinyAnswer` match in the tiny MSBuild
  fixture's `App/main.cpp` and found 2. `w1/run-service` (82c6c56, ninth batch) made that program print
  "tiny answer N" for its run tests, so it calls `tinyAnswer()` twice (lines 17 and 30); the slice was based before
  that.
- **The fix** (3b0025f): both tests expect 2 matches in `main.cpp`. `lib.cpp`, `lib.h` and the matched-file counts (3
  for the solution, 1 for App's project) are unchanged, and the fixture note says why `main.cpp` has two. The
  expectation follows the fixture; no assertion is removed or loosened. A fix forward, not a revert.
- **The keymap goldens** (the pre-state), regenerated from the merged tree with `FROST_UPDATE_GOLDENS=1` (input_tests
  on the hidden desktop; in that mode every golden test reports "golden updated"). Only `keymap-reachability.txt`
  changed: 213 actions and 225 sequences become 215 and 227 in all eight layout lines (the slice's two entries on top
  of `main`'s), still 0 gaps with the same dead-key strokes. The `intellij-windows/linux/macos` goldens were already
  right after the merge, and every other input golden is byte-identical, so only additions appear.
- **Checked:** the Debug build (exit 0) and ctest `-L "mod.(input|search)"` on the hidden desktop: 276/276 (254
  input, 22 search).

#### Format follow-up (4ecb9ab, eleventh batch)
- **The failure:** the first batch run (Verification below) passed every test on both OSes, but its format check
  failed on ten files the slice added: `src/document/src/rewrite.cpp`, `src/search/src/find_in_files.cpp` and
  `replace_in_files.cpp`, `src/ui/find/src/find_feature.cpp`, `find_popup.cpp`, `find_session.cpp` and
  `find_tool_window.cpp`, and `tests/search/find_in_files_test.cpp`, `replace_in_files_test.cpp` and
  `tests/ui/find/find_ui_test.cpp`. `.clang-format` has not changed since the slice's base (52eb3d6), so the slice's
  sources were committed unformatted (its hand-off names lint, licence and feature checks, not format).
- **The fix** (4ecb9ab): clang-format 23.1.4 `-i` on the ten files (line breaks at the 120-column limit, one
  regrouped include block in `rewrite.cpp`, one nested conditional aligned), plus one hand edit: FileMask's `?` case
  wrote `"a\xC3\xA9" "c.txt"` as two adjacent literals (a hex escape would swallow the `c`), which clang-format split
  over two lines; it is now `"a\303\251c.txt"`, the same bytes (U+00E9 in UTF-8) in octal escapes. No token of code
  or test changes otherwise. A fix forward, not a revert: the cause was clear and the change mechanical.
- **Checked:** `clang-format --dry-run --Werror` clean for every C/C++ file of `src/search`, `src/ui/find`,
  `src/document`, `tests/search` and `tests/ui/find`; the Debug build (exit 0, 50 steps); ctest
  `-L "mod.(search|document|ui)"` on the hidden desktop: 374/374.

#### Verification (eleventh batch, merged tree 4ecb9ab)
- Before the batch run: the Windows Debug build after the merge and after each follow-up, and the touched suites
  through ctest on the hidden desktop (above).
- **The first run** of `tools\verify.ps1 -Quick -Jobs 8`, on 3b0025f, through `run_hidden_nobox.py` (a hidden desktop;
  no window opened on it), after `night.py build-gate` (32.8 GB commit free): 487.6 s, exit code 1. Every test passed
  on both OSes (win debug 2269/2275, 6 skipped; wsl debug 2155/2164, 9 skipped), and frost-lint, licensecheck (both
  OSes) and featurecheck were ok, but the format check failed on the slice's ten files (the format follow-up above).
  No slice was reverted.
- **The run that counts**, the same command on 4ecb9ab (clean tree; this record not yet written), after `night.py
  build-gate` (39.2 GB commit free): 533.4 s, exit code 0. Windows ran its Debug leg (ctest `-j 8`) and the checks;
  WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`): `Verified: win debug 2269/2275 (6
  skipped), wsl debug 2155/2164 (9 skipped), wsl tsan-quick not-run (off by default), wsl tidy not-run (off by
  default), wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run (off by
  default)` (frost-lint 2001 files; licensecheck 39 packages on Windows, 35 on WSL). The test steps took 145.3 s
  (Windows, parallel at `-j 8`) and 309.3 s (WSL). The 11 `msbuild` and 17 `screenshot` tests ran and passed on
  Windows; `mod.search` has its 22 tests on both OSes. `FileWatcher.NewDirectoryAndItsFilesAreSeen`, the slice's WSL
  flake, passed in both runs.
- **The skips:** the tenth batch's, unchanged. Windows: the four `WatcherPerf` tests,
  `PollingWatcher.DirectorySymlinksAreNotFollowed` and `SafeWrite.SymlinksStayLinks`. WSL: the four `WatcherPerf`
  tests, `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale`,
  `CrashpadReporter.PipeThatNeverAppearsTimesOutAndStopsTheHandler`,
  `MsBuildEvaluation.MapsEvaluatedMetadataToClangClFlags`,
  `MsBuildProvider.FailedEvaluationFallsBackToTheStaticModel` and
  `RhiGlyphPainter.RendersTheDisplayListWhereTheRasterBackendDoes`.
- **No test was lost:** against the tenth batch (win debug 2246, wsl debug 2132) the tree adds 23 tests on each OS,
  the 23 `TEST` cases the slice adds: `search_tests` 17 (beside shell-nav's 5 FuzzyPath cases, 22 in all) and
  `ui_find_tests` 6. The follow-ups change two expected counts (1 to 2, the fixture's two calls), the reachability
  golden's counts and formatting; no `TEST` line and no assertion line is removed.
- **The pre-state's checks:** `main` was clean at b04a621 and equal to `origin/main` at the start; no untracked
  `Testing/` folder appeared in the repository root during this batch (the integrator's ctest runs used
  `--test-dir` on the build tree), so nothing had to be moved aside.
- **Processes:** the runner ended every process tree it started; no night.py lock was taken (build-gate takes none)
  and no background loop is left. The cmake, ninja, ctest and test processes still running at the end belong to other
  agents' worktrees (`m5-git-core` among them) and were left alone.
- `-Quick` builds no ASan, TSan or fuzz tree. The slice's sanitizer, fuzz, stress and `-Full` runs are in "Deferred
  to the hardening phase (ADR 0031)", with `-Full` of `main`.

#### Merge of `m1/shell-nav` (9d00def, tenth batch)
- **The merge** (9d00def) of `m1/shell-nav` (588ddfe) into `main` at ac253cd: in io `IFileSystem::canonicalPath`
  (Windows: the real case of every component through GetFinalPathNameByHandleW; MemFs and Linux: the lexical form)
  and the tree scan's `.ignore` files, default build-output excludes and skipped `.git/.vs/.svn/.hg`; the new
  frost::search (`src/search`: the fuzzy path matcher and `file:line[:col]`/`file(line)` parsing), frost::ui_nav
  (`src/ui/nav`, Qt-free: navigation history, recent files and locations, the session JSON with its yyjson store,
  DocumentRegistry, ProjectFiles and MergePanes) and frost::ui_workbench (`src/ui/workbench`: the Workbench with the
  File System view, Find Action, Go to File, Search Everywhere, Recent Files and Locations, Back/Forward, session
  restore, the hot-exit restore prompt, the editor banners, the merge view, Save All and autosave); in frost::ui_shell
  ListPopup's filter mode, disabled rows and section headers, EditorViewState with `viewState`/`restoreViewState`,
  `IEditorFactory::canonicalPath` and tabs keyed by it; and in frost::app persistent AppServices (the document stack,
  the hot-exit journal, sessions, a thread pool and the file watcher), MainWindow owning a Workbench, and main()
  opening folders and solutions as projects (else the last project) and offering recoverable journals after the first
  frame. Feature rows 28, 29, 76, 77, 78, 91 and 92 are `in-progress` with acceptance tests. The slice record below
  says more.
- **ADR 0042** (`docs/adr/0042-shell-navigation.md`): the number is unchanged and no other ADR on `main` uses it (0043
  is `m1/editor-rhi`'s). Its references are right: 0015 (appearance), 0021 (yyjson), 0026 (display map), 0028 (save
  manager), 0031 (feature-first) and 0038 (shell frame). No line the slice adds cites the shell as ADR 0035.
- **The branch's own merges** of `m1/shell-frame` 9576c24 (33dfc14) and `m1/save-manager` e2b2362 (0756a45) were
  already on `main` (three merge bases: e2b2362, 9576c24 and 2e298a8), so those parts were no-ops. The pre-state's
  check: the slice tree still had the shell's ADR as `docs/adr/0035-shell-frame.md`, and it did not come back; git
  kept `main`'s rename to `0038-shell-frame.md` (b1d05bc), so `docs/adr` has 0035 (the build service), 0038 (the
  shell frame) and 0042 (this slice), each once.
- **Nine conflicts**, each resolved so that both sides' intent survives:
  - `src/CMakeLists.txt`: `main`'s text plus the slice's `add_subdirectory(search)` after the fuzz-guarded block
    (frost::search needs only core, so fuzz trees build it too). The slice's second, unguarded
    `add_subdirectory(build)` is dropped: `main` builds build inside the guard since the seventh batch.
  - `tests/CMakeLists.txt`: `main`'s `frost_add_test_directory` list and guards, with `search` after `lang`, so
    `search_tests` has `mod.search`.
  - `src/ui/CMakeLists.txt`: both header notes; `nav` and `workbench` after `main`'s `shell`, `editor`, `toolwindows`
    and the guarded `lang`. Fuzz trees build no UI (`FROST_FUZZ` needs `FROST_BUILD_UI=OFF`), so nav's and
    workbench's frost::document dependency needs no guard.
  - `tests/ui/CMakeLists.txt`: `ui_nav_tests` and `ui_workbench_tests` after `ui_editor_tests`, with their lines in
    the header list; `ui_screenshot_tests`' line keeps `main`'s form (the kind "unit" with the tag "screenshot").
  - `src/app/CMakeLists.txt`: frost::app links the slice's ui_workbench, ui_nav, document, storage, project, settings
    and sched beside `main`'s ui_shell, ui, input, io, diag and core; one header note.
  - `src/app/include/frost/app/app_services.hpp` and `main_window.hpp`: the slice's header notes (ADR 0038 and 0042).
  - `src/app/src/main.cpp`: the slice's two hunks, the persistent AppServices passed to MainWindow, and the command
    line opening folders and solutions as projects (or restoring the last project) before files open in tabs.
    `main`'s diagnostics flow around them is unchanged.
  - `tests/features/matrix.json`: rows 28, 29, 76, 77 and 78 take the slice's acceptance tests and status (91 and 92
    merged without a conflict); their other fields and rows 26, 27, 30, 70 and 226 stay `main`'s (ADR 0029's
    milestones and completions, which the slice's base did not have).
- **The composition** (the pre-state): `src/app` on `main` had not changed since the seventh batch's b1d05bc, and
  neither `m1/editor-view` nor `w1/tool-windows` wired anything into it, so the `src/app` conflicts were the two
  resolutions of the shell-frame merge (`main`'s b1d05bc and the slice's 33dfc14) plus the slice's composition. No
  composition piece was defined twice; shell-nav's structure stands, and `main`'s TODO(M16) note in `main.cpp`, the
  `.clang-tidy` files of `src/app` and `tests/app`, and `tests/app`'s `frost_desktop` lock and `PROCESSORS 2` stay.
  The new IEditorView and IEditorFactory members have defaults, and nothing on `main` but the shell's placeholder, the
  shell tests' fakes and the slice's DocumentEditorFactory implements them. `main`'s app tests passed in the batch run
  (Verification below). `w1/workbench` was not merged (the pre-state; Next).
- **Goldens and the label check** (the pre-state): the slice changes nothing under `tests/input` and adds no tagged
  suite (`search_tests`, `ui_nav_tests` and `ui_workbench_tests` are "unit"), so neither the keymap goldens nor
  `tests/tools/test_verify_support.py` needed an update; `KeymapReachability.Golden` and `tools.verify_support`
  passed in the batch run.
- **The build of the resolved tree** (before the merge commit, which records the same tree): `night.py build-gate`
  (35.2 GB commit free) and `cmake --build --preset win-clangcl-debug -j 8`: exit 1 at step 273 of 350, one compile
  error in `ui_workbench_tests` (the follow-up below).

#### Long-line option follow-up (cfbd46e, tenth batch)
- **The failure:** `WorkbenchTest.BannersShowLongLinesAndOutOfDateFiles` set
  `LoadServiceOptions::maxEditableLineChars`, which the eighth batch's long-line follow-up (4a9954b) renamed to
  `maxEditableLineBytes` when the rule moved to `kLongLineCapBytes`. The slice was based before it, so its own run
  never saw the rename.
- **The fix** (cfbd46e): the test sets `maxEditableLineBytes = 10`. Its long line is 50 ASCII `x`, 50 bytes as well as
  50 characters, so the case still opens a line over the cap read-only with the long-line banner; nothing else in the
  test changes and nothing is weakened. The slice's source never named the option. A fix forward, not a revert: the
  cause was clear and the change one identifier.
- **Checked:** `night.py build-gate` (37.8 GB commit free) and the Debug build: exit 0, 96 steps; then the slice's
  suites through ctest on the hidden desktop: 71/71 (WorkbenchTest, the ui_nav models, FuzzyPath and the app's
  MainWindow cases among them).

#### Verification (tenth batch, merged tree cfbd46e)
- Before the batch run: the Windows Debug build after the merge and after the follow-up (above), and the slice's
  suites after the follow-up.
- **The first run** of `tools\verify.ps1 -Quick -Jobs 8`, on cfbd46e, through `run_hidden_nobox.py` (a hidden desktop;
  no window opened on it), after `night.py build-gate` (37.5 GB commit free): 596.5 s, exit code 1. Every test passed
  on both OSes (win debug 2246/2252, 6 skipped; wsl debug 2132/2141, 9 skipped) and every other check was ok, but
  frost-lint failed on one file that is not in the tree: `Testing/Temporary/CTestCostData.txt`, under an untracked
  `Testing/` folder in the repository root (an empty ctest run's `CTestCostData.txt` and `LastTest.log`, written at
  02:43:56, after this session's first `git status` had shown the tree clean; frost-lint checks untracked files too).
  This session ran no ctest from that directory; the folder was moved, not deleted, to the integrator's scratchpad,
  and no slice was reverted.
- **The run that counts**, the same command on cfbd46e (clean tree; this record not yet written), after `night.py
  build-gate` (33.9 GB commit free): 475.7 s, exit code 0. Windows ran its Debug leg (ctest `-j 8`) and the checks;
  WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`): `Verified: win debug 2246/2252 (6
  skipped), wsl debug 2132/2141 (9 skipped), wsl tsan-quick not-run (off by default), wsl tidy not-run (off by
  default), wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run (off by
  default)` (frost-lint 1975 files; licensecheck 39 packages on Windows, 35 on WSL). The test steps took 155.9 s
  (Windows, parallel at `-j 8`) and 266.7 s (WSL). The 11 `msbuild` and 17 `screenshot` tests ran and passed on
  Windows; `mod.search` has its 5 tests on both OSes.
- **The skips:** the ninth batch's, unchanged. Windows: the four `WatcherPerf` tests,
  `PollingWatcher.DirectorySymlinksAreNotFollowed` and `SafeWrite.SymlinksStayLinks` (no symlink right for this
  account). WSL: the four `WatcherPerf` tests, `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale`,
  `CrashpadReporter.PipeThatNeverAppearsTimesOutAndStopsTheHandler`,
  `MsBuildEvaluation.MapsEvaluatedMetadataToClangClFlags`,
  `MsBuildProvider.FailedEvaluationFallsBackToTheStaticModel` and
  `RhiGlyphPainter.RendersTheDisplayListWhereTheRasterBackendDoes`.
- **No test was lost:** no `TEST` line and no assertion line is removed between ac253cd and cfbd46e; the follow-up
  renames one option in a test's set-up. Against the ninth batch (win debug 2220, wsl debug 2111) the tree adds 32
  tests on Windows and 30 on WSL, the 32 `TEST` cases the slice adds: `tests/ui` 22 (`ui_workbench_tests` 14,
  `ui_nav_tests` 8), `tests/search` 5, `tests/io` 3 and `tests/app` 2. Two of them build only on Windows:
  `RealFileSystem.CanonicalPathGivesOneSpellingPerFile` and `WorkbenchWindows.CaseVariantsOpenOneDocument`.
  `frost_process_tests`' Linux cache now also redirects `XDG_DATA_HOME` (the GUI's journal and sessions), an
  addition.
- **Processes:** the runner ended every process tree it started; no night.py lock was taken (build-gate takes none)
  and no background loop is left. The ctest, test and `verify.sh` processes still running at the end belong to other
  agents' worktrees (`m3-editor-insight`, `m2-cmake`, `w1-workbench`) and were left alone.
- `-Quick` builds no ASan, TSan or fuzz tree. The slice's sanitizer, fuzz, stress and `-Full` runs are in "Deferred
  to the hardening phase (ADR 0031)", with `-Full` of `main`.

#### Merge of `w1/run-service` (fcc0a46, ninth batch)
- **The merge** (fcc0a46) of `w1/run-service` (82c6c56) into `main` at d34334c: RunService, RunSession and
  `run_target` in frost::build, the `onDone` callback on `BuildService::build`, ProcessSpec `rawArguments` and
  `newConsole` in io, `parseConfigurationProperties` in project, the new frost::ui_run (`src/ui/run`: RunConsole,
  RunController and the run actions), Ctrl+F5 (`run.runWithoutDebugging`) as a Visual Studio alias in the IntelliJ
  keymap (unbound on macOS, where the swap would give VoiceOver's Meta+F5), and a synthetic `App.vcxproj.user` and a
  richer `main.cpp` in the `tiny_sln` fixture; the slice record below says more. ADR 0040
  (`docs/adr/0040-run-service.md`); the number is unchanged and no other ADR on `main` uses it, and its "ADR 0035" is
  the build service, as on `main`. The branch's own merge (8ea53fb) of `w1/build-service` 8d77400 was already on
  `main`, so that part was a no-op.
- **Six conflicts**, each resolved so that both sides' intent survives:
  - `src/CMakeLists.txt` and `tests/CMakeLists.txt`: `main`'s text. The slice commit changes neither file; the branch
    only lacked `main`'s later additions (diff, syntax, settings, document and lang; `frost_add_test_directory` and its
    guards), so `tests/build` keeps `mod.build`.
  - `src/ui/CMakeLists.txt`: both header notes; the slice's `if(TARGET frost::build)` around `add_subdirectory(run)`
    stays.
  - `tests/build/CMakeLists.txt`: `build_msbuild_tests`' note keeps `main`'s form (8ee7a1d: the kind "integration"
    with the tag "msbuild") and gains the slice's RunService runs; the new `build_run_tests` is "integration".
  - `tests/ui/CMakeLists.txt`: `ui_run_tests` after `ui_toolwindows_tests`, with its line in the header list.
  - `src/project/src/vcxproj_reader.cpp`: both new functions side by side, `main`'s `clCompileMetadata`
    (`w2/clangd-diagnostics`) and the slice's `parseConfigurationProperties`.
- **The keymap goldens** (the pre-state): the slice's regenerated input goldens (one `run.runWithoutDebugging` line in
  each `intellij-*.txt`; 211 to 212 actions and 223 to 224 sequences in `keymap-reachability.txt`) merged without a
  conflict, since `main` had not touched them since the slice's base. `w2/clangd-nav`'s merge regenerated them once
  more (its merge section below).
- **Not wired yet:** nothing in `src/app` uses frost::ui_run; the W1 workbench follow-up does that (Next).
- **The orchestrator's decision on its first known bug:** cmd.exe interpreting `^ & | < >` and `%VAR%` in the external
  console's arguments is queued as a follow-up: a tiny own console helper exe (`frost-console-host`) that starts the
  program directly with the exact command line, waits, prints "Press any key to continue" and keeps the exit code,
  replacing the `cmd /c` wrapper (Next).
- **The build after the merge commit:** `night.py build-gate` (39.2 GB commit free) and `cmake --build --preset
  win-clangcl-debug -j 8`: exit 0, 209 steps.

#### Merge of `m1/editor-rhi` (97e490e, ninth batch)
- **The merge** (97e490e) of `m1/editor-rhi` (9438b85) into `main` at fcc0a46: the QRhi glyph-atlas editor renderer
  behind IEditorRenderer (EditorGlyphAtlas, RhiGlyphPainter, RhiEditorRenderer with RhiEditorViewport, and the
  backend choice in `editor_backend`), the `editor.renderer` setting (auto, raster or rhi; auto picks raster in a
  remote session, on a WARP adapter and on Linux), the lazy QRhi attach after the first raster frame, a run-time
  fallback that keeps the editor state, and atlas rebuilds on font, zoom and DPI changes; the slice record below says
  more. ADR 0043 (`docs/adr/0043-editor-qrhi-renderer.md`); the number is unchanged and no other ADR on `main` uses
  it, and its references (ADR 0007 decisions 1, 3, 5 and 8, ADR 0011 A1, ADR 0037) are right. The branch's own merge
  (dd43253) of `m1/editor-view` 3acd71c was already on `main`, so that part was a no-op (the pre-state).
- **Two conflicts**, each resolved so that both sides' intent survives: `src/CMakeLists.txt` takes `main`'s text (the
  branch only lacked document, lang and build's move into the fuzz guard), and `tests/ui/CMakeLists.txt` takes
  `main`'s header list with the slice's QRhi note on `ui_editor_tests` (`editor/editor_rhi_test.cpp` joined its
  sources without a conflict).
- **The pins** (the pre-state): the slice changes no theme and no theme or registry pin. Nothing under
  `resources/themes`, `src/ui/model`, `src/ui/theme_compiler` or `tests/ui/model` changes, so `kExpectedRegistryHash`
  and the theme blob size stay the eighth batch's. The settings registry gains one entry, `editor.renderer` (an enum,
  default `auto`, application scope, under Editor/General/Appearance), with its typed key `key::editorRenderer`, which
  `SettingsRegistry.TypedKeysMatchTheRegistry` checks; the registry has no pinned count or golden (its size test is a
  lower bound), so nothing else needed an update.
- **The orchestrator's decisions:** the view-level QRhi path is unverified on screen (a QRhiWidget never initializes
  under the offscreen QPA); the W1 workbench follow-up adds a hidden-desktop render self-test, and the preview defaults
  to raster if that test fails (Next). A theme change that does not rebuild the atlas is a recorded deviation (the
  atlas holds coverage only; ADR 0043).
- **The build after the merge commit:** `night.py build-gate` (40.6 GB commit free) and the Debug build: exit 0, 81
  steps.

#### Merge of `w2/clangd-nav` (29869f9, ninth batch)
- **The merge** (29869f9) of `w2/clangd-nav` (660a1e9) into `main` at 97e490e: in frost::lang the MSBuild-evaluated
  compile-commands provider and the provider chain (existing, then MSBuild, then static), the ILspRequester seam,
  models for completion, signature help, hover, navigation, usages, highlights, symbols, rename, code actions and
  formatting, and WorkspaceEdit parsing with its applier; the new frost::ui_lang (`src/ui/lang`: the widgets and
  LanguageController, with the IntelliJ actions registered); and `navigate.relatedFiles` = Ctrl+Alt+Home (macOS
  Ctrl+Meta+Up) in the keymap; the slice record below says more. ADR 0041
  (`docs/adr/0041-clangd-navigation-completion-msbuild-commands.md`); the number is unchanged and no other ADR on
  `main` uses it, and its references (0016, 0031, 0032, 0034 and 0036) are right. The branch's own merge (49f5bb0) of
  `w2/clangd-diagnostics` e7d9ccc was already on `main`, so that part was a no-op.
- **Four conflicts**, each resolved so that both sides' intent survives:
  - `tests/CMakeLists.txt`: `main`'s text; settings and lang already come in through `frost_add_test_directory`.
  - `src/ui/CMakeLists.txt`: both header notes (shell, editor, ui_run, then ui_lang), and the slice's
    `if(TARGET frost_lang)` around `add_subdirectory(lang)` after `main`'s shell, editor and toolwindows.
  - `tests/lang/CMakeLists.txt`: `lang_clangd_tests`' note keeps `main`'s form (the kind "integration" with the tag
    "clangd"). `lang_msbuild_tests` (Windows only) already had the tags-after-kind form on the branch (`LABELS
    integration msbuild clangd`), as the orchestrator asked, and its note now names the kind and tags the same way.
  - `tests/ui/CMakeLists.txt`: `ui_lang_tests`' line after `ui_run_tests`' in the header list; its target merged
    without a conflict after `ui_run_tests`.
- **The keymap goldens, regenerated** (the pre-state): both slices took `keymap-reachability.txt` from 211 to 212
  actions and from 223 to 224 sequences on every layout row, so git merged the identical change once and
  `KeymapReachability.Golden` failed on the merged tree (213 actions, 225 sequences). Before the merge commit, the
  goldens were regenerated from the merged Windows Debug tree with `FROST_UPDATE_GOLDENS=1` and `input_tests`
  (hidden), as `tests/input/support/golden.hpp` says. The update run rewrites every input golden; only
  `keymap-reachability.txt` changed: its eight count lines (213 actions, 225 sequences, 0 gaps on each layout and
  platform), with the dead-key lines unchanged. Against d34334c the three `intellij-*.txt` goldens gain exactly two
  lines each (`run.runWithoutDebugging` and `navigate.relatedFiles`) and lose none. `input_tests` then passed 234/234.
  The reachability test's hard asserts (de-DE and en-US complete on both platforms, pl-PL and fr-FR against their
  in-code list of expected gaps) are unchanged, so the regeneration cannot hide a gap.
- **Not wired yet:** nothing implements IEditorLanguageHost over the real editor, so none of the features work in the
  running app (Next). The orchestrator's note: the MSBuild evaluation cache ignores environment-variable changes unless
  an imported file changes (deferred, and the slice record's known bug).
- **The build after the resolution** (before the merge commit, since the goldens needed the merged tree): `night.py
  build-gate` (39.7 GB commit free) and the Debug build: exit 0, 82 steps.

#### Label-check follow-up (76d5e7f, ninth batch)
- **The failure:** the first batch run (on 29869f9) failed one test on Windows, `tools.verify_support`
  (`BuildTreeLabelsTest.test_one_kind_label_and_tags_after_it`). That test, from the seventh batch's label follow-up
  (8ee7a1d), pins which suites carry the "msbuild" and "clangd" tags, and `w1/run-service` and `w2/clangd-nav` were
  based before it, so their own runs never saw it. Six tests had a tag outside the pinned suites: `MsBuildRun.*` (2,
  `build_msbuild_tests`, "msbuild") and `MsBuildEvaluationE2E.*` (2, `lang_msbuild_tests`, "msbuild" and "clangd").
  Every one of them had the right kind ("integration") and the `frost_desktop` lock.
- **The fix** (76d5e7f, `tests/tools/test_verify_support.py`): the pinned prefixes gain `MsBuildRun.` (msbuild) and
  `MsBuildEvaluationE2E.` (msbuild and clangd), and the note names `lang_msbuild_tests`. The other rules (one kind
  label, a tag only on its suites, the kind and the lock for the integration suites) are unchanged, so nothing is
  weakened; the unit-labelled `MsBuildEvaluation.*` and `MsBuildProvider.*` in `lang_unit_tests` still must not carry
  a tag. A slice fix forward, not a revert: the cause was clear and the change small.
- **Checked:** `tools.verify_support` passed in the Windows Debug tree (hidden ctest, 11.0 s); then the batch run
  again (the Verification below).

#### Verification (ninth batch, merged tree 76d5e7f)
- Before the batch run: the Windows Debug build after each of the three merges (above), and `input_tests` after the
  golden regeneration.
- **The first run** of `tools\verify.ps1 -Quick -Jobs 8`, on 29869f9 (clean tree), through `run_hidden_nobox.py` (a
  hidden desktop; no window opened on it), after `night.py build-gate` (40.5 GB commit free): 471.9 s, exit code 1.
  Windows failed one test, `tools.verify_support`, and was otherwise 2213/2220 (6 skipped); WSL passed (2102/2111, 9
  skipped) and every check was ok. The cause and the fix forward are the label-check follow-up above; no slice was
  reverted.
- **The run that counts**, the same command on 76d5e7f (clean tree; this record not yet written), after `night.py
  build-gate` (40.7 GB commit free): 400.2 s, exit code 0. Windows ran its Debug leg (ctest `-j 8`) and the checks;
  WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`): `Verified: win debug 2214/2220 (6
  skipped), wsl debug 2102/2111 (9 skipped), wsl tsan-quick not-run (off by default), wsl tidy not-run (off by
  default), wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run (off by
  default)` (frost-lint 1944 files; licensecheck 39 packages on Windows, 35 on WSL). The test steps took 125.7 s
  (Windows, parallel at `-j 8`) and 233.9 s (WSL). The 11 `msbuild` tests (the seven before, `MsBuildRun`'s two and
  `MsBuildEvaluationE2E`'s two) and the 17 `screenshot` tests ran and passed on Windows.
- **The skips:** Windows, the same six as in the eighth batch: the four `WatcherPerf` tests,
  `PollingWatcher.DirectorySymlinksAreNotFollowed` and `SafeWrite.SymlinksStayLinks` (no symlink right for this
  account). WSL, the eighth batch's six (the four `WatcherPerf` tests,
  `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale` and
  `CrashpadReporter.PipeThatNeverAppearsTimesOutAndStopsTheHandler`) and three new ones, each skipped on purpose off
  Windows: `MsBuildEvaluation.MapsEvaluatedMetadataToClangClFlags` (its canned evaluation uses Windows paths),
  `MsBuildProvider.FailedEvaluationFallsBackToTheStaticModel` (MSBuild evaluation runs on Windows only) and
  `RhiGlyphPainter.RendersTheDisplayListWhereTheRasterBackendDoes` (QRhi's Null backend on Linux; ADR 0043).
- **No test was lost:** no `TEST` line and no assertion line is removed between d34334c and 76d5e7f. The lines that
  change on purpose: the eight count lines of `keymap-reachability.txt` (the regeneration above), the label check's
  two pinned prefix lists (the follow-up above) and the editor harness, whose `show()` is now an option that defaults
  to on. Against the eighth batch (win debug 2121, wsl debug 2018) the tree adds 99 tests on Windows and 93 on WSL,
  the 99 `TEST` cases the slices add: `w1/run-service` 37 (`tests/build` 29, `tests/ui` 5, `tests/io` 2,
  `tests/project` 1), `m1/editor-rhi` 13 (`tests/ui`) and `w2/clangd-nav` 49 (`tests/lang` 36, `tests/ui` 13). Six of
  them build only on Windows: `MsBuildRun`'s two, `MsBuildEvaluationE2E`'s two,
  `RealProcessLauncher.RawArgumentsReachTheChildVerbatim` and `RunSession.TheExternalConsoleWrapperKeepsTheExitCode`.
- **Processes:** the runner ended every process tree it started; no night.py lock was taken (build-gate takes none)
  and no background loop is left.
- `-Quick` builds no ASan, TSan or fuzz tree. The slices' sanitizer, fuzz, stress and `-Full` runs are in "Deferred to
  the hardening phase (ADR 0031)", with `-Full` of `main`.

#### Merge of `m1/editor-view` (bde77a5, eighth batch)
- **The merge** (bde77a5) of `m1/editor-view` (3acd71c) into `main` at 52eb3d6: frost::ui_editor (`src/ui/editor`)
  with EditorView, the renderer seam with a raster backend, QtTextShaper, highlighting, key routing over the IntelliJ
  keymap, the find/replace bar, Go to Line, IME, mouse, clipboard, live settings and the status/signals/factory API;
  in the model `commands::pasteLines`, the `deleteLines` caret merge and `frost::text::kLongLineCapBytes`; the slice
  record below says more. ADR 0037 (`docs/adr/0037-editor-view.md`); the number is unchanged and no other ADR on
  `main` uses it. The branch's own merges of `m1/editor-model` 0b4aac1 (6ce7513), `m1/syntax` 681c749 (0976882) and
  `m1/settings` 481fc26 (b50fb7d) were already on `main`, so those parts were no-ops.
- **Six conflicts**, each resolved so that both sides' intent survives, and with the same resolutions as
  `w1/workbench`'s own merge of the slice (591ad72, on 8ee7a1d, which is `main` without the seventh batch's record):
  the merged tree equals 591ad72's apart from `docs/progress.md`, so `w1/workbench`'s later merge stays plain.
  - `src/CMakeLists.txt`, `tests/CMakeLists.txt` and `vcpkg.json`: `main`'s text. The branch only lacked `main`'s later
    additions (document, build, lang and diag; `frost_add_test_directory` and its guards; spdlog). Its
    `add_subdirectory(text/editor)` is already done by `tests/text/CMakeLists.txt`, so the editor model's tests keep
    `mod.text`.
  - `src/ui/CMakeLists.txt`: both subdirectories (`shell`, then `editor`) and both header notes.
  - `tests/ui/CMakeLists.txt`: `ui_editor_tests` after `ui_shell_tests`, with its line in the header list.
  - `tests/features/matrix.json`: rows 57, 85, 94 and 250 take the slice's acceptance tests and `in-progress`; every
    other row stays `main`'s (ADR 0029's milestones for rows 59, 66, 95, 246 and 251-256).
- **The theme pins** (the pre-state): the slice adds 37 `Editor.*`/`Syntax.*` colour keys to the four Glacier themes and
  updates `ui_model_tests`' `kExpectedRegistryHash` (0x229dd62c1484c1a3) and the pinned theme blob size, as the tests
  instruct. `m1/shell-frame`'s merge changed neither pin: no commit between 023f354 and 52eb3d6 touches `src/ui/model`,
  `src/ui/theme_compiler`, `resources/themes` or `tests/ui/model`, so the slice's values are the merged tree's and
  nothing was recomputed. `Registry.TablesConsistent` and the theme blob tests passed in the batch run.
- **The build after the merge commit:** `night.py build-gate` (35.4 GB commit free) and `cmake --build --preset
  win-clangcl-debug -j 8`: exit 0, 150 steps.

#### Long-line follow-up (4a9954b, eighth batch)
- **kLongLineCapBytes in the load service** (the pre-state's follow-up): `m1/save-manager`'s placeholder
  `kMaxEditableLineChars` (1 Mi characters) is now `kMaxEditableLineBytes = frost::text::kLongLineCapBytes` (1,000,000
  bytes, ADR 0037), and `LoadServiceOptions::maxEditableLineChars` is `maxEditableLineBytes`. `ReadOnlyReason::LongLine`
  now means a line of more bytes than the cap, its break excluded: the cap past which the editor view stops shaping a
  row. ADR 0028 decision 7 names the new constant. Nothing else is wired (`w1/workbench` does that); neither
  `w1/workbench` nor `m1/editor-rhi` uses the old names.
- **Still O(1) for almost every text:** the snapshot summary counts the longest line in characters. Snapshot content is
  well-formed UTF-8, so a line has one to four times as many bytes as characters: a text no larger than the cap, or
  whose longest line has at most a quarter of the cap in characters, has no long line, and one whose longest line has
  more characters than the cap has one. Only the texts in between are measured line by line (`Snapshot::linesFrom`,
  O(size), at load).
- **`m1/save-manager`'s test stays meaningful:** `LoadServiceTest.OverlongLinesOpenReadOnly` keeps its boundaries
  (exactly the cap is editable, one byte more is read-only, a clean reload that brings a long line) and now checks
  bytes, not characters: two-byte characters that fill the cap stay editable (a CRLF does not count), one more makes
  the line long; a file whose line with the most characters is not the one with the most bytes; a large file of short
  lines; and restored hot-exit text. The old rule fails the new cases.
- **Slips on the way:** the first build failed on `text::LineIndex` (it is `frost::LineIndex`), and a PowerShell
  `Set-Content -Encoding utf8` put a BOM on `load_service.cpp`; both were fixed before the commit.
- **The build after the commit:** `night.py build-gate` (33.9 GB commit free) and the Debug build: exit 0, then
  `document_unit_tests` 34/34 (hidden) and clang-format clean on the three changed sources.

#### Merge of `w1/tool-windows` (e574965, eighth batch)
- **The merge** (e574965) of `w1/tool-windows` (c81008c) into `main` at 4a9954b: frost::ui_toolwindows
  (`src/ui/toolwindows`) with the Solution Explorer, the Build output pane, Problems, BuildActions with the shortcut
  table, ConfigurationCombo, TrustDialog and TrustPrompter, BuildController with GUI-thread marshalling and
  ISaveAllHandler, and in frost::input `KeymapService::setModuleDefaults`; the slice record below says more. ADR 0039
  (`docs/adr/0039-build-tool-windows.md`); the number is unchanged and no other ADR on `main` uses it, and its "ADR
  0035" is the build service, as on `main`. The branch's own merge (6a591e8) of `w1/build-service` 8d77400 was already
  on `main`, so that part was a no-op.
- **Three conflicts**, each resolved so that both sides' intent survives, and as in `w1/workbench`'s merge 43908a5: the
  merged tree equals 43908a5's apart from `docs/progress.md` and the long-line follow-up.
  - `src/io/include/frost/io/scripted_process_launcher.hpp`: both sides hold the same three ScriptedProcess fields
    (`racesTerminate`, `outputHeld`, `leftovers`) in a different order; `main`'s order (after `chunkBytes` and
    `chunkInterval`, from the seventh batch's merge of `w1/build-service`) stays.
  - `tests/CMakeLists.txt`: `main`'s text; `tests/build` already comes in through `frost_add_test_directory`.
  - `src/ui/CMakeLists.txt`: `toolwindows` joins after `shell` and `editor`.
- **Orchestrator decisions on its questions:** Build Solution stays Ctrl+Shift+B (the Visual Studio habit) as the
  single `kBuildShortcutDefaults` entry, although it shadows the IntelliJ keymap's Go to Type Declaration
  (`navigate.typeDeclaration`) while a solution is open; Ctrl+Shift+B against Ctrl+F9 is the user's open question
  (Questions for the user). The `StallHooks.ASlowSlotIsOneStallAndOneReport` flake it saw once under load is
  `m1/crash-diag`'s known timing flake (that slice record's Known bugs); it is now in the deferred list as well.
- **The build after the merge commit:** `night.py build-gate` (34.4 GB commit free) and the Debug build: exit 0, 152
  steps.

#### ADR-reference fix (791fa63, eighth batch)
- `createEditorView`'s note in `src/ui/editor/include/frost/ui/editor/editor_view.hpp` named the shell's ADR as 0035,
  its number before the seventh batch renumbered it; it reads ADR 0038 now. Comment only. `w1/workbench` and
  `m1/editor-rhi` keep that line as 3acd71c has it, so their merges stay clean. No other reference in the two slices
  is off: ADR 0039's "ADR 0035" is the build service, and ADR 0037's are 0011, 0025, 0026 and 0030.
- **The build after the commit** (after the batch run): its Verification bullet below.

#### Verification (eighth batch, merged tree e574965)
- Before the batch run: the Windows Debug build after both merges and the long-line follow-up (above), and
  `document_unit_tests` after the follow-up.
- **The run** of `tools\verify.ps1 -Quick -Jobs 8`, on e574965 (clean tree; the ADR-reference fix and this record not
  yet written), through `run_hidden_nobox.py` (a hidden desktop; no window opened on it), after `night.py build-gate`
  (34.1 GB commit free): 519.3 s, exit code 0. Windows ran its Debug leg (ctest `-j 8`) and the checks; WSL ran
  `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`): `Verified: win debug 2115/2121 (6 skipped), wsl
  debug 2012/2018 (6 skipped), wsl tsan-quick not-run (off by default), wsl tidy not-run (off by default), wsl
  licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run (off by default)`
  (frost-lint 1872 files; licensecheck 39 packages on Windows, 35 on WSL). No test failed, so no slice was reverted
  and no fix was needed. The test steps took 201.9 s (Windows, parallel at `-j 8`) and 232.0 s (WSL). Another
  agent's verify ran beside it.
- **No bad objects this time:** none of the seventh batch's SEH 0xc0000005 crashes in the ScriptedProcessLauncher
  tests came back, so nothing was rebuilt with `SCCACHE_RECACHE=1`.
- **After the run:** the ADR-reference fix (791fa63, one comment) was built (Debug, 37 steps, exit 0), checked
  (clang-format, frost-lint 1872 files) and its 32 `Editor` tests in `mod.ui` passed (hidden ctest); a comment
  cannot change the counts above.
- **The skips:** the same six on each OS as in the seventh batch. Windows: the four `WatcherPerf` tests,
  `PollingWatcher.DirectorySymlinksAreNotFollowed` and `SafeWrite.SymlinksStayLinks` (no symlink right for this
  account). WSL: the four `WatcherPerf` tests, `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale` and
  `CrashpadReporter.PipeThatNeverAppearsTimesOutAndStopsTheHandler` (Windows-only). The seven `msbuild` and 17
  `screenshot` tests ran and passed on Windows.
- **No test was lost:** no `TEST` line is removed between 52eb3d6 and e574965. Four assertion lines change on
  purpose: the `deleteLines` caret merge (ADR 0037), the two theme pins (604 body bytes, a 676-byte blob) and the
  long-line test's two-byte check, which now covers two documents. Against the seventh batch (win debug 2063, wsl
  debug 1960) the tree adds 58 tests on each OS, the 58 `TEST` cases the slices add: `tests/ui` 54
  (`ui_toolwindows_tests` 31, `ui_editor_tests` 23), `tests/text` 3 and `tests/input` 1. The long-line follow-up adds
  cases to an existing test.
- `-Quick` builds no ASan, TSan or fuzz tree. The slices' sanitizer, fuzz, stress and `-Full` runs are in "Deferred to
  the hardening phase (ADR 0031)", with `-Full` of `main`.

#### Merge of `w1/build-service` (eb82b95, seventh batch)
- **The merge** (eb82b95) of `w1/build-service` (8d77400) into `main` at 12edb19: BuildService, diagnostic resolution
  and index, English MSBuild output, infrastructure-only cleanup after MSBuild exits, the cancel-race fix, `$(VAR)`
  environment expansion in `.vcxproj`, and the new IProcess container calls (`containedProcesses`,
  `terminateContained`, `releaseContainer`, `abandonOutput`); the slice record below says more. The slice ran on
  `w1/project-sln` 15ba653 plus its own merge (84c332a) of `w1/build-msbuild` 111aad0, both on `main`.
- **ADR 0035** (`docs/adr/0035-build-service.md`) keeps its number; `m1/shell-frame`'s ADR, also written as 0035, is
  renumbered to 0038 (below). ADR 0035 supersedes ADR 0034 decisions 3 (language) and 4 (what ends after MSBuild
  exits), and ADR 0034's status line now says so.
- **Seven conflicts**, each resolved so that both sides' intent survives:
  - `src/CMakeLists.txt`: `build` moves into the `NOT FROST_FUZZ` block with storage, settings and project (it depends
    on frost::project, which fuzz trees do not build); `main`'s syntax and diag entries are unchanged.
  - `tests/CMakeLists.txt`: `main`'s `frost_add_test_directory` list and configure guards (`tests/build` was already
    in it).
  - `tests/build/CMakeLists.txt`: the slice's suite descriptions (BuildService over MemFs, BuildService over the real
    MSBuild) with `main`'s single kind label for `build_msbuild_tests`; its `msbuild` tag came back in the label
    follow-up (below).
  - `src/io/src/process_posix.cpp`: the slice's wake pipe, `/proc` reader and process-group listing; its own
    `openPidfd` is dropped for `main`'s `detail::openPidfd` (`child_reaper_posix.cpp`, ADR 0020).
  - `src/io/include/frost/io/scripted_process_launcher.hpp`, `src/io/src/scripted_process_launcher.cpp` and
    `tests/io/scripted_process_test.cpp`: `main`'s async-double fields (`chunkBytes`, `chunkInterval`) and includes
    and the slice's container fields (`racesTerminate`, `outputHeld`, `leftovers`) and includes.
- **The build after the merge commit:** `night.py build-gate` (39.1 GB commit free) and `cmake --build --preset
  win-clangcl-debug -j 8` (CMake re-ran on its own): exit 0, 144 steps, no compiler warning (only Qt's own "No qtpaths
  executable found" configure notice, as after every merge below).

#### Merge of `m1/shell-frame` (b1d05bc, seventh batch)
- **The merge** (b1d05bc) of `m1/shell-frame` (9576c24, on 023f354) into `main` at eb82b95: frost::ui_shell
  (`src/ui/shell`), frost::i18n in core and `qtr` in ui, and frost::app's AppServices, MainWindow on ShellWindow and
  the main() wiring; the slice record below says more.
- **ADR renumbered to 0038** (orchestrator decision: the number clashed with `w1/build-service`'s ADR 0035, which
  merged first). `docs/adr/0035-shell-frame.md` is `docs/adr/0038-shell-frame.md` (`git mv` in the merge, with a
  Number line saying so), and every "ADR 0035" that means the shell now reads "ADR 0038": the ADR's title, 22 files in
  `src/ui/shell`, `src/ui` (`tr.hpp`, `CMakeLists.txt`), `src/app` (`app_services.hpp`, `main_window.hpp`), `src/core`
  (`i18n.hpp`), `src/input/src/layout.cpp` and `tests/input/display_text_i18n_test.cpp`, and the merged comments in
  `src/app/src/main.cpp`, `src/app/CMakeLists.txt` and `src/core/CMakeLists.txt`. frost::build's references keep 0035;
  `matrix.json` had no ADR note to change.
- **Six conflicts**, each resolved so that both sides' intent survives:
  - `src/app/CMakeLists.txt`: frost::app links `main`'s frost::diag and the slice's ui_shell, ui, input and io;
    `frost` (Linux) and `frost_app.dll` link ui_shell, ui and diag; `main`'s crashpad-handler deployment stays.
  - `src/app/src/main.cpp`: `main`'s diagnostics flow (the plugin-preload warning in the log, the scoped QApplication,
    the stall watchdog and the diagnostics thread after the first frame, the exit log line and the flush; ADR 0022)
    around the slice's AppServices, the appearance install with its notifications, and the files from the command
    line. The slice's `TODO(M1)` about logging a failed preload is done by `main`'s code.
  - `src/core/CMakeLists.txt` and `tests/core/CMakeLists.txt`: `main`'s log facade (`log.cpp`, `log_test.cpp`) and the
    slice's i18n (`i18n.cpp`, `i18n_test.cpp`).
  - `tests/ui/CMakeLists.txt`: the slice's `ui_shell_tests` description and `main`'s single kind label for
    `ui_screenshot_tests` (its `screenshot` tag came back in the label follow-up).
  - `tests/features/matrix.json`: rows 55, 219 and 239-242 take the slice's status (`in-progress`) and acceptance
    tests and `main`'s milestones and `completes` (ADR 0029: rows 55, 239 and 240 complete in M15); row 226 stays
    `main`'s (M15).
- **The import checks** (the slice's merge note: `frost_app.dll` now links frost::ui_shell, ui, input and io
  statically): `frost_process_tests --gtest_filter=FrostProcess.*Import*:FrostProcess.AppDll*`, run hidden on the
  merged Debug tree. LauncherImportsOnlyKernel32, AppDllHasNoDependentLoadFlags, AppDllEmbedsNoManifest and
  AppDllImportsOnlyAllowedDlls passed, so the DLL still delay-loads ws2_32.dll and nothing else (79512d6).
- **Reserved action ids** (the slice's note): the shell's actions follow the IntelliJ keymap ids, plus
  `window.closeOtherTabs`, `window.closeAllTabs`, `window.closeAllButPinned`, `window.pinTab`, `window.notifications`,
  `view.toggleToolbar`, `view.toggleStatusBar`, `view.compactMode`, `view.zoomInIde`, `view.zoomOutIde`,
  `view.resetIdeZoom`, `file.openFile`, `file.exit` and `help.about`. Other slices must not register these ids.
- **Orchestrator decisions on its questions:** the renumbering above. The three `FrostProcess.Restart*` tests that
  fail when `frost_process_tests` runs directly through `run_hidden_nobox.py`, outside ctest, are environmental, not a
  regression; they pass under `verify.ps1` (Verification below).
- **The build after the merge commit:** `night.py build-gate` (40.8 GB commit free) and the Debug build: exit 0, 178
  steps, no compiler warning.

#### Merge of `w2/clangd-diagnostics` (db9d24e, seventh batch)
- **The merge** (db9d24e) of `w2/clangd-diagnostics` (e7d9ccc, on 023f354 plus its merge a96da6f of `m1/settings`)
  into `main` at b1d05bc: frost::lang (`src/lang`, layer lang at rank 7) with the LSP client, clangd supervision,
  compile-command providers, document sync and the diagnostics store, and the frost::project vcxproj reader's
  ClCompile metadata; the slice record below says more. ADR 0036 (`docs/adr/0036-clangd-lsp-client-diagnostics.md`);
  the number is unchanged and no other ADR on `main` uses it.
- **Four conflicts**, each resolved so that both sides' intent survives:
  - `src/CMakeLists.txt`: `lang` joins `main`'s `NOT FROST_FUZZ` block after `build` (it needs project, settings and
    yyjson).
  - `src/core/CMakeLists.txt`: `main`'s i18n and log sources. The slice's frost::core log facade is a byte-for-byte
    copy of `main`'s (from `m1/crash-diag`), so its add/add resolved to `main`'s files (orchestrator decision).
  - `tests/CMakeLists.txt`: `tests/lang` comes in through `frost_add_test_directory`, after `build`, so its tests
    carry `mod.lang`.
  - `tests/features/matrix.json`: row 98 takes the slice's `in-progress` and acceptance test; rows 59, 60, 66 and 99
    stay `main`'s (ADR 0029's milestones; `m1/settings`' row 59).
- **One change without a textual conflict:** `lang_clangd_tests` said `LABELS clangd`, which `main`'s `frost_add_test`
  refuses (one kind label, ADR 0027). In the merge it is `integration`: it starts real clangd processes, so it takes
  `RESOURCE_LOCK frost_desktop`. Its `clangd` tag came back in the label follow-up.
- **Orchestrator decisions on its questions:** the log facade above. The `.props`-sheet gap in the synthesized compile
  commands is being closed by `w2/clangd-nav` (in flight) with an MSBuild-evaluated provider (`msbuild
  -getItem:ClCompile`).
- **The build after the merge commit:** `night.py build-gate` (34.6 GB commit free) and the Debug build: exit 0, 64
  steps, no compiler warning.

#### Merge of `m1/save-manager` (5fbd262, seventh batch)
- **The merge** (5fbd262) of `m1/save-manager` (e2b2362) into `main` at db9d24e: frost::document (`src/document`, the
  new layer `document` at rank 6 in `cmake/FrostLayers.cmake`) with LoadService, FileDocument, SaveManager,
  AutosaveController and SavePolicies, and in frost::io `IFileSystem::safeWrite`, `parseAtomicTempName` and
  `removeStaleTempFiles`; the slice record below says more. ADR 0028 (`docs/adr/0028-save-manager.md`); the number is
  unchanged and no other ADR on `main` uses it. The branch's own merges of `m1/diff-merge` 7851f95 (6ef9c47) and
  `m1/settings` 481fc26 (fc17004) were already on `main`, so those parts were no-ops (orchestrator decision).
- **Five conflicts**, each resolved so that both sides' intent survives:
  - `src/CMakeLists.txt`: `document` joins `main`'s `NOT FROST_FUZZ` block after `project` (rank 6, below build and
    lang at 7); `diff` stays where `main` has it, after `text`.
  - `tests/CMakeLists.txt`: `tests/document` comes in through `frost_add_test_directory`, after `project`, so its
    tests carry `mod.document`.
  - `NOTICE` and `tests/fuzz/CMakeLists.txt`: `main`'s text, which already holds the JGit paragraph (wrapped) and the
    `diff_merge` target that the branch had from `m1/diff-merge`.
  - `tests/features/matrix.json`: rows I2 and 218 take the slice's acceptance tests (both stay `in-progress` until the
    UI is wired); every other row stays `main`'s.
- **Orchestrator decisions on its questions:** FrostLayers gets the `document` layer at rank 6. Its known bug
  "document keys are case-sensitive on Windows" is being fixed in `m1/shell-nav` (canonical real-case paths at every
  open).
- **Orchestrator decision on ADR 0019 Q5** (under Questions for the user until this batch; moved here): diff3-style
  conflict markers stay the default, and the save-manager prompt's labels are "Keep my version", "Take disk version"
  and "Compare..." (`m1/shell-nav` builds the merge view).
- **PLAN.md section 3.2** still lists SaveManager under `src/text`; ADR 0028 decision 1 and the comment at
  `FROST_LAYER_RANK_document` in `cmake/FrostLayers.cmake` record `src/document`. Not changed in this batch (deferred
  list).
- **The build after the merge commit:** `night.py build-gate` (34.6 GB commit free) and the Debug build: exit 0, 272
  steps, no compiler warning.

#### Label follow-up (8ee7a1d, seventh batch)
- **Tags after the kind label** (the pre-state's follow-up): `frost_add_test`'s `LABELS` starts with the kind (unit,
  integration, stress or perf; default unit), which alone sets the scheduling defaults; any labels after it are
  free-form tags, lower-case names that are not a kind. The labels reach `gtest_discover_tests` as one quoted
  argument, so ctest sees one LABELS value instead of a property per element (the reason the sixth batch had to cut
  them).
- **Restored:** `build_msbuild_tests` has `LABELS integration msbuild`, `ui_screenshot_tests` `LABELS unit screenshot`
  and `lang_clangd_tests` `LABELS integration clangd`, so `ctest -L msbuild`, `-L screenshot` and `-L clangd` select
  them again: 7, 17 and 3 tests in `ctest -N` on the Windows Debug tree, which has 2132 tests before and after the
  change. The two CMake comments that named a `-R` filter instead are updated.
- **The self-test:** `tools.verify_support`'s BuildTreeLabelsTest gains `test_one_kind_label_and_tags_after_it`: in
  the build tree every test has exactly one kind label, and the three tagged suites carry their tag, their kind and,
  for integration, the `frost_desktop` lock. It and the suite passed on the Windows Debug tree (hidden). ADR 0027's
  decisions 3 and 5 and its Consequences are amended to say so.
- **A slip on the way:** the first version took the tags with `list(SUBLIST ... 1 -1 ...)`, which CMake 4.4.5 rejects
  for a one-element list ("begin index: 1 is out of range"), so the configure failed; `list(POP_FRONT)` on a copy
  replaced it before the commit.
- **The build after the commit:** `night.py build-gate` (39.0 GB commit free) and the Debug build (CMake re-ran): exit
  0, 26 steps.

#### Verification (seventh batch, merged tree 8ee7a1d)
- Before the batch run: the Windows Debug build after each of the four merges and the label follow-up (above), the
  import checks after `m1/shell-frame`, and `tools.verify_support` after the follow-up.
- **The first run** of `tools\verify.ps1 -Quick -Jobs 8`, on 8ee7a1d (clean tree; this record not yet written) through
  `run_hidden_nobox.py` (a hidden desktop; no window opened on it), after `night.py build-gate` (40.9 GB commit free),
  517.5 s, exit 1: `win debug 2010/2063 (6 skipped)`, `wsl debug 1954/1960 (6 skipped)`; format, frost_lint (1814
  files), licensecheck and featurecheck ok. WSL was green. On Windows 47 tests failed, every one in code that runs on
  ScriptedProcessLauncher: `ScriptedProcessLauncher.*` and `ScriptedAsyncProcess.*` (22, `io_unit_tests`),
  `BuildSession.*`, `ServiceTest.*`, `StartMsBuild.*` and `ListVsInstances.*` (19, `build_unit_tests`),
  `CrashpadReporter.*` (5, `diag_tests`) and `DiffGit.RunnerFaults`. Each died at once with an access violation (SEH
  0xc0000005 in the test body, 0 ms).
- **No source change fixed it:** the merged scripted launcher matched both parents' code, and recompiling only
  `scripted_process_launcher.cpp.obj` and `scripted_process_test.cpp.obj` with `SCCACHE_RECACHE=1` made all 22 io
  tests pass. So every Debug object compiled since the first merge (278) was deleted and rebuilt with
  `SCCACHE_RECACHE=1` (exit 0, 382 steps, 114 s, no compiler warning), and ctest `--rerun-failed -j 8` (hidden) passed
  all 47. The likely cause is a bad object from the shared sccache cache or a stale incremental object; it is in the
  deferred list. Nothing in the tree was changed, so no fix commit.
- **The second run**, on 8ee7a1d again (this record not yet written), after `night.py build-gate` (40.4 GB commit
  free), 398.1 s, exit 0. Windows ran its Debug leg (ctest `-j 8`) and the checks; WSL ran `tools/verify.sh` in the
  ext4 mirror (`~/frost-src-e235f3842261`): `Verified: win debug 2057/2063 (6 skipped), wsl debug 1954/1960 (6
  skipped), wsl tsan-quick not-run (off by default), wsl tidy not-run (off by default), wsl licensecheck ok, format
  ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run (off by default)` (frost-lint 1814 files;
  licensecheck 39 packages on Windows, 35 on WSL). No test failed. The test steps took 135.4 s (Windows, parallel at
  `-j 8`) and 227.3 s (WSL, serial). Another agent's `-Quick` (`w1/tool-windows`, in its own worktree) ran beside the
  first run.
- **The skips:** on Windows the four `WatcherPerf` tests (outside optimised, unsanitised builds),
  `PollingWatcher.DirectorySymlinksAreNotFollowed` (creating a directory symlink fails for this account) and, new,
  `SafeWrite.SymlinksStayLinks` (the same reason; `m1/save-manager`'s known bug); on WSL the four `WatcherPerf` tests,
  `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale` (no translated locale) and
  `CrashpadReporter.PipeThatNeverAppearsTimesOutAndStopsTheHandler` (Windows-only). The seven `msbuild` tests, the
  three `clangd` tests (on both OSes), the 17 `screenshot` tests and the `FrostProcess.Restart*` tests ran and passed.
- **No test was lost in the four merges:** no `TEST` and no assertion line is removed between 12edb19 and 8ee7a1d.
  Against the sixth batch (win debug 1890, wsl debug 1789) the tree adds 173 tests on Windows and 171 on WSL; the
  slices' test files add 170 `TEST` cases (`tests/lang` 57, `tests/document` 37, `tests/ui` 31, `tests/build` 21,
  `tests/io` 16, `tests/core` 5, `tests/project` 2, `tests/input` 1), and the rest are parameterised and platform-only
  cases.
- `-Quick` builds no ASan, TSan or fuzz tree. The slices' sanitizer, fuzz, stress and `-Full` runs are in "Deferred to
  the hardening phase (ADR 0031)", with `-Full` of `main`.

#### Merge of `m1/verify-tooling` (08b92fd, sixth batch)
- **The merge** (08b92fd) of `m1/verify-tooling` (2e286e0) into `main` at 2e298a8: parallel Windows ctest, no error
  boxes from tests, isolation defaults by kind label, `mod.*` module labels, the WSL PATH filter, `tools/perf.ps1` and
  the B28 harness, and clang-tidy and the TSan quick leg behind off-by-default switches; the slice record below says
  what they hold. ADR 0027; the number is unchanged and no other branch uses it. The slice ran on 5f377e7, the
  piece-tree merge, 43 commits behind `main`.
- **Seven conflicts**, each resolved so that both sides' intent survives:
  - `tests/CMakeLists.txt`: every test directory comes in through `frost_add_test_directory`, `main`'s `input`,
    `storage`, `project`, `build`, `diag` (under `NOT FROST_FUZZ`) and `ui` included, so the slice's configure guard
    passes and every test gets its `mod.<directory>` label (the slice's merge hazard 1).
  - `tests/tools/CMakeLists.txt`: `main`'s `asset_fetch` and `symbolize` suites and the slice's `verify_support`,
    `frost_perf` and `verify_scripts` suites are all registered. They now run as scripts in the build directory, as
    the slice's working-directory guard requires; both of `main`'s suites find their files from `__file__`.
  - `tests/cmake/CMakeLists.txt`: `main`'s `io_no_static_init` test and the slice's `PROCESSORS 4` for
    `FrostRuntimeDlls.CopiesOncePerDirectory`.
  - `tests/text/CMakeLists.txt`: `main`'s suite descriptions (anchors and undo, the display map, the encodings) and
    the slice's parallel-ctest note and `PROCESSORS` settings.
  - `tests/io/process_test.cpp`: `main` had moved `ProcessWatch` into `process_env.hpp` (ADR 0020); the slice's
    member-initializer constructors (a clang-tidy fix) are applied there.
  - `tools/verify.ps1` and `tools/verify.sh`: the slice's restructured steps (the changed-file list, ctest `-j`, tidy,
    the TSan quick leg, `-LE '^stress$'`) with `main`'s `-Full`-only symbolize step (ADR 0022) after the Windows
    configurations and after the Linux Debug run.
- **The slice's other merge hazards**, applied in the merge:
  - Hazards 2 and 4: `ui_screenshot_tests` takes `LABELS unit` and `build_msbuild_tests` `LABELS integration`, since
    `frost_add_test` now refuses more than one label. A correction to the hand-off: on `main` both second labels
    (`screenshot` and `msbuild`) were real ctest labels (the old `PROPERTIES LABELS "${ARG_LABELS}"` passed the list
    as one argument; the generated test files had `[[LABELS]] [[integration;msbuild]]`), so they were not dropped
    before. No script or preset selects by either label, so no test is lost, but `ctest -L msbuild` now selects
    nothing; the two CMake comments say so and name a `-R` filter. Several labels per test were deferred; the
    seventh batch's label follow-up (above) brought both back as tags.
  - Hazard 3: `ui_native_tests`, which calls `gtest_discover_tests` itself, holds `RESOURCE_LOCK frost_desktop`.
  - Hazard 4: `build_msbuild_tests` (MSBuild, cmd and ping), `diag_integration_tests` (crash_child, the crashpad
    handler, llvm-symbolizer and llvm-readobj) and `frost_process_tests` (frost.exe and its self-tests) are
    integration suites, so they hold `frost_desktop` through the default. `diag_tests` starts no process (its crash
    reporter tests run on ScriptedProcessLauncher), and no suite needed `RUN_SERIAL`.
  - Hazard 5: the F2 TODO about the FindOpenGL searches (entry "M0 decisions") is replaced by a done note.
- **Orchestrator decisions on its questions:** B28's `-Base HEAD` method stays recorded in ADR 0027 only;
  `budgets.json` is not changed in this phase (a budget-text change goes to the user at hardening); `-Tidy` and
  `-TsanQuick` go back to on-by-default when the hardening phase starts (PLAN.md 6.4; ADR 0031 defers, never removes).
- **The build after the merge commit:** `night.py build-gate` (no lock, 40.7 GB commit free) and `cmake --build
  --preset win-clangcl-debug -j 8` (CMake re-ran on its own): exit 0, 186 steps, no compiler warning (only Qt's own
  "No qtpaths executable found" configure notice, as after every merge below).

#### Merge of `m1/syntax` (b706290, sixth batch)
- **The merge** (b706290) of `m1/syntax` (681c749) into `main` at 08b92fd: frost::syntax (tree-sitter 0.27.1, seven
  MIT grammars, our own queries, SyntaxSession and SyntaxTree) and the vendor-lock tooling; the slice record below
  says what they hold. ADR 0023; the number is unchanged and no other branch uses it.
- **Four conflicts**, each resolved so that both sides' intent survives:
  - `src/CMakeLists.txt`: `syntax` follows `input`, outside the `NOT FROST_FUZZ` blocks (it needs no vcpkg port;
    tree-sitter is vendored), and `main`'s storage, project, build and diag blocks are unchanged. The `syntax` rank
    (5) was already in `cmake/FrostLayers.cmake`.
  - `tests/CMakeLists.txt`: `tests/syntax` comes in through `frost_add_test_directory`, after `input`.
  - `tools/verify.ps1` and `tools/verify.sh`: the slice's format exclusion of `third_party/tree-sitter` and
    `third_party/grammars` is applied to `m1/verify-tooling`'s restructured format step.
  - Merged without conflict: `REUSE.toml`, `NOTICE`, `.gitattributes`, `cmake/FrostCompilerOptions.cmake`,
    `cmake/FrostEmbed.cmake`, `tools/asset_fetch.py`, `tools/frost_lint.py`, `tools/licensecheck.py` and their tests.
- **One interaction is left for the hardening phase:** `m1/verify-tooling`'s clang-tidy step (`-Tidy`, off) excludes
  only the banned probes, fixtures and `build/` (`_EXCLUDED` in `tools/verify_support.py`), so once tidy is on it
  would analyse the vendored tree-sitter C sources. Excluding `third_party/` there, as the format step does, is in
  the deferred list.
- **The build after the merge commit:** `night.py build-gate` (40.4 GB commit free) and the Debug build: exit 0, 56
  steps, no compiler warning.

#### Merge of `m1/editor-model` (b2a6b49, sixth batch)
- **The merge** (b2a6b49) of `m1/editor-model` (0b4aac1) into `main` at b706290: frost::text_editor, the Qt-free
  editor commands and frost::text::Editor, with RE2 (and Abseil) and utf8proc from vcpkg; the slice record below says
  what they hold. ADR 0025; the number is unchanged and no other branch uses it.
- **Two conflicts**, each resolved so that both sides' intent survives:
  - `vcpkg.json`: asio, cpptrace, crashpad, curl (Linux only, no default features), gtest, pugixml, re2, spdlog,
    sqlite3, utf8proc and xxhash, in name order.
  - `tests/CMakeLists.txt`: `main`'s directory list is kept, and `tests/text/editor` is added from
    `tests/text/CMakeLists.txt` (`add_subdirectory(editor)`), so its tests carry `mod.text`.
    `frost_add_test_directory(text/editor)` would label them `mod.text/editor`, which `tools.verify_support`'s label
    test rejects (a module label names a top-level test directory) and the TSan selection never names. Fuzz trees add
    only `tests/fuzz`, so the tests need no `NOT FROST_FUZZ` guard; `src/text/editor` keeps its own.
  - Merged without conflict: the root `CMakeLists.txt` (`find_package` of re2 and utf8proc under `NOT FROST_FUZZ`),
    `src/CMakeLists.txt` (`text/editor` under the same guard) and `Snapshot::reversedChunks` in frost::text.
- **Orchestrator decisions on its questions:** Up on the first row and Down on the last row keep the caret where it is
  (Rider behaviour). deleteLines with several carets should merge carets that land on the same line; that fix is being
  made in `m1/editor-view` (in flight).
- **The build after the merge commit:** `night.py build-gate` (37.6 GB commit free) and the Debug build: exit 0, 114
  steps (vcpkg installed abseil, re2 and utf8proc), no compiler warning.

#### Merge of `m1/diff-merge` (88d75ac, sixth batch)
- **The merge** (88d75ac) of `m1/diff-merge` (7851f95) into `main` at b2a6b49: frost::diff, the histogram line diff
  and the three-way merge cores; the slice record below says what they hold. ADR 0019; the number is unchanged and no
  other branch uses it. The slice ran on 6fb6826, `m1/piece-tree`'s head.
- **Four conflicts**, each resolved so that both sides' intent survives:
  - `src/CMakeLists.txt`: `diff` follows `text`, outside the `NOT FROST_FUZZ` blocks (it needs only core and text, and
    the new fuzz target links it).
  - `tests/CMakeLists.txt`: `tests/diff` comes in through `frost_add_test_directory`, after `text` and before `fuzz`,
    which reuses its support library.
  - `tests/fuzz/CMakeLists.txt`: `main`'s `io_ignore_rules` and `text_decode` targets and the slice's `diff_merge`
    target are all registered.
  - `NOTICE`: `main`'s asset and tree-sitter paragraphs, then the slice's JGit attribution as a paragraph of its own.
- **PLAN.md section 3.2** (ADR 0019 D1 and Q6, left to the integrator): the layout gains `src/diff/` (histogram line
  diff, refinement, three-way merge cores, ADR 0019), and `src/vcs/`'s entry reads "git (on src/diff's diff and
  merge)".
- **For `m1/save-manager`** (ADR 0019 D1): FrostLayers forbids edges between different layers of equal rank, so a
  target that links frost::storage (layer storage, rank 4) and frost::diff (layer text, rank 4) needs rank 5 or more.
- **Orchestrator decisions on its questions:** its LongPathsEnabled blocker is not a blocker. The two FrostProcess
  long-path tests failed only on its old base 6fb6826; `main` at 023f354 runs and passes them, and so does this
  batch's run (Verification). Q4: keep the 2^22 cap everywhere (option (a)). Q2: the rewritten histogram, which cites
  JGit's algorithm only, is accepted; no code-level port. Q1 and Q3 (a pinned classic-dialect git oracle; which git
  defines M5's criterion) are deferred to M5 and the hardening phase. Q5 was decided in the seventh batch (the merge
  of `m1/save-manager` above).
- **The build after the merge commit:** `night.py build-gate` (36.8 GB commit free) and the Debug build: exit 0, 73
  steps, no compiler warning.

#### Merge of `m1/settings` (8a5768e, sixth batch)
- **The merge** (8a5768e) of `m1/settings` (481fc26) into `main` at 88d75ac: frost::settings, the typed settings
  registry on yyjson 0.13.0 (MIT, vcpkg, PRIVATE); the slice record below says what it holds. ADR 0021; the number is
  unchanged and no other branch uses it.
- **Five conflicts**, each resolved so that both sides' intent survives:
  - The root `CMakeLists.txt`: `main`'s pugixml and the slice's yyjson `find_package` calls under `NOT FROST_FUZZ`.
  - `src/CMakeLists.txt`: `settings` joins `storage` and `project` in the `NOT FROST_FUZZ` block (fuzz trees install
    no vcpkg ports). The `settings` rank (5) was already in `cmake/FrostLayers.cmake`.
  - `tests/CMakeLists.txt`: `tests/settings` comes in through `frost_add_test_directory`, after `storage`.
  - `vcpkg.json`: utf8proc, xxhash and yyjson, in name order, after `main`'s entries.
  - `tests/features/matrix.json`: row 59 (EditorConfig) takes the slice's `in-progress` with its acceptance test; row
    60 keeps `main`'s `in-progress` with `EncodingRoundTrip.FixturesByteExact`; row I1 merged on its own (the slice's
    `in-progress`).
- **The build after the merge commit:** `night.py build-gate` (34.7 GB commit free) and the Debug build: exit 0, 42
  steps (vcpkg installed yyjson 0.13.0), no compiler warning.

#### Merge of `plan/thin-path` (414155a, sixth batch)
- **The merge** (414155a) of `plan/thin-path` (6315d86) into `main` at 8a5768e: ADR 0029, accepted with the user's
  answers, the thin path to a first usable build (a minimal debugger in U1's M4; the deferred halves of M1-M4 in the
  new M15, M16 and M17), W1 and the feature-first phase in PLAN.md section 7, gates moved with their features in
  `budgets.json` (targets unchanged), featurecheck and frost-lint for M0..M17 with M0 closed, and the retagged to-do
  markers; the slice record below says more. The number is unchanged and no other branch uses it (`main`'s ADRs
  after 0010 are now 0011 to 0023, 0025 to 0027 and 0029 to 0034; 0024 and 0028 are not on `main`). The branch had
  merged `main` at dcfd9ae; `main` was 21 commits further.
- **Two conflicts**, each resolved so that both sides' intent survives:
  - `src/app/src/main.cpp`: `main`'s note from `m1/crash-diag` (crash reports record `restartedFrom`; logs and dumps
    live under the cache root) is kept, its `TODO(M1)` keeps only restarting frost, and the branch's `TODO(M16)` takes
    registering frost and the AppUserModelID.
  - `tests/features/matrix.json`: row 59 (EditorConfig) takes `main`'s status and acceptance test (`m1/settings`) and
    the plan's owner, M15. `featurecheck.py generate` then left the file unchanged, and `featurecheck.py check`
    reports no problem.
- **The to-do markers:** frost-lint on the merged tree (1697 files) finds no violation, so every marker the batch's
  slices added names an open milestone of M0..M17. From now on, slices tag debugger work M4 and work that ADR 0029
  moved M15, M16 or M17 (the branch's note).
- **Orchestrator decision on its question:** the debugger contents in ADR 0029 decision 12 follow Q4's own wording
  (line breakpoints, stepping, call stack, threads, variables, watches, Evaluate, stop on crash, MSVC programs
  through PDBs, Windows and Linux); settled, not asked again.
- **The build after the merge commit:** `night.py build-gate` (35.0 GB commit free) and the Debug build: exit 0, 256
  steps (the comment edits in shared headers rebuilt most targets), no compiler warning.

#### Verification (sixth batch, merged tree 358ffd3)
- Before the batch run: the Windows Debug build after each of the six merges (above).
- **The first run** of `tools\verify.ps1 -Quick -Jobs 8`, on 414155a (with this record uncommitted) through
  `run_hidden_nobox.py` (a hidden desktop; no window opened on it), after `night.py build-gate` (no lock, 35.0 GB
  commit free), 472.9 s, exit 1: `win debug 1884/1890 (5 skipped)`, `wsl debug 1782/1789 (6 skipped)`, with one
  failure on each OS, `tools.verify_support`, and `format FAILED (1 files)`; frost_lint, licensecheck and
  featurecheck passed. Both came from the `m1/verify-tooling` merge:
  - `TsanSelectionTest.test_selection_matches_the_cmake_deps_oracle` (module `sched`): `['sched', 'text'] != ['io',
    'sched', 'text']`. The oracle derives the selection from the real CMake graph, and on `main` `src/io` links
    frost::sched (the reactor and watchers, ADR 0020), which the slice's base did not, so a sched change must also
    run io's TSan tests.
  - clang-format wanted the `ProcessWatch` constructor that the merge rewrote in `tests/io/process_env.hpp` (the
    slice's member initializer) on one line: `io::ProcessId` there is shorter than `frost::io::ProcessId` was in
    the slice's `process_test.cpp`.
- **The fix** (358ffd3), forward and without weakening: `_TSAN_CLOSURE["sched"]` in `tools/verify_support.py` now
  includes io, and the hand-written table test's two sched rows expect it (the selection only grows; the oracle
  test is unchanged); `process_env.hpp` is formatted with the pinned clang-format. `tools.verify_support` then passed
  on Windows (hidden desktop).
- **The second run**, on 358ffd3 (with this record uncommitted), after `night.py build-gate` (no lock, 34.3 GB commit
  free), 412.4 s, exit 0. Windows ran its Debug leg (ctest `-j 8`) and the checks; WSL ran `tools/verify.sh` in the
  ext4 mirror (`~/frost-src-e235f3842261`):
  `Verified: win debug 1885/1890 (5 skipped), wsl debug 1783/1789 (6 skipped), wsl tsan-quick not-run (off by
  default), wsl tidy not-run (off by default), wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok,
  featurecheck ok, tidy not-run (off by default)` (frost-lint 1697 files; licensecheck 39 packages on Windows, 35 on
  WSL). No test failed. The test steps took 139.1 s (Windows, parallel at `-j 8`; 265.5 s and 255.1 s serially in the
  last two batches with fewer tests) and 228.2 s (WSL, serial). Other agents' `-Quick` runs (`m1/save-manager` and
  `w2/clangd-diagnostics`, in their own worktrees) ran at the same time.
- **The skips are the last batch's:** on Windows the four `WatcherPerf` tests (outside optimised, unsanitised builds)
  and `PollingWatcher.DirectorySymlinksAreNotFollowed` (creating a directory symlink fails for this account); on WSL
  the four `WatcherPerf` tests, `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale` (no translated locale) and
  `CrashpadReporter.PipeThatNeverAppearsTimesOutAndStopsTheHandler` (Windows-only). The FrostProcess long-path tests
  (`RunsFromPathLongerThanMaxPath`, `PlatformPluginPathBoundary`), which failed on `m1/diff-merge`'s old base, ran
  and passed, and so did the six MSBuild integration tests, now under the `frost_desktop` lock.
- **No test was lost in the six merges.** Against the fifth batch (win debug 1671, wsl debug 1570) the tree adds 219
  tests on each OS, which is what the slices added over their bases: `m1/syntax` 42 (1176 and 1090 at 681c749,
  against 1134 and 1048 at e9c3214), `m1/editor-model` 48 (904 and 817 at 0b4aac1, against 856 and 769 at 877893b),
  `m1/diff-merge` 93 (the 92 `mod.diff` tests and the `diff_merge` corpus replay), `m1/settings` 33 and
  `m1/verify-tooling` 3 (the `verify_support`, `frost_perf` and `verify_scripts` suites); `plan/thin-path` adds none.
- `-Quick` builds no ASan, TSan or fuzz tree. The fuzz trees now build frost::diff and frost::syntax as well, and
  register the `diff_merge` target; neither fuzz tree was built. The slices' sanitizer, fuzz, stress and `-Full` runs
  are in "Deferred to the hardening phase (ADR 0031)", with `-Full` of `main`.

### Next
- **W1, the work-ready build** (ADR 0032): BuildService (ADR 0035), the shell (ADR 0038), the editor view (ADR 0037),
  the build tool windows (ADR 0039) and, since the ninth batch, RunService with frost::ui_run (ADR 0040) are on `main`,
  none of them wired into the app yet. Since the tenth batch `main` also has `m1/shell-nav`'s composition (ADR 0042:
  AppServices owns the document stack, the hot-exit journal, sessions, the thread pool and the watcher; MainWindow
  owns a Workbench). `w1/workbench` (in flight, on 8ee7a1d with its own merges 591ad72 and 43908a5 of the two slices)
  wires the W1 pieces in, but duplicates part of that composition (open project, the document service, the save
  actions, the hot-exit prompt, the external-change notification); a dedicated unify slice reconciles the two after
  the tenth batch, and `w1/workbench` is not merged as it stands. From `w1/tool-windows`'
  hand-off: one BuildController and one BuildServiceBackend per main window; `registerActions`;
  `setModuleDefaults(buildShortcutBindings())`; a TrustPrompter on the main window; ISaveAllHandler from the editor
  host; `openFile`/`openLocation` to the editor; ConfigurationCombo and the BuildActions QActions on the toolbar;
  `requestFailed` to notifications. From `w1/run-service`'s: RunConsole into the Run tool window,
  `RunController::bindStartupCombo(MainToolbar::runConfigurations())`, `registerRunActions` with the Solution
  Explorer's selection for `run.setStartupProject`, RunService created with `RunController::callbacks()` and
  `makeRunBuilder(buildService)`, and `RunService::defaultStatePath()`. From `m1/editor-rhi`'s (orchestrator
  decision): a render self-test of the QRhi viewport on a hidden desktop, with the preview defaulting to raster if it
  fails, since the view-level QRhi path is unverified on screen. From `m1/editor-view`'s: the error list feeds
  `setLineMarkers`, and the views of one document share one SyntaxSession. W1 docks the Solution Explorer with
  `toolWindows().setContent("project", ...)` and the build output and problems windows with
  `setContent("run"/"problems", ...)`, registers `build.*` and `run.*` actions (which light up the toolbar slots and
  the Build/Run menus) and fills `mainToolbar().runConfigurations()`. Then opening a `.sln`/`.slnx` from the command
  line, File > Open and the recent list, then the rest of U1 in ADR 0029's order.
- **W2, clangd** (ADR 0036): the UI slice wires ClangdService into the app (start on project open with the Existing
  then the Solution providers; open, change, save and close from the editor's Document ChangeSets), squiggles from
  DiagnosticsStore, and a Problems window that merges build and live diagnostics; then completion and go-to-definition
  through `ClangdService::request()`. `w2/clangd-nav` (ADR 0041, on `main` since the ninth batch) has the models,
  frost::ui_lang and the MSBuild-evaluated compile-command provider; the wiring implements IEditorLanguageHost over
  EditorView and IWorkspaceDocuments over the open documents, docks the usages panel, and feeds
  `solutionCompileCommandsChain` with `frost::build::findMsBuild` and the live trust state.
- **`frost-console-host`** (orchestrator decision on `w1/run-service`'s first known bug): a tiny own console helper exe
  that starts the program directly with the exact command line, waits, prints "Press any key to continue" and keeps
  the exit code, replacing the external console's `cmd /c` wrapper, so cmd.exe no longer interprets `^ & | < >` and
  `%VAR%` in the user's arguments.
- **The shell's consumers** (ADR 0038): the editor view is on `main`; `w1/workbench` (or the unify slice) wraps
  `createEditorView` in the shell's IEditorFactory, maps EditorViewStatus to EditorStatus and connects the status
  signals; adapters bind IShellSettings and IKeymapSettings to the m1/settings registry (in-memory defaults until
  then). Find Action on ListPopup is on `main` since the tenth batch (`m1/shell-nav`). The shell's action ids are
  reserved (its merge section above).
- **`m1/shell-nav`'s hand-off** (ADR 0042): swap PlaceholderEditorFactory for the real editor factory under
  DocumentEditorFactory in AppServices; that factory, over `m1/editor-view`'s EditorView, implements the seam's
  `viewState`/`restoreViewState` and `IEditorFactory::canonicalPath`; `w1/tool-windows`' document bindings go through
  the DocumentRegistry; and the settings-file actions bind to `SettingsStore::prepareForEditing` once AppServices
  holds a SettingsStore (frost::settings itself is on `main` since the sixth batch; the hand-off's "when m1/settings
  lands" means that binding).
- **`m2/find-in-files`' hand-off** (ADR 0045): W1 (the unify slice) installs FindFeature in MainWindow and connects
  `openLocation` (to the editor), `selectedText` (from the editor view) and `documentEdited` (to `main`'s
  `Editor::follow`, so an open editor redraws after a replace). `openLocation` has to map the engine's UTF-8 byte
  columns to the editor's columns (the slice record's known bug). Then its TODO(M2)s: Replace in Files for closed
  files on a background lane with progress (it runs on the GUI thread now, accepted for the moment by the
  orchestrator), and the Directory scope through `m1/shell-nav`'s gitignore-aware scan (io `scanTree` with
  `.gitignore`, `.ignore` and the default excludes, ADR 0042). Later: Replace from the Find tool window, tabs for
  several searches, the last query, mask and scope persisted through m1/settings with history drop-downs, and Alt
  mnemonics for the toggles.
- **`m3/editor-insight`'s hand-off** (ADR 0047): the workbench (the W1 unify slice) creates one EditorInsightBinding
  per editor and calls `attach(clangdService)` after `openDocument`; it sends build results through
  `setBuildDiagnostics(buildService.diagnosticsForDocument(path))` from BuildService's `onDiagnostic` and
  `onDiagnosticsCleared`, puts InsightStatusIndicator in the status bar with `setView(activeEditor)`, connects
  `problemsViewRequested` to the Problems tool window, and routes the LanguageController host's `setUsageHighlights`
  through `toUsageHighlights` into `EditorView::setUsageHighlights`. The binding's first known bug makes the order
  binding: each edit goes to clangd (`changeDocument`) before the 300 ms token debounce fires. W1 can move its own
  marks (spelling, run-to-cursor, breakpoint tints) onto `setDecorations("<owner>", ...)`. `m1/editor-rhi`'s GPU
  backend has to draw the new display-list parts (decorations, the stripe, chrome and chromeText); until it does,
  only the raster backend shows the insight. (It draws `m5/git-core`'s change stripes since 25d83a8.)
- **`m2/cmake`'s hand-off** (ADR 0046): W1 wires CMakeToolWindow and CMakeTreeModel into the Solution Explorer and the
  ToolWindowManager for a folder with a CMakeLists.txt, and adds a BuildController adapter for CMakeService (its
  BuildState, BuildOutputKind lines and diagnostics mirror BuildService's; profiles are the configurations and
  targets the projects), and puts BuildDirectoryCompileCommandsProvider (with
  `CMakeService::activeBuildDirectory`) into clangd's provider chain for CMake folders.
- **`m5/git-core`'s hand-off** (ADR 0048): wire the git UI into the running workbench. One GitController over
  GitService per project (with the shell's BackgroundTaskManager and NotificationCenter), the Commit window in the
  Commit tool window (`tool_windows::kCommit`, whose focus already sets `CommitViewFocus`), the branch widget in the
  status bar, the git console, `registerGitActions` with GitActionTargets, `vcsFileColorProvider` on the Solution
  Explorer (`SolutionTreeModel::setFileColorProvider`), the editor tabs (`EditorTabs::setTitleColorProvider`) and
  `m1/shell-nav`'s File System view (on `main` since the tenth batch; ADR 0048's "the view does not exist yet" was
  true of the slice's base only), and one EditorChangeMarkers per EditorView. Then the M5 diff follow-ups, first an
  editable right (working-copy) side in the diff viewer (orchestrator decision: it stays read-only until then).
- **The SaveManager's consumers** (ADR 0028): one manager on the document thread; AutosaveController fed by input
  events, focus loss and a timer; the merge view driven by PendingMerge and `resolveExternalChange` (with the Q5
  labels above; changes on adjacent lines in both buffer and disk count as a conflict, the same rule as git); banners
  for `DiskState::OutOfDate` and `ReadOnlyReason::LongLine`; `restore()` after `findRecoverableJournals` on startup.
  `m1/shell-nav` brought the merge view, the banners, autosave and the restore prompt in the tenth batch; the editor
  host feeding the view's `setFileInfo`/`markSaved` is still `w1/workbench`'s (or the unify slice's). The long-line
  rule uses `kLongLineCapBytes` since the eighth batch (its long-line follow-up above).
- **The in-flight slices:** `w1/workbench` (the W1 wiring above), held for the unify slice that reconciles it with
  `m1/shell-nav`'s composition; the unify slice also takes up `m3/editor-insight`'s and `m2/cmake`'s wiring above.
  `m5/git-core` merged in the thirteenth batch, `m3/editor-insight` and `m2/cmake` in the twelfth, `m2/find-in-files`
  in the eleventh, `m1/shell-nav` in the tenth; `m1/editor-rhi`, `w1/run-service` and `w2/clangd-nav` in the ninth.
- **The frost::text_editor consumers** (ADR 0025): `m1/editor-view` (on `main`) binds keys to `commands::*`, calls
  `Editor::run`, supplies the Qt ITextShaper, sets `pageRows` from the viewport height, paints the EditorUpdate rows
  and implements IOutlineProvider over the syntax outline (SyntaxOutlineProvider). An index slice may add a
  semantic one.
- **The frost::syntax consumers** (ADR 0023): the editor view (on `main`) applies each ChangeSet to SyntaxSession,
  calls `poll()` from the parse-ready hook, takes highlights for the visible range and uses `outline()` for next and
  previous member; `enclosingRanges` for extend selection is still unused. Sharing one SyntaxSession among the views
  of a document is W1's.
- **The editor view, later** (ADR 0037): the QRhi backend is on `main` (ADR 0043, the ninth batch); an on-screen
  validation of its viewport when on-screen runs are allowed, after the hidden-desktop self-test above. Editor-only
  zoom (Ctrl+wheel), and in the hardening phase the B5 measurement, B5/B6 and B1/B2/B15 with QRhi on the user's
  5120x1392 work area, and the token-contrast thresholds.
- **The frost::settings consumers** (ADR 0021): `m1/shell-nav` (on `main`) has the settings-file actions; they open
  the path that `prepareForEditing()` returns once AppServices holds a SettingsStore (until then a missing file is
  created as `{}`); the composition root binds the keymap, appearance and SaveManager readers to
  SettingsStore snapshots, feeds TrustStore decisions into `setProjectTrusted`, and starts `follow()` on watches of
  `watchDirectories()` or forwards the project root watch's batches to `applyChanges()`.
- **ADR 0029's moves:** large-file mode and B15's F1 gate move to M15 (until then M1's large-file guard refuses files
  of 128 MB or more; `budgets.json`), and EditorConfig (row 59) is M15's; the rest of the 2026-11-01 entry's Next
  stands with that re-tagging.

### Deferred to the hardening phase (ADR 0031)
Kept here until U1; the hardening phase works through it (ADR 0031, Consequences). Newest first; the items from
`m1/crash-diag` down are carried forward from the 2026-11-01 entry.
- **From the thirteenth batch's merge** (the integrator):
  - A review of the merge resolutions (the independent review that ADR 0031 moves to the hardening phase): the theme
    registry's key order and the two recomputed pins, `display_list.hpp`, `editor_view.hpp`'s API and signals, the
    click order in `editor_view_input.cpp`, the CMake lists and the label check's "git" rule.
  - A review of the follow-up 25d83a8 (the QRhi change stripes and their test).
  - A pixel comparison of the change stripes between the raster and the QRhi backends
    (`DrawsTheChangeStripesAsSolidQuads` checks the quads; `RendersTheDisplayListWhereTheRasterBackendDoes` has no
    stripes in its list).
- **From `m5/git-core`** (ADR 0048), as its hand-off lists them:
  - ASan, TSan and UBSan builds of frost::vcs and frost::ui_vcs. GitRunner's pipe threads and GitService's two
    serial queues and snapshot mutex are the TSan targets.
  - Fuzzing the porcelain v2 parser, `checkoutBlockers` and `gitFailure`'s stderr classification.
  - Stress and differential runs of the side-by-side diff against `git diff --histogram` beyond the six fixtures.
    ADR 0019 Q3 (which git release defines the newer histogram dialect) stays open (orchestrator decision), and the
    test's documented allowance (ADR 0019 D6) is unchanged.
  - Perf and budget gates: the process-start cost per git call on Windows, the 300 ms marker debounce and the refresh
    debounce (300 ms, at most 2 s late) are unmeasured.
  - Screenshot goldens for the Commit window, the DiffViewer, the gutter stripes, the branch widget and the git
    console; verify `-Full`; clang-tidy; the independent review the core rules call for.
  - Feature follow-up (orchestrator decision): an editable right (working-copy) side in the diff viewer.
  - Feature follow-ups recorded in ADR 0048: `frost-askpass` (credential prompts inside the IDE), commit signing, a
    persistent `cat-file --batch`, partial (hunk) commits and changelists, stash and shelve, LFS UI, running the
    change-marker diff off the GUI thread for very large files, and the File System view's colours (seam only). The
    QRhi drawing of the stripes, also listed there, was done at the merge (25d83a8).
  - Wiring the git widgets and actions into the running workbench (ADR 0048, Deferred; this entry's Next).
- **From the twelfth batch's merges** (the integrator):
  - A review of the merge resolutions (the independent review that ADR 0031 moves to the hardening phase):
    `editor_view.hpp`'s note and signals and `src/ui/editor/CMakeLists.txt`'s sources (the insight beside the QRhi
    backend); `src/build/CMakeLists.txt`'s sources, DEPS and TODO(M4), and the placement of
    `BuildDirectoryCompileCommandsProvider::locate()` in `compile_commands.cpp`; the `tests/ui` and `tests/build`
    CMake lists.
  - A review of the follow-ups 63aefbc (the two DisplayText fields in `editor_rhi_test.cpp`) and 96adc58 (the presets
    fixture's `CMakeUserPresets.json` and its `.gitignore` exception), and of the label check's new "cmake" and
    "msvc" rules (bcc3f32).
  - A pixel comparison of the insight parts between the raster and the QRhi backends once the QRhi backend draws them
    (`RhiGlyphPainter.RendersTheDisplayListWhereTheRasterBackendDoes` covers text runs and the gutter only).
- **From `m2/cmake`** (ADR 0046), as its hand-off lists them:
  - Sanitizer builds (ASan, TSan, UBSan) for the new project, build and ui_cmake code.
  - Fuzz targets for the presets reader, the File API reader and the GNU/CMake output parsers.
  - Stress, long differential and perf/budget runs (for example large File API replies, many reconfigures, watcher
    churn).
  - Screenshot goldens for CMakeToolWindow and the CMake tree; `-Full`.
  - Malicious-repository and trust-abuse tests (planted toolchain files, symlinked or junctioned build folders that
    leave the project, very large or deeply nested presets).
  - Real end-to-end runs with a Visual Studio generator and with cl.exe (only Ninja with clang-cl or clang is tested
    for real).
  - Icons in the CMake tree and polish of the tool window.
  - Wiring CMakeToolWindow, CMakeTreeModel and the CMake service into the W1 workbench (Solution Explorer,
    ToolWindowManager, Build menu and a BuildController adapter) on `main` (Next).
  - The cmake/msvc tag rules in the label check, which the slice left out to avoid a merge conflict: added in the
    merge bcc3f32 (the orchestrator's decision), so nothing of it is left.
  - Independent review of the build and project code (moved to the hardening phase by ADR 0031).
- **From `m3/editor-insight`** (ADR 0047), as its hand-off lists them:
  - Sanitizer builds (ASan, TSan, UBSan) for the new lang, editor and ui_lang code.
  - Fuzz targets for `decodeSemanticTokens`, `applySemanticTokensEdits`, `parseSemanticTokensResult` and
    `decodeInactiveRegions`.
  - Perf work and budgets: range mapping costs O(ranges) per edit plus O(ranges x edits) when tokens from an older
    version arrive; `diagnostics()` and `insightSummary()` are recomputed several times per paint (including the
    build/live duplicate check).
  - Screenshot goldens of squiggles, the error stripe, the inspection widget, the spinner and the balloon (checked
    only through the display list).
  - Contrast thresholds for the `Semantic.*` colours and the new marker and stripe colours (token colours are not
    policed yet, as with `Syntax.*`).
  - Handling `workspace/semanticTokens/refresh` (refreshSupport is announced as false).
  - A finer analysing state from clangd's fileStatus (today it is based on the diagnostics' text version).
  - Sharing tokens between two views of the same document (each view has its own binding).
  - An end-to-end test of `EditorInsightBinding::attach()` against a real ClangdService (covered separately by the
    lang e2e test and the scripted-server binding test).
  - Translation (`tr()`) of the new English strings: the balloon, tooltips and status text.
  - Independent review of the lang and editor changes (moved to the hardening phase by ADR 0031).
- **From the eleventh batch's merges** (the integrator):
  - A review of the merge resolutions (the independent review that ADR 0031 moves to the hardening phase): the folded
    frost::search (`src/search/CMakeLists.txt`'s sources, DEPS and rank 7; one `search_tests` in
    `tests/search/CMakeLists.txt`), `add_subdirectory(search)` moved inside the fuzz guard, and the CMake header notes.
  - A review of the follow-ups 3b0025f (the TinySolution counts after `w1/run-service`'s fixture change, and the
    eight count lines of `keymap-reachability.txt`) and 4ecb9ab (clang-format of the slice's ten files and the octal
    form of one test literal).
  - `FileWatcher.NewDirectoryAndItsFilesAreSeen` (io) is timing-sensitive under load in WSL (orchestrator decision on
    `m2/find-in-files`' first run, where it failed once; it passed alone three times and the full rerun was green):
    stress runs of it under load, then a wait that does not depend on timing.
- **From `m2/find-in-files`** (ADR 0045), as its hand-off lists them:
  - Sanitizers (ASan/TSan) and the `-Full` legs (ADR 0031).
  - Fuzzing `FileMask::parse` and `globMatch`.
  - Stress tests: editing documents while a search runs, rapid search restarts and cancels, many large files.
  - A perf gate for search throughput on the R2a/R2b corpora; budget numbers for search latency.
  - Screenshot goldens for the Find popup and the Find tool window.
  - Independent review of `SaveManager::rewriteClosedFile` and `replaceInFiles` (data safety), moved to the hardening
    phase.
  - TODO(M2): run Replace in Files for closed files on a background lane with progress (it runs on the GUI thread
    now).
  - TODO(M2): use `m1/shell-nav`'s gitignore-aware scan for the Directory scope.
  - W1 wiring: install FindFeature in MainWindow and connect `openLocation` (to the editor), `selectedText` (from the
    editor view) and `documentEdited` (to `main`'s `Editor::follow`).
  - Replace from the Find tool window, and tabs for several searches in it.
  - Persisting the last query, mask and scope through m1/settings; query and mask history drop-downs; Alt mnemonics
    for the Cc/W/.* toggles.
  - Plural forms in status and summary texts (they read "1 files").
  - A trigram index or fuzzy matching (TODO(M13)).
- **From the tenth batch's merges** (the integrator):
  - A review of the merge resolutions (the independent review that ADR 0031 moves to the hardening phase):
    `src/app/src/main.cpp` (the persistent services and the command line's project handling inside `main`'s
    diagnostics flow), `src/app/CMakeLists.txt`'s DEPS, `add_subdirectory(search)` outside the fuzz guard (inside it
    since the eleventh batch, whose review item covers that), and the matrix rows that keep `main`'s milestones.
  - A review of the long-line option follow-up (cfbd46e; one identifier in `tests/ui/workbench/workbench_test.cpp`).
- **From `m1/shell-nav`** (ADR 0042), as its hand-off lists them:
  - Feature-first (ADR 0031) hardening: independent review (this touches the core-adjacent io `canonicalPath` and
    `tree_scan`), sanitizers, fuzzing of `parseLocationQuery` and the session JSON reader, stress and mutation runs,
    `verify -Full`.
  - Perf and budget gates: Go to File and Find Action ranking latency on R2/R3-sized trees, the File System view
    model's rebuild cost on large trees, and the hot-exit scan's start-up cost after the first frame (B1).
  - Screenshot goldens for the popups, banners, merge view and File System view.
  - On-screen (non-offscreen) runs of mouse back/forward, focus-loss autosave and real window deactivation.
  - A real-disk end-to-end of watcher-driven external changes reaching the merge view (the tests use MemFs batches;
    the real watcher is only checked for start and stop).
  - Linux `canonicalPath` is lexical only (no symlink or case resolution); a realpath-based version and its test.
  - Watcher exclusions for `.git/` and build folders (they are watched now; batches under `.git/` are ignored for
    rescans).
  - Session writes on a timer or after changes (sessions are saved on project close and window teardown only, so a
    crash loses the session changes since then).
  - Untitled-buffer tabs after a hot-exit restore (only documents with paths are reopened).
  - Settings-file creation through `SettingsStore::prepareForEditing` (AppServices has no SettingsStore yet; a
    missing file is created as `{}` and migrated on load).
  - Translation catalogues for the new strings (they go through `qtr`/`tr` already).
- **From the ninth batch's merges** (the integrator):
  - A review of the merge resolutions: the CMake header notes and the guarded `add_subdirectory` calls in
    `src/ui/CMakeLists.txt` (ui_run on `TARGET frost::build`, ui_lang on `TARGET frost_lang`), and the two new
    functions side by side in `src/project/src/vcxproj_reader.cpp` (the independent review that ADR 0031 moves to the
    hardening phase).
  - A review of the keymap-golden regeneration in 29869f9 (the eight count lines of `keymap-reachability.txt`; its
    merge section above).
- **From `w2/clangd-nav`** (ADR 0041), as its hand-off lists them:
  - Sanitizers (ASan/TSan/UBSan), fuzzing (the snippet parser, the MSBuild evaluation JSON, the `-pp` import parser),
    stress, mutation and perf/budget gates, screenshot goldens and `verify -Full` for the new code.
  - Independent review of the frost::lang changes (moved to the hardening phase by ADR 0031).
  - Workbench wiring: IEditorLanguageHost over EditorView, IWorkspaceDocuments over the open documents, docking the
    find-usages panel, and passing `frost::build::findMsBuild` plus the live trust state into
    `solutionCompileCommandsChain` (`TODO(M2)`).
  - MSBuild evaluation of several projects in parallel (today one at a time).
  - Environment variables in the evaluation cache key (only the imported files' size and mtime are compared), and
    `Directory.Build.props` files created after the first evaluation.
  - clangd tweaks that need `workspace/executeCommand` and server-initiated `applyEdit` (only code actions that carry
    their own edit are offered).
  - Dedicated atlas icons for symbol and completion kinds (letter badges are used for now).
  - Highlighting the matched characters in the lookup rows; Rider's persistent toggle for highlight usages.
  - Full snippet template mode: tab stops are followed by shifting them with the document size change, which assumes
    edits happen inside the current placeholder.
- **From `m1/editor-rhi`** (ADR 0043), as its hand-off lists them:
  - Sanitizers, fuzzing, stress runs and independent review (ADR 0031 hardening phase).
  - B5/B6 measurements on screen, a PresentMon present-mode check, swap-chain frame latency, a quiet-desktop control.
  - B1/B2/B15 measurement of the lazy QRhi attach (which cost of the window-level RHI switch lands on B2/B15), and
    warming D3D11 on a worker thread.
  - On-screen validation of the QRhi viewport in a real shown window (the first QRhi viewport switches the top-level
    window to RHI composition; a possible one-time flicker is untested), and multi-monitor and mixed-DPI moves on real
    hardware.
  - Screenshot goldens for the QRhi backend.
  - An atlas eviction policy (a full 4096-texel atlas is cleared and refilled); glyph gamma and LCD subpixel AA.
  - Precompiled shaders (qsb); today the D3D11 backend compiles the HLSL at pipeline creation.
  - Re-evaluating auto when the session changes at run time (an RDP connect or disconnect while running), and adapter
    changes on multi-GPU systems.
  - A Linux QRhi leg (Vulkan/GL shaders; ADR 0007 decision 8); Linux uses raster and the pixel test skips on the Null
    backend.
  - A real device-lost recovery test on a GPU (only the detection path exists; the tests use the forced failure hook).
- **From `w1/run-service`** (ADR 0040), as its hand-off lists them:
  - Sanitizer builds (ASan, TSan, UBSan) of RunService, RunSession and RunController threading.
  - Fuzz targets for the `-getProperty` JSON parsing, `splitWindowsArguments`, macro expansion and
    `parseConfigurationProperties`.
  - Stress or long runs: many reruns or Stops in a row, Stop racing the program's exit, large or fast output into the
    Run console.
  - Perf or budget gates: the MSBuild evaluation (about 1 s) on the first run, RunConsole appending large output,
    reading the whole exe to find its PE subsystem.
  - Screenshot goldens of the RunConsole; `verify -Full`; the independent review of the io (`process_win`
    `newConsole`/`rawArguments`) and build changes.
  - A real launch with `newConsole=true` is never tested, on purpose, so no window opens; only its ProcessSpec and the
    cmd tail (with pipes) are tested.
  - Wiring into the app: the Run tool window, `MainToolbar::runConfigurations` and the Solution Explorer context menu
    are not wired into `src/app`, because `m1/shell-frame` and `w1/tool-windows` were not in the slice's base (both are
    on `main` now). `RunController::bindStartupCombo(QComboBox*)` and `registerRunActions` are ready for that merge.
  - One console tab per run (`TODO(M4)`); a PTY, colours and carriage-return overwriting in the Run console (the
    terminal is M5).
  - Run configurations beyond the startup project, CMake targets (M2/M3), the debugger (M4).
- **From the eighth batch's merges** (the integrator):
  - A review of the long-line follow-up (4a9954b), document code: the byte rule's character bounds and its line walk;
    and a measurement of that walk at load on a text near 128 MiB whose longest line has between a quarter of the cap
    and the cap in characters (B5 and the load path).
  - `StallHooks.ASlowSlotIsOneStallAndOneReport` (`tests/app`): `m1/crash-diag`'s known timing flake (its slice
    record), seen once more under the full parallel verify load in `w1/tool-windows`' own run, where it passed alone
    and on the re-run. It passed in this batch's run.
- **From `w1/tool-windows`** (ADR 0039), as its hand-off lists them:
  - Sanitizers (ASan/UBSan/TSan) over `ui_toolwindows_tests`, especially TSan on the BuildController relay.
  - Fuzzing, stress runs, mutation checks, perf/budget gates, screenshot goldens, `verify -Full`.
  - Independent review of the slice (feature-first phase, ADR 0031).
  - Capping the output pane's own line count (the controller's replay is capped at 200 000 lines, but the
    QPlainTextEdit is not).
  - Problems grouped by file (optional in scope).
  - Show All Files in the Solution Explorer (`TODO(M2)` in `solution_tree_model.hpp`).
  - Highlighting the matched characters during speed search.
  - A real `explorer.exe /select` and `xdg-open` launch check (only the spec and a scripted launch are tested).
  - On-screen interaction tests (focus-out ending speed search, the context-menu popup, the window-modal trust dialog
    over a real main window).
  - Tests with an installed FrostStyle checking the actual icons and tint colours (tests run without a style, so icons
    are null and tints use fallbacks).
- **From `m1/editor-view`** (ADR 0037), as its hand-off lists them:
  - Sanitizer builds (ASan/TSan/UBSan), fuzz targets and runs, stress or differential runs, mutation checks, `verify
    -Full`: not run, as the feature-first phase requires.
  - Perf and budget gates: measuring the InputToPaint marker against B5 (the hook is in place, nothing feeds ETW or the
    budget yet); measuring `kLongLineCapBytes` against B5; the glyph-cache size.
  - Editor screenshot goldens and pixel checks (tests read the display list only).
  - Contrast thresholds for the `Syntax.*` token colours (only `Editor.foreground` on `Editor.background` is in the
    contrast policy; `TODO(M1)` in `theme_registry.hpp`).
  - The QRhi glyph-atlas renderer behind IEditorRenderer (raster only at the time; `m1/editor-rhi` brought it in the
    ninth batch, and its own deferred items are under `m1/editor-rhi` above).
  - EditorSettingsBinding: reading `.editorconfig` off the GUI thread, and the race where a settings listener on
    another thread runs while the binding is being destroyed.
  - IME with several carets (only the primary caret composes), and the IME replacementStart/Length attributes.
  - Paste with several carets does not split the clipboard text per caret; drag auto-scroll has no timer (it only
    scrolls when the mouse moves).
  - Caret column against glyphs for ZWJ emoji sequences and fallback fonts whose glyphs are wider than the shaper's
    cells.
  - `tr()` wrapping of the find bar, Go to Line and action names (`TODO(M1)`: it waited for `m1/shell-frame`'s
    `tr.hpp`, which is on `main` now).
  - Several views sharing one SyntaxSession per document (the W1 integration slice).
  - Independent review of the text/editor model changes (moved to the hardening phase by ADR 0031).
- **From the seventh batch's merges** (the integrator):
  - Find why the Windows Debug tree held a bad object in this batch (its Verification): 47 tests on
    ScriptedProcessLauncher crashed with 0xc0000005 until the objects compiled since the first merge were rebuilt with
    `SCCACHE_RECACHE=1`, with no source change. Suspects: the sccache cache shared by every worktree and agent
    (`E:\sdk\sccache-cache`), or a stale incremental object.
  - A review of the merge resolutions: `src/app/src/main.cpp` (`main`'s diagnostics flow around the shell's start-up),
    `src/io/src/process_posix.cpp` (the slice's helpers on `main`'s `detail::openPidfd`; built and run only in the WSL
    leg) and `frost_add_test`'s tag handling (8ee7a1d).
  - A configure-time test that `frost_add_test` refuses a first label that is not a kind and a malformed tag
    (`tools.verify_support` checks only the labels a good tree ends up with).
  - PLAN.md section 3.2's layout for `src/document` (it still lists SaveManager under `src/text`; ADR 0028 decision
    1).
- **From `m1/save-manager`** (ADR 0028), as its hand-off lists them:
  - Sanitizer builds (ASan/UBSan/TSan) and `verify -Full` for frost::document and the new io code.
  - Fuzzing the journal-label parser (`decodeJournalLabels`) and `parseAtomicTempName`.
  - Full fault injection beyond MemFs Write/AtomicRename faults and the single kill -9 test (crash at every step of
    ReplaceFileW / rename / in-place rewrite, SQLite VFS faults during journal puts).
  - Stress and long differential runs (many documents, watcher storms against SaveManager, repeated kill loops).
  - A backup copy before the in-place rewrite of hard-linked files (a crash mid-rewrite can tear the file; the journal
    still holds the buffer).
  - Evaluating REPLACEFILE_IGNORE_ACL_ERRORS for ReplaceFileW (currently flags 0, strict ACL merge).
  - Moving the encode, write and journal puts of `save()` to a background lane (the `TODO(M1)` in `save_manager.hpp`);
    saves currently block the owner thread.
  - Case-insensitive document-key normaliser for Windows paths (storage's `TODO(M2)`).
  - Saving through a dangling symlink (currently fails with NotFound instead of creating the target).
  - Perf and budget gates for save, journal and restore (B20/B21) and large-file saves.
  - Independent review of the io/storage-touching code (moved to hardening by ADR 0031).
  - UI wiring: merge view, frame-deactivation and idle hooks in the shell, out-of-date and read-only banners.
- **From `w2/clangd-diagnostics`** (ADR 0036), as its hand-off lists them:
  - Fuzzing FrameDecoder and `parseJson` (and the publishDiagnostics decoder).
  - Sanitizer runs (ASan/UBSan/TSan) of the JSON-RPC connection, Deadline and the ClangdService supervisor.
  - Stress tests: restart loops, crash during initialize or shutdown, cancellation storms, many open documents, large
    didChange batches.
  - Perf gates: keystroke-to-didChange latency, publishDiagnostics mapping cost, B24/B26 clangd memory and index
    budgets.
  - ResourceGovernor-driven clangd `-j` sizing by free commit, and a restart after the first full index (ADR 0008).
  - Delaying or dropping Composition-origin ChangeSets (IME preedit) before didChange.
  - Synthesized flags beyond the ClCompile ItemDefinitionGroup: per-file item metadata, imported `.props` sheets,
    AdditionalOptions, PCH (PrecompiledHeader/ForcedInclude of the pch), TreatWChar_tAsBuiltInType, non-configuration
    Conditions (all need MSBuild evaluation, M4).
  - Wiring GeneratorCompileCommandsProvider to a user configuration (`TODO(M4)`).
  - Re-anchoring stale diagnostics to newer text versions in the editor; the Problems window and editor squiggles
    (UI).
  - Independent review of `src/lang` and the frost::project reader change (moved to hardening by ADR 0031).
  - clang-tidy on the new files (verify reports tidy not-run).
  - Goldens for the synthesized `compile_commands.json`.
  - Settings-driven clangd options (extra args, log level) and a workspace/configuration schema.
- **From `m1/shell-frame`** (ADR 0038), as its hand-off lists them:
  - Sanitizer builds (ASan/TSan) of `ui_shell_tests`, including TSan over BackgroundTask and
    `NotificationCenter::post`.
  - Fuzzing, stress and mutation runs; no review agents (ADR 0031).
  - Perf and budget gates: B1 menu/shortcut latency and shell start-up time on screen.
  - Screenshot goldens of the shell, and pseudo-localization goldens.
  - `verify -Full`.
  - On-screen tests with real native key delivery (all shell tests send synthetic QKeyEvents offscreen).
  - Telling apart tabs of files that share a name (path suffix in the tab title).
  - Status-bar encoding and line-separator popups (Reload in / Convert to, ADR 0018); the widgets show values only.
  - Frameless title bar, Windows snap layouts and a merged menu/toolbar row like Rider's.
  - Drag docking, floating and undocked tool windows, split editors, more than one window per side.
  - m1/settings adapters for IShellSettings and IKeymapSettings (in-memory defaults for now).
  - Real translation catalogues (only the ITranslator seam and pseudo-localization exist).
  - Find Action popup: `m1/shell-nav`'s job per `find_action.hpp`; the popup framework it should reuse is in place.
  - Keyboard focus cycling through the toolbar and status bar beyond focusing their first widget.
  - Find Action usage counts and the Keymap settings page.
- **From `w1/build-service`** (ADR 0035), as its hand-off lists them:
  - Sanitizers (ASan/UBSan/TSan), fuzzing, stress, mutation, perf gates, screenshot goldens and verify -Full were not
    run (feature-first phase, ADR 0031).
  - A real MSBuild test where a post-build step starts a detached process and that process outlives the build. Only
    covered by the scripted launcher and by the real `io_test_child` container tests.
  - A real MSBuild test of the cancel-versus-exit race (covered by the scripted launcher only).
  - Progress reports finished projects only; 'project started' events and exact per-project progress wait for
    FrostIDE.MSBuildLogger (`TODO(M4)`).
  - Document-path matching ignores ASCII case only on Windows; non-ASCII case folding is not done.
  - Concurrency hardening of BuildService (cancel during the window where MSBuild is being launched, destruction while
    a Save-All completion is in flight, many quick build/cancel cycles) has no tests yet.
  - Linux leg: `containedProcesses` depends on `/proc`; other POSIX systems return Unsupported (macOS stays
    unsupported).
  - The output grace (1 s) and the infrastructure image list are fixed; no setting for them yet.
- **From the sixth batch's merges** (the integrator):
  - Exclude `third_party/` (the vendored tree-sitter and grammar sources) from the clang-tidy step, as the format step
    does (`_EXCLUDED` in `tools/verify_support.py`), before `-Tidy` goes back on.
  - A review of the merge resolutions: the merged `tools/verify.ps1` and `tools/verify.sh` (the symbolize step inside
    the restructured flow), the `ProcessWatch` constructors in `tests/io/process_env.hpp`, and the label cuts (the
    labels came back as tags in the seventh batch, 8ee7a1d).
- **From `plan/thin-path`** (ADR 0029), as its hand-off lists them:
  - No independent review of the plan changes, and no `verify -Full` run.
  - The U1 scenario's debug steps were checked against the corpus source files on disk (`not.cpp`, `winmain.cpp`)
    but never run; spike S10 in M4 checks lldb-dap on cl.exe-built Notepad++.
  - Other ADRs from `main` still use the old slice name `m1/large-file` in ordinary text (ADR 0018's section headings,
    the `encoding_detection.hpp` and `text_file.hpp` comments); only the to-do markers were retagged.
- **From `m1/settings`** (ADR 0021), as its hand-off lists them:
  - Fuzzing `SettingsDocument::parse` and the span scanner, and the EditorConfig parser and glob matcher.
  - Sanitizer runs (ASan, UBSan, TSan) of `settings_unit_tests`.
  - A stress test for concurrent reloads and writes: two reloads of one file can publish out of order, and two
    concurrent `set()` calls on one file can lose an update (`editFile` reads, edits and writes without a per-file
    lock).
  - A bound on backtracking in the EditorConfig glob matcher (nested `*` and `**` can be slow on adversarial patterns;
    only the recursion depth is capped, at 64).
  - Case-insensitive path comparison on Windows in `scopeOf()` and the watcher path rules (paths are compared after
    `normalizePath` only).
  - An integration test against the real native file watcher (only MemFsWatcher is covered).
  - Removing the only member of an object drops any comments inside it.
  - Performance or budget checks for snapshot rebuilds (republishing is O(keys x 3) per change).
  - The Settings dialog UI (M15) and wiring the three actions into the shell (`m1/shell-nav`).
  - Composition-root adapters that feed `keymap.*`, `ui.*` and `save.*`/`localHistory.*` into frost::input, the ui
    model and SaveManager (settings may not depend on them).
  - Importers from VS Code, Rider and Visual Studio settings (the `TODO(M11)` in `src/settings/CMakeLists.txt`).
- **From `m1/diff-merge`** (ADR 0019), as its hand-off lists them:
  - Recheck issue 2: exact new-dialect git pins (pin the measured git-fewer cases instead of bounding at twice the
    measured rate). The half-done WIP pins were reverted to b4e652f's state and `tests/diff/git_new_dialect_pins.hpp`
    removed.
  - Perf re-measurement after the partner path and histogram reuse: `DiffPerf.WorkCapSweep`, `WorkUnitCost`,
    `Pathological` (P1-P7), `CapScale` at full scale, the EvenlySpaced timings and `PeakMemory` (the run cache adds
    12 B per B line and exact counts 3 B per id). Not re-timed in the feature-first phase; ADR 0019 says so.
  - Sanitizer (ASan, UBSan, TSan) runs, fuzz runs beyond the corpus replay, `diff_stress_tests`, long git
    differentials, mutation checks and `verify -Full`.
  - Tail-drop narrowing for regions that are a prefix of the indexed one (the reverse-peel pattern still rebuilds the
    index).
  - The O(D^2) walk that remains for very dense evenly spaced edits (every other line still degrades).
  - Finer degradation when the cap strikes while interning (currently one whole-file hunk or conflict).
  - Release-build timing of the `DefaultCap.*` inputs against B21's 50 ms (work units are build-independent; times
    were not taken).
  - Q1 and Q3 of ADR 0019 (a pinned classic-dialect git oracle in `E:\sdk`; which git defines M5's "matches git diff
    --histogram"), deferred to M5 and the hardening phase by the orchestrator.
- **From `m1/editor-model`** (ADR 0025), as its hand-off lists them:
  - Sanitizer (ASan, UBSan, TSan) runs, fuzz targets and stress or differential tests for the commands and
    SearchPattern.
  - A QTextBoundaryFinder cross-check of the grapheme boundaries and Unicode's GraphemeBreakTest.txt conformance run
    (the `TODO(M1)` in `unicode.hpp`).
  - B5 timing and perf gates for the commands, search and replace all on large files; no `-Full` run, goldens or
    clang-tidy.
  - Regex search on long lines and multi-line patterns without copying: Lines mode copies each line, Whole mode
    copies the whole text on every call.
  - Home and End by display row under soft wrap (they go by buffer line now).
  - Multi-caret and column-mode creation commands (M15, out of the slice's scope).
  - Smart keys beyond the basic table: language-aware auto-indent, outdenting a closing brace while typing, and
    remembering which closers were auto-inserted, for typing over them.
  - Feature-matrix rows 39, 44, 45, 85, 94 and I6 still have no acceptance test or status update from this slice.
- **From `m1/syntax`** (ADR 0023), as its hand-off lists them:
  - Sanitizer builds (ASan, UBSan, TSan), fuzzing of the TSInput and edit mapping and of the queries, stress and
    differential runs, perf and budget gates (B5: the 2 ms incremental target is not measured), goldens and `-Full`.
  - A counting allocator with a syntax memory cap (`ts_set_allocator`) and a `syntaxMemory` stat.
  - Releasing old trees off the owner thread (the `TODO(M1)` in `tree_state.hpp`).
  - Narrowing the `-Weverything` suppression around the tree-sitter runtime to the warnings it actually emits.
  - Compile-time and binary-size measurements of the grammars in ADR 0023.
  - Fetching and pinning `.tar.gz` vendor archives in `asset_fetch.py` (vendor locks are `--verify` only; its
    `TODO(M1)`).
  - Fold ranges and the Structure-view model (M15, ADR 0029).
  - The Markdown inline grammar and language injection for fenced code (the `TODO(M4)` in `markdown/highlights.scm`).
  - Rows for the feature matrix, if syntax needs its own.
- **From `m1/verify-tooling`** (ADR 0027), as its hand-off lists them:
  - clang-tidy in verify on by default (`-Tidy` is fully wired but off); after the merge, a `tidy --all` run on both
    OSes. The TSan quick leg on by default (`-TsanQuick` / `--tsan-quick` is wired but off); its first run in a fresh
    mirror builds the whole TSan tree. Both go back on when the hardening phase starts (orchestrator decision).
  - The root cause of the 0xc0000142 start failures: not reproduced. The hidden desktop's heap is the normal 20480 KB;
    running out of system commit memory is a candidate (chrome.exe failed with 0xC000012D and 0xC0000142 on
    2026-10-31). Failures can no longer raise boxes, but a recurrence would still fail tests.
  - A soak of parallel ctest at high `-j` (8 and 24) on the merged `main`; only `-j 4` (a probe) and `-j 6` were run
    in the slice. (This batch's `-Quick` ran `-j 8` once; Verification.)
  - Parallel ctest in `verify.sh` (the WSL leg stays serial).
  - Narrowing or lifting the `frost_desktop` lock: the 124 tests behind it were the critical path at `-j 6` (22.6 s to
    52.9 s).
  - Schedule tuning: FrostRuntimeDlls takes `PROCESSORS 4` of 6 slots for about 20 s, which delays the start of the
    locked chain; the perf suites also run alone in Debug, where nothing is asserted.
  - Speeding up or splitting `tools.frost_perf` (21-40 s, the slowest single test) and `tools.verify_support`
    (12-25 s); they add about 35-60 s of serial time to the WSL leg.
  - B28's "after" measurement with `perf.ps1` inside a `night.py` measure lock, and the `budgets.json` B28 method text
    for `-Base HEAD` (needs the user's approval; not changed in this phase).
  - Sanitizer builds, fuzz runs, stress runs and `verify -Full` were not run.
- **From `m1/crash-diag`** (ADR 0022), as its hand-off lists them:
  - `verify -Full` with its Release, ASan and fuzz configurations and the new symbolize step, which has never run
    in a gate.
  - Sanitizer builds of frost::diag.
  - `diag_stress_tests` (the sampler under heap and loader churn, the watchdog's park and wake rounds).
  - Fuzz targets for the freeze-report writer, the minidump reader and `tools/symbolize.py`'s parsers.
  - Property tests of `planPrune` at scale.
  - ADR 0022's measurements: the handler start p50, B1 before and after (with `night.py measure`), the self-tests'
    wall times in Release, and B19 memory.
  - An independent review of the security-relevant code: the pipe DACL patch, the pruner's link safety, fatal-path
    re-entrancy and `ProcessSpec::posixInheritedFds`. With it, a review of the integrator's two fixes at the merge:
    4d42a50 (`LinkingFs`) and 79512d6 (`frost_app.dll` may delay-load ws2_32.dll and nothing else).
  - Adopt `m1/storage`'s cache-root resolver in `frost/diag/paths.hpp` (its `TODO(M1)`); `m1/storage` is on
    `main`.
  - Resolve the io `TODO(M1)`s that log the child's executable, working directory and arguments, now that
    frost::log exists.
  - Add the crash handler's pipe to the M2 listener-enumeration test (row I9) and to PLAN.md 5.2.
  - WER module registration or `WerAddExcludedApplication` for `__fastfail` crashes (permanent HKCU writes; not
    done).
- **From `w1/build-msbuild`** (ADR 0034), as its hand-off lists them:
  - Sanitizer runs of `build_unit_tests` (BuildSession's threads and cancel races).
  - Fuzzing `parseDiagnosticLine` and the JSON reader.
  - Large-output and stress tests: hundreds of MB of output, many builds, start and cancel loops.
  - Measure the per-build PDB server cost and the ending of stragglers on large solutions (ADR 0034's
    `TODO(M4)`), and confirm that `_MSPDBSRV_ENDPOINT_` isolates a build from a concurrent Visual Studio build.
  - GNU-format diagnostics for a future CMake driver; localized category words (I12, M4).
  - A test that no process (mspdbsrv included) outlives a normal, uncancelled build.
  - Discovery through the VS Setup COM API with caching; an x86 and arm64 MSBuild fallback.
  - A real end-to-end build with the ClangCL toolset.
  - Async pipes on the frost::io reactor instead of two blocking reader threads per build (the `TODO(M1)` in
    `build_session.hpp`).
- **From `w1/project-sln`** (ADR 0033), as its hand-off lists them:
  - Fuzz targets for the `.sln`, `.slnx`, `.vcxproj` and `.filters` readers (they need a fuzz tree that installs
    vcpkg ports, as ADR 0014 notes for storage).
  - Sanitizer runs of `project_unit_tests`.
  - Perf of `SolutionModel::load` against B23 on large solutions (R2a/R2b), and of wildcard walks.
  - Stress: deep or huge wildcard trees (`globMatch` is exponential in the number of `**`) and very large `.sln`
    files.
  - A size cap on project and solution files before parsing.
  - Non-ASCII case folding on Windows for path keys, lookups and trust.
  - The trust store: resolve symlinks and junctions before comparing; multi-instance safety; move it into M2's
    application settings.
  - The MSBuild semantics the static reader skips: Remove/Update, environment-variable properties, property
    functions, conditions, imported `.props`/`.targets` (M2 path (c)).
  - Windows-absolute paths in project files read on Linux; non-UTF-8 ANSI `.sln` files.
  - An independent review of the trust code (a security area).
- **From `m1/display-map`** (ADR 0026), as its hand-off lists them:
  - The display-map property test against a brute-force oracle (an M1 exit criterion) and long differential runs.
  - Fuzzing and sanitizer runs over DisplayMap.
  - B5 with soft wrap on, and memory per wrapped row (no measurements yet).
  - Background or lazy wrapping of very large files and long lines.
  - UAX #14 line breaking and UAX #29 grapheme clusters; UCD-generated width tables (the `TODO(M1)` in
    `text_shaper.hpp`).
  - An O(1)-per-row DisplayRowCursor.
  - A hanging-indent option for continuation rows.
  - Mutation and stress tests of the block splice.
- **From `m1/keymap-de`** (ADR 0030), as its hand-off lists them:
  - The xkb layout oracle (test-only, approved in ADR 0030; the `TODO(M1)` in its tests).
  - The Linux active-layout reader through libxkbcommon (it needs a named system-library entry in
    `tools/licensecheck.py`).
  - Rule 3.6 on real de-CH, es and Nordic layout tables (M11).
  - Sanitizer, fuzz and stress runs (`input_stress_tests` with the new rule), Release and `-Full` runs.
  - An independent review of the layout-rule change.
  - The real native key path on the real desktop (`m1/automation-bench`).
- **From `m1/encodings`** (ADR 0018), as its hand-off lists them:
  - Mutation checks of `decodeBlocks` (review issue 1, test strength).
  - Measure the transient load peak (review issue 2: the payload-sized reservation that the shrink hides, which no
    unit test observes) and add a B15/B26 gate for it.
  - Sanitizer runs: Windows ASan, WSL ASan+UBSan and WSL TSan (`EncodingConcurrency`, and the gconv mutex
    workaround in `codec_posix.cpp`).
  - Fuzz runs: 60 s `text_decode` smokes on both OSes with check (d), and a corpus merge. Only its corpus replay
    (`fuzz_replay.text_decode`) ran; no fuzz tree was built for this merge.
  - The stress label (`text_encoding_stress_tests`: the full-BMP differential, `LosslessProperty` at 2000 and 20000
    seeds).
  - The Release perf gates (`text_encoding_perf_tests`: D15's 1.5x UTF-8 load ratio, the detection bounds,
    throughput) and `verify -Full`.
  - Re-measure the committed memory of loaded 32 MiB UTF-16LE and windows-1252 documents against ADR 0018's figures.
  - A re-review of ADR 0018's "Review follow-up" section and its comment wording.
  - Decide whether to file an upstream TSan report about gconv module loads (LLVM 23.1.4, glibc 2.39).
- **From `m1/io-reactor-watch`** (ADR 0020), as its hand-off lists them:
  - Sanitizer runs (ASan+UBSan, TSan) of the io unit, integration and stress tests. TSan matters most for the
    reactor's shutdown, the WatchQueue awaiters, the inotify wd tree and async pipe cancellation.
  - The `io_stress_tests` label (50k-file watcher stress, rename storms, overflow loops, 64 MiB to 1 GiB pipe
    streams, 200k coalescer seeds) is written but has not run in WSL.
  - The `io_perf_tests` reports (change-to-batch latency, shutdown with 10 watches, inotify set-up per 10k
    directories, polling per 10k files) and the start-up cost of asio's static initialisers; ADR 0020's WSL
    measurement column.
  - A fuzz run of `io_ignore_rules` (only its corpus replay ran). No fuzz tree was built for this merge.
  - An independent review of the reactor, the watch queue and the async pipes, and of the comment-only
    `scheduler.hpp` edit with its ADR 0002 amendment.
  - Windows cross-directory moves arrive as Removed plus Created (FileId pairing and the USN journal are M13).
  - Windows polling calls `status()` per file; switch the snapshot to listings with attributes.
  - Who owns ThreadPool's timers on the reactor and Windows high-resolution waits (the `TODO(M1)` in
    `thread_pool.hpp`, ADR 0002; sched needs a timer seam).
  - Who owns helper-process priorities (BELOW_NORMAL plus EcoQoS, SCHED_IDLE) in the io launcher.
- **From `m1/storage`** (ADR 0014, low): `openStore` and `Database::open` do not yet re-check the path after
  locking, as `HotExitJournal::create` does, so the residual POSIX removal race stays open (`store.hpp` and ADR 0014
  decision 8 state it, 39837c1).
- **From `m1/storage`'s verification:** perf-smoke and coverage were not run; `tools/perf.ps1 -Smoke` and a coverage
  leg do not exist yet (ADR 0014, Context, "Tooling gaps"). The journal fuzz target waits for fuzz trees that
  install vcpkg ports (ADR 0014 question 4, under Questions for the user).
- **`-Full` of `main`** (Release, ASan, TSan and the fuzz legs). The M1 merges since the `m0` sign-off ran
  `-Quick` only; each slice's own sanitizer results stand (the slice records).
- **The theme and icon-atlas fuzz targets** (moved from Next; ADR 0015, Consequences: the `TODO(M1)` "after
  m1/piece-tree", which is on `main` now): libFuzzer adapters for `tests/ui/fuzz/theme_blob_harness.cpp` and
  `icon_atlas_harness.cpp`, registered with `frost_add_fuzz_target` over `tests/fuzz/corpus/theme_blob` and
  `icon_atlas`. `frost::ui_model` is Qt-free but lives under `src/ui`, which fuzz trees skip (`FROST_FUZZ` needs
  `FROST_BUILD_UI=OFF`), so the registration has to build ui_model there; the `theme_json` harness needs Qt Core
  until the theme compiler moves to m1/settings' JSON library. It needs a fuzz-tree build and smoke on both OSes,
  which `-Quick` does not cover. Until then `ui_model_tests`' `fuzz_replay_test.cpp` replays the corpora in every
  normal tree, as on the branch.
- **The `input_keymap` fuzz target** (moved from Next; ADR 0016, Consequences; the `TODO(M1)`s named above):
  register it with `frost_add_fuzz_target` over `runKeymapFuzzInput` and `tests/fuzz/corpus/input_keymap`, build
  and smoke it in `win-clangcl-fuzz` and `linux-clang-fuzz`, and only then delete `KeymapFuzzReplay`. That test
  also runs seeded mutations of the corpus, so its removal must not lose them (keep them as a unit test, or show
  the fuzz legs cover them); nothing is weakened.
- **Re-measure B28** with ADR 0010's harness (moved from Next; the slice record "M1 first slice", "B28 effect").

### Blockers
- None. The hand-off notes of the thirteenth batch (`m5/git-core`), of the twelfth (`m3/editor-insight`,
  `m2/cmake`), of the eleventh
  (`m2/find-in-files`), of the tenth (`m1/shell-nav`), of the ninth (`w1/run-service`, `m1/editor-rhi`,
  `w2/clangd-nav`), of the eighth (`m1/editor-view`, `w1/tool-windows`), of the seventh
  (`w1/build-service`, `m1/shell-frame`, `w2/clangd-diagnostics`, `m1/save-manager`) and of the sixth
  (`m1/verify-tooling`, `m1/syntax`, `m1/editor-model`, `m1/settings`, `plan/thin-path`) name none; `m1/diff-merge`'s
  LongPathsEnabled item is not a blocker (its merge section above).
- B1 warm (about 195 ms on screen against p50 150 ms) stays M1 work (ADR 0011, A3; entry "M0 complete").

### Questions for the user
- **Build Solution's shortcut** (`w1/tool-windows`, ADR 0039): Ctrl+Shift+B (Visual Studio) or Ctrl+F9 (the IntelliJ
  keymap's Build)? Until you decide, Build Solution is Ctrl+Shift+B, the single `kBuildShortcutDefaults` entry
  (orchestrator decision). The shadowing that comes with it: Ctrl+Shift+B is also the IntelliJ keymap's Go to Type
  Declaration (`navigate.typeDeclaration`), which Build Solution shadows while a solution is open; while a build runs,
  or with no solution open, the build action is disabled and Ctrl+Shift+B falls through to Go to Type Declaration
  (Visual Studio would report "build already in progress" instead; the slice record's known bug).

Apart from that, no slice of the thirteenth, the twelfth, the eleventh, the tenth, the ninth, the eighth, the seventh or
the sixth batch asks one in its hand-off (`m5/git-core`, `m3/editor-insight`, `m2/cmake`, `m2/find-in-files` and
`m1/shell-nav` list no question); the orchestrator's decisions on their questions are in the merge sections above
and, for `m5/git-core`, `m2/find-in-files` and `m2/cmake`, in their slice records.
ADR 0019 Q5 (diff3-style markers and the save-manager prompt's labels), which the last integrator listed here, is
decided: diff3 stays the default, and the labels are "Keep my version", "Take disk version" and "Compare..." (the merge
section of `m1/save-manager` above).

The open questions in the 2026-11-01 entry's Questions for the user still stand: `w1/project-sln`'s (environment
variables and imported `.props` in the SDK's projects), `m1/keymap-de`'s two, `plan/w1`'s command-list file,
`m1/storage`'s six, `m1/theme-icons`' nine and `m1/undo-anchors`' four.

### Slice record: `m5/git-core` (ADR 0048)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): f98193c on d34334c. Worktree `.claude\worktrees\m5-git-core`. (Merge note: merged
into `main` as d67f420, with the follow-up 25d83a8; the 2026-11-02 entry above.)

#### Done
- **The slice (f98193c):** frost::vcs over the git CLI and frost::ui_vcs (the Commit window, the side-by-side diff
  viewer, the editor's change markers, the branch widget, the git console and the `vcs.*` actions), the editor's
  DisplayChangeStripe, the colour seams in the Solution Explorer and the editor tabs, ContextKeys `CommitViewFocus`
  and `DiffViewFocus` with Ctrl+D, F7 and Shift+F7 under them, 14 theme keys, the label check's "git" rule, ADR
  0048 (`docs/adr/0048-git-core.md`) and tests.
- **The theme pins** changed on purpose on its older base: `kExpectedRegistryHash` 0x01f21b52f86152e7 and the theme
  blob 660/732; recomputed at the merge for the union with `m3/editor-insight`'s keys (the merge section above).
- **Verification at f98193c** (its own `-Quick`), on both legs: win debug 2169/2175 (6 skipped), wsl debug 2066/2072
  (6 skipped); the WSL leg needed no code fixes. The last session's only edit was a comment correction in
  `display_list.hpp`. Every process it started was ended; nothing points into the worktree any more, and no WSL
  process uses its mirror (`~/frost-src-d145f9b7c9e6`). Not merged or pushed by the slice; `docs/progress.md` and
  PLAN.md untouched.
- **Orchestrator decisions on its questions:** ADR 0019 Q3 stays deferred; F7/Shift+F7 under `DiffViewFocus` are
  accepted (as in IntelliJ); the diff viewer's right side stays read-only (a later M5 follow-up).

#### Known bugs
- None found. Every suite passes on both OSes. Diff fixtures run against the newer histogram dialect (Windows git
  2.56) still rely on the ADR 0019 D6 allowance (git may report fewer changed lines, at most once). That is
  documented, not a regression.

#### Left
- Its hand-off's Next (this entry's Next): wire the git UI into the workbench, then the M5 diff follow-ups (an
  editable right side). The re-check of the keymap goldens and the theme pins on the merged tree that it asked for
  was done at the merge.
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m5/git-core`'s.

### Slice record: `m2/cmake` (ADR 0046)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): ce467e4 on 52eb3d6. Worktree `.claude\worktrees\m2-cmake`. (Merge note: merged into
`main` as bcc3f32, with the follow-up 96adc58; the 2026-11-02 entry above.)

#### Done
- **Qt-free CMake support** in frost::project (presets, profiles, the File API model and the view model) and
  frost::build (tool discovery, the vcvarsall capture and its cache, CMakeService, the GNU and CMake output parsers,
  and a parser factory in BuildSession).
- **The settings** `cmake.profiles` and `tools.ninja.path`; the clangd provider that follows the active build
  directory (BuildDirectoryCompileCommandsProvider).
- **frost::ui_cmake** (the tree model, the controller and the tool window).
- **ADR 0046** (`docs/adr/0046-cmake-projects.md`).
- **Verification at ce467e4** (its own `-Quick`), on both OSes: win 2093/2099, wsl 1989/1995. Not merged or pushed by
  the slice; `docs/progress.md` and PLAN.md untouched.

#### Known bugs
- CMakeService prepares the tools and the MSVC environment once per service and reuses them. If a cmake or ninja
  binary is replaced while the service runs, that is only noticed when the version check or the launch fails.
- The ui_cmake widgets are not hosted by the workbench yet (the slice's tree had no `src/ui/toolwindows`), so the tool
  window is reachable only from tests until W1 wires it in.
- Found at the merge (fixed by 96adc58): the presets fixture's `CMakeUserPresets.json` was not committed, because the
  repository's `.gitignore` ignores that name; it lived untracked in the slice's worktree.

#### Left
- Its hand-off's Next (this entry's Next): wire CMakeToolWindow and CMakeTreeModel into the W1 Solution Explorer and
  ToolWindowManager, and add a BuildController adapter for CMakeService. The cmake/msvc tag rules of the label check
  were added at this merge (the orchestrator's decision).
- The merge conflicts it expected (`src/ui` and `tests/ui` CMake lists, the settings registry and keys, maybe
  `build_session.hpp`): the CMake lists and `compile_commands.cpp` conflicted, the settings files and
  `build_session.hpp` auto-merged (the merge section above).
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m2/cmake`'s.

### Slice record: `m3/editor-insight` (ADR 0047)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): eb033ab on 52eb3d6 with its merges 7d2fab1 of `m1/editor-view` 3acd71c and d969d15 of
`w2/clangd-nav` 660a1e9, and a1fd262, a copy of `main`'s label-check fix 76d5e7f. Worktree
`.claude\worktrees\m3-editor-insight`. (Merge note: merged into `main` as a2626b4, with the follow-up 63aefbc; the
2026-11-02 entry above.)

#### Done
- **The slice (eb033ab):** semantic tokens with full and delta requests, clangd's inactiveRegions, EditorView's
  insight seams, squiggles, the error stripe, the inspection widget, the status-bar indicator, F2/Shift+F2 with the
  balloon, EditorInsightBinding, 28 theme colours, ADR 0047 (`docs/adr/0047-editor-insight.md`) and tests.
- **The theme pins** change on purpose: `kExpectedRegistryHash` 0x31cfb245a8dcffe0 and the theme blob 716/788 for the
  28 new `Semantic.*` and marker keys.
- **Verification at eb033ab** (its own `-Quick`), on both legs: win 2148/2154 (6 skipped), wsl 2041/2049 (8 skipped).
  Not merged or pushed by the slice; `docs/progress.md` and PLAN.md untouched.

#### Known bugs
- The binding's token requests and the analysing state assume the workbench sends each edit to clangd
  (`changeDocument`) before the 300 ms debounce fires. If it doesn't, tokens can describe older text and analysing
  never clears.
- Inactive regions are mapped against the snapshot of the last published diagnostics. This assumes clangd sends
  diagnostics before inactiveRegions for each build (its usual order); if the order were reversed after an edit, the
  regions would be one version behind until the next build.
- The F2 balloon and the inspection widget size their text as character count times the monospace cell width.
  Proportional or wide (CJK) text in a message can overflow the balloon.
- The inspection widget sits on top of the text in the top-right corner (as in Rider) and can cover the end of a long
  first line. The stripe always takes 11 px of width, even when no language server is attached.
- Build diagnostics are placed when they are set; after later edits they follow the text, but a rebuild is needed to
  fix their positions.

#### Left
- Its hand-off's Next (this entry's Next): the workbench creates one EditorInsightBinding per editor and wires it to
  ClangdService, BuildService, the status bar and the LanguageController host; W1 can move its own marks onto
  `setDecorations`; `m1/editor-rhi`'s GPU backend has to draw the new display-list parts (decorations, the stripe,
  chrome and chromeText).
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m3/editor-insight`'s.

### Slice record: `m2/find-in-files` (ADR 0045)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): 2ce9819 on 52eb3d6. Worktree `.claude\worktrees\m2-find-in-files`. (Merge note:
merged into `main` as aca0ea8, with the follow-ups 3b0025f and 4ecb9ab; the 2026-11-02 entry above.)

#### Done
- **frost::search's Find in Files engine:** masks, scopes, encodings, open documents, cancelling and the 10,000-match
  limit; Replace in Files with `SaveManager::rewriteClosedFile`.
- **frost::ui_find:** the popup, the Find tool window and the actions; the two new keymap entries with regenerated
  goldens.
- **ADR 0045**; `search_tests` (17) and `ui_find_tests` (6).
- **Verification at 2ce9819** (its own `tools\verify.ps1 -Quick -Jobs 6`), green on both legs: win debug 2080/2086 (6
  skipped), wsl debug 1977/1983 (6 skipped); lint, licence and feature checks ok. The first run hit a flaky WSL
  FileWatcher test that the slice does not touch; it passed alone three times and the full rerun was green. No
  processes or locks were left running. Not merged or pushed by the slice.

#### Known bugs
- Each new search cancels the previous one and then waits for it on the GUI thread. If very large files are being
  searched, typing can stall for a moment.
- Match columns are UTF-8 byte offsets. The tab fallback passes them to `IEditorView::goToLine`, which counts
  characters, so the caret lands on the wrong column on non-ASCII lines until W1's callback maps them.
- Replace in an open document edits its FileDocument, but an open editor widget only redraws once W1 connects
  `documentEdited` to `Editor::follow`.
- Status texts are not plural-aware ("2 replacements in 1 files").
- The first verification run failed one WSL test, `FileWatcher.NewDirectoryAndItsFilesAreSeen` (io, not touched by
  this slice). It passed three times when rerun alone, and the full rerun was green. It looks timing-sensitive under
  load.

#### Left
- Its hand-off's Next (this entry's Next): W1 installs FindFeature in MainWindow and connects `openLocation`,
  `selectedText` and `documentEdited` (`main`'s EditorView and `Editor::follow`); then the TODO(M2) items, Replace on
  a background lane and the gitignore-aware Directory scope through shell-nav.
- Its merge-order note: the branch touches `shell_menus.cpp` (3 added lines) and `keymap_intellij.cpp` with its
  goldens, so after another branch's keymap change the goldens are regenerated (`FROST_UPDATE_GOLDENS=1`) and the diff
  reviewed. That was done at this merge (the follow-up 3b0025f).
- The orchestrator's decisions on its points: the `FileWatcher.NewDirectoryAndItsFilesAreSeen` flake is not its code
  (io) and is in the deferred list as a timing-sensitive test; Replace in Files running on the GUI thread for closed
  files is accepted for now (TODO(M2), deferred).
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m2/find-in-files`'.

### Slice record: `m1/shell-nav` (ADR 0042)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): 588ddfe on 2e298a8 plus its merges 33dfc14 of `m1/shell-frame` 9576c24 and 0756a45
of `m1/save-manager` e2b2362. Worktree `.claude\worktrees\m1-shell-nav`. (Merge note: merged into `main` as 9d00def,
with the follow-up cfbd46e; the 2026-11-02 entry above.)

#### Done
- **New libraries:** frost::search, frost::ui_nav and frost::ui_workbench; in io `canonicalPath` and the tree scan's
  ignore files and default excludes.
- **frost::app is wired:** persistent AppServices, MainWindow owns the Workbench, main() opens a project or restores
  the last one, and the hot-exit prompt runs after the first frame.
- **ADR 0042** (`docs/adr/0042-shell-navigation.md`). `tests/features/matrix.json`: rows 28, 29, 76, 77, 78, 91 and
  92 moved to `in-progress` with acceptance tests.
- **Verification at 588ddfe** (its own `-Quick`): win debug 1906/1912 (6 skipped), wsl debug 1802/1808 (6 skipped),
  the checks ok (tidy not-run). Not merged or pushed by the slice; it left `docs/progress.md` and PLAN.md untouched,
  as instructed.

#### Known bugs
- The File System view rebuilds its whole QStandardItemModel on every rescan (expansion and selection are kept). This
  is slow on very large trees, and a watcher batch under the root triggers a full rescan.
- The merge view's panes are QPlainTextEdits fed from display rows, not the real editor view, so there is no syntax
  colouring and no editing inside the panes. Choices are made per chunk only.
- Search Everywhere has only Files and Actions sections (Classes and Symbols wait for M3/clangd).
- Recent Locations stores places (path, line, column) without a code snippet preview.
- Command-line files whose suffix is `.sln`/`.slnx` always open as projects. Opening such a file as text needs
  File > Open File.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/shell-nav`'s.
- Its hand-off's Next (this entry's Next): swap PlaceholderEditorFactory for the real editor factory under
  DocumentEditorFactory in AppServices, implementing the seam's `viewState`/`restoreViewState` and
  `IEditorFactory::canonicalPath` over `m1/editor-view`'s EditorView; `w1/tool-windows`' document bindings through the
  DocumentRegistry; the settings actions bound to `SettingsStore::prepareForEditing`. The expected conflicts with
  `m1/editor-view` and `w1/tool-windows` did not come up in `src/app`, since neither had wired anything there (the
  merge section above).

### Slice record: `w2/clangd-nav` (ADR 0041)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): 660a1e9 on top of its merge 49f5bb0 of `w2/clangd-diagnostics` e7d9ccc. Worktree
`.claude\worktrees\w2-clangd-nav`. (Merge note: merged into `main` as 29869f9; the 2026-11-02 entry above.)

#### Done
- **frost::lang:** the MSBuild-evaluated compile-commands provider and the provider chain (existing, then MSBuild, then
  static); the ILspRequester seam; models for completion, signature help, hover, navigation, usages, highlights,
  symbols, rename, code actions and formatting; WorkspaceEdit parsing and the applier.
- **frost::ui_lang** (`src/ui/lang`, new): the widgets and LanguageController, with the IntelliJ actions registered.
- **Keymap:** `navigate.relatedFiles` = Ctrl+Alt+Home (macOS Ctrl+Meta+Up), and the four input goldens updated.
- **ADR 0041** (`docs/adr/0041-clangd-navigation-completion-msbuild-commands.md`), `lang_unit_tests`' new cases,
  `lang_msbuild_tests` (Windows only) and `ui_lang_tests`.
- **Verification at 660a1e9** (its own `-Quick`): win debug 1809/1814 (5 skipped), wsl debug 1703/1711 (8 skipped),
  the checks ok (tidy not-run). Not merged or pushed by the slice.

#### Known bugs
- Snippet tab stops are shifted by the overall document size change, so an edit typed outside the current placeholder
  can put later stops in the wrong place.
- The MSBuild cache does not notice a changed environment variable (such as an SDK root) unless one of the imported
  files also changes.
- None of these features work in the running app yet: nothing implements IEditorLanguageHost over the real editor.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `w2/clangd-nav`'s.
- Its hand-off's Next (W2 in this entry's Next): implement IEditorLanguageHost over EditorView and IWorkspaceDocuments
  over the open documents, dock the usages panel, and feed `solutionCompileCommandsChain` with
  `frost::build::findMsBuild` and the live trust state; then the deferred hardening items.

### Slice record: `m1/editor-rhi` (ADR 0043)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): 9438b85 on top of its merge dd43253 of `m1/editor-view` 3acd71c. Worktree
`.claude\worktrees\m1-editor-rhi`. (Merge note: merged into `main` as 97e490e; the 2026-11-02 entry above.)

#### Done
- **The QRhi glyph-atlas editor renderer** behind IEditorRenderer (`src/ui/editor`).
- **The `editor.renderer` setting** and its auto rule (a remote session, WARP or Linux picks raster), the lazy QRhi
  attach after the first raster frame, a run-time fallback that keeps the editor state, and atlas rebuilds on font,
  zoom and DPI changes.
- **ADR 0043** (`docs/adr/0043-editor-qrhi-renderer.md`) and `ui_editor_tests`' `editor_rhi_test.cpp`.
- **Verification at 9438b85** (its own `-Quick`): win debug 1924/1929 (5 skipped), wsl debug 1821/1828 (7 skipped),
  the checks ok. Not merged or pushed by the slice. Its process hygiene: every process it started had exited; no
  night.py lock or background loop was left.

#### Known bugs
- Under the offscreen QPA a QRhiWidget never initializes and emits `renderFailed`, so the view-level QRhi path (attach,
  then fallback) is tested only up to the failure, and actual on-screen QRhi rendering inside EditorView is unverified.
  The painter itself is pixel-tested offscreen on WARP.
- A theme change does not rebuild the atlas (by design: the atlas holds coverage only). The slice text listed theme
  changes with atlas rebuilds; ADR 0043 records this deviation.
- After a run-time fallback, QRhi is not retried until `editor.renderer` changes; a transient device loss leaves raster
  on for the rest of the session.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/editor-rhi`'s.
- Its hand-off's Next: an on-screen validation of the QRhi viewport when on-screen runs are allowed (the W1 workbench
  follow-up adds a hidden-desktop render self-test first; this entry's Next), and in the hardening phase B5/B6 and
  B1/B2/B15 with QRhi on the user's 5120x1392 work area.

### Slice record: `w1/run-service` (ADR 0040)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): 82c6c56 on 2e298a8 plus its merge 8ea53fb of `w1/build-service` 8d77400. Worktree
`.claude\worktrees\w1-run-service`. (Merge note: merged into `main` as fcc0a46; the 2026-11-02 entry above.)

#### Done
- **frost::build:** RunService, RunSession and `run_target`, and the `onDone` callback on `BuildService::build`.
- **io and project:** ProcessSpec `rawArguments` and `newConsole`; `parseConfigurationProperties`.
- **frost::ui_run** (`src/ui/run`, new): RunConsole, RunController and the run actions.
- **Keymap:** Ctrl+F5 as a Visual Studio alias (`run.runWithoutDebugging`) in the IntelliJ keymap.
- **Fixture:** a synthetic `App.vcxproj.user` and a richer `main.cpp` in `tests/build/fixtures/tiny_sln`.
- **ADR 0040** (`docs/adr/0040-run-service.md`), `build_unit_tests`' run cases, `build_run_tests`,
  `build_msbuild_tests`' runs and `ui_run_tests`.
- **Verification at 82c6c56** (its own `-Quick`): win debug 1732/1737 (5 skipped), wsl debug 1625/1631 (6 skipped),
  the checks ok. Not merged or pushed by the slice.

#### Known bugs
- In the external console, cmd.exe interprets `^ & | < >` and `%VAR%` in the user's argument string, and `%` in the
  program path, because the program is started through `cmd /c`. Visual Studio starts the program directly. The Run
  console passes arguments exactly as written. (Queued: `frost-console-host`; this entry's Next.)
- In the Run console, output from programs that do not flush stdout shows up late, because the C runtime fully buffers
  a pipe (documented; the external console is the default for that reason). Text the program prints through ANSI APIs
  (for example a path containing U+00E9 from `_getcwd`) shows as U+FFFD, since output is decoded as UTF-8.
- The fallback warning ("MSBuild could not report the properties ... imports were not read") is shown again on every
  run while the cached fallback properties are in use.
- The console shows only the latest run. Output from an older run of another project that is still going is dropped
  from view, though it is still delivered to RunService callbacks.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `w1/run-service`'s.
- Its hand-off's Next, the shell integration (this entry's Next, W1): RunConsole in the Run tool window,
  `RunController::bindStartupCombo(MainToolbar::runConfigurations())`, `registerRunActions` with the Solution
  Explorer's selection for `run.setStartupProject`, RunService created with `RunController::callbacks()` and
  `makeRunBuilder(buildService)`, and `RunService::defaultStatePath()`.

### Slice record: `w1/tool-windows` (ADR 0039)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): c81008c on top of its merge 6a591e8 of `w1/build-service` 8d77400, whose conflicts it
resolved by keeping both sides. Worktree `.claude\worktrees\w1-tool-windows`. (Merge note: merged into `main` as
e574965; the 2026-11-02 entry above.)

#### Done
- **frost::ui_toolwindows** (`src/ui/toolwindows`): the Solution Explorer, the Build output pane, Problems,
  BuildActions with the shortcut table, ConfigurationCombo, TrustDialog/TrustPrompter, BuildController with GUI-thread
  marshalling, and ISaveAllHandler.
- **frost::input:** `KeymapService::setModuleDefaults`.
- **ADR 0039** (`docs/adr/0039-build-tool-windows.md`) and `ui_toolwindows_tests` (31 tests).
- **Verification at c81008c** (its own `-Quick`): win debug 1727/1732 (5 skipped), wsl debug 1624/1630 (6 skipped),
  the checks ok (tidy not-run). Its first run had one unrelated StallHooks timing flake. Not merged or pushed by the
  slice.

#### Known bugs
- Linux: Open Containing Folder launches `xdg-open` uncontained; the child is not reaped and stays a zombie until
  FrostIDE exits (the existing io `TODO(M3)` for uncontained POSIX children).
- While a build runs, or with no solution open, Ctrl+Shift+B falls through to `navigate.typeDeclaration` (the build
  action is disabled, so the matcher tries the next candidate). Visual Studio would report "build already in
  progress" instead.
- `BuildController::openSolution` and `closeSolution` emit `diagnosticsCleared` after reading the backend's
  diagnostics. If a backend still had diagnostics at that moment, the Problems view would drop them until the next
  build. BuildService has none after an open, so this is latent.
- Problems: Enter on a row with no location (a project-level message, say) is consumed and does nothing.
- `StallHooks.ASlowSlotIsOneStallAndOneReport` (`tests/app`, not this slice) failed once under the full parallel
  verify load on Windows and passed alone and on the re-run: a timing flake to look at in hardening.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `w1/tool-windows`'.
- The wiring in this entry's Next (W1, now `w1/workbench`'s); its shortcut question is under Questions for the user.

### Slice record: `m1/editor-view` (ADR 0037)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): 3acd71c on 023f354 plus its merges of `m1/editor-model` 0b4aac1 (6ce7513),
`m1/syntax` 681c749 (0976882) and `m1/settings` 481fc26 (b50fb7d). Worktree `.claude\worktrees\m1-editor-view`.
(Merge note: merged into `main` as bde77a5; the 2026-11-02 entry above.)

#### Done
- **frost::ui_editor** (`src/ui/editor`): EditorView, the renderer seam with a raster backend, QtTextShaper,
  highlighting, key routing over the IntelliJ keymap, the find/replace bar, Go to Line, IME, mouse, clipboard, live
  settings and a status/signals/factory API.
- **Also in the commit:** the 37 theme keys, `pasteLines`, the `deleteLines` caret merge, `kLongLineCapBytes`, ADR
  0037 (`docs/adr/0037-editor-view.md`), matrix rows 57, 85, 94 and 250 `in-progress`, and 23 offscreen tests.
- **Verification at 3acd71c** (its own `-Quick`): win debug 1669/1674 (5 skipped), wsl debug 1580/1585 (5 skipped),
  the checks ok. Not merged or pushed by the slice.

#### Known bugs
- Highlighting and row painting copy each visible row's bytes. Soft wrap is on by default, so long lines become many
  short rows, but with wrap off a multi-megabyte line is still shaped up to `kLongLineCapBytes` per row, and `xAt` hit
  tests on it are O(line length).
- A view with an AppearanceManager follows zoom by rebuilding the font when the point size differs. Editor-only zoom
  (Ctrl+wheel) is not implemented.
- Without EditorKeyRouting (`services.keys` null) the view only types printable text: no navigation or shortcuts.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/editor-view`'s.
- Its hand-off's Next: the shell wraps `createEditorView` in its IEditorFactory, maps EditorViewStatus to EditorStatus
  and connects the status signals; the SaveManager feeds `setFileInfo`/`markSaved`; W1 wires the error list to
  `setLineMarkers` and shares one SyntaxSession per document (all `w1/workbench`'s now). The load service's use of
  `kLongLineCapBytes` is done (the long-line follow-up in this entry). Later: the QRhi backend (`m1/editor-rhi`, in
  flight), and in the hardening phase the B5 measurement and the token-contrast thresholds.

### Slice record: `m1/save-manager` (ADR 0028)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): e2b2362 on top of its merges of `m1/diff-merge` 7851f95 (6ef9c47) and `m1/settings`
481fc26 (fc17004), whose CMake lists, NOTICE and `matrix.json` it resolved by keeping both sides. Worktree
`.claude\worktrees\m1-save-manager`. (Merge note: merged into `main` as 5fbd262; the 2026-11-02 entry above.)

#### Done
- **frost::document** (`src/document`, FrostLayers rank 6 through `FROST_LAYER_RANK_document`): LoadService,
  FileDocument, SaveManager (split over `save_manager.cpp`, `external_change.cpp` and `hot_exit.cpp`),
  AutosaveController, SavePolicies and the private journal labels.
- **New in frost::io:** `IFileSystem::safeWrite` (a default for doubles in `src/io/src/file_system.cpp`, overrides for
  Windows and POSIX), `parseAtomicTempName` and `removeStaleTempFiles`. The `TODO(M1)`s at `writeFileAtomic` are
  closed.
- **ADR 0028** (`docs/adr/0028-save-manager.md`, short, 10 decisions). Feature rows I2 and 218 name the acceptance
  tests; both stay `in-progress` until the UI is wired.
- **For the next slices:** the shell wiring in this entry's Next; `m1/editor-view` sets `kMaxEditableLineChars` in
  `load_service.hpp` (merge note: done in the eighth batch's long-line follow-up, as `kMaxEditableLineBytes` =
  `kLongLineCapBytes`). Changes on adjacent lines in both buffer and disk count as a conflict (the same rule as git), so
  the merge view must expect them.
- **Verification at e2b2362** (its own `-Quick`): win debug 1691/1697 (6 skipped), wsl debug 1602/1607 (5 skipped),
  the checks ok (tidy not-run).
- **Processes:** every process it started exited. A cmake/ninja/clang-cl build of another session's
  `scratchpad\runsvc\build.ps1` was left alone. No background loops or locks are left.

#### Known bugs
- Document keys are normalised paths compared case-sensitively, so on Windows opening the same file with different
  letter case gives two documents (storage `TODO(M2)` normaliser; being fixed in `m1/shell-nav`).
- The in-place rewrite of a hard-linked file is not atomic: a crash during it can leave a torn file (the hot-exit
  journal keeps the buffer).
- ReplaceFileW with flags 0 fails with IoError (file left unchanged) where the user can write the file but not merge
  its ACL.
- Saving through a dangling symlink fails with NotFound instead of creating the link's target.
- If the newest journal entry for a key is Corrupt, its journal is discarded even when an older journal held a
  readable copy of that key.
- `restore()` without a journal service (`SaveManagerServices::journal` null) restores into memory and still discards
  the old journals; production always passes a journal.
- `SafeWrite.SymlinksStayLinks` is skipped on this Windows machine (no Developer Mode or elevation), so Windows
  symlink saves are untested here; the test runs on Linux.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/save-manager`'s.
- The SaveManager's consumers in this entry's Next; the orchestrator's decisions on its questions and on ADR 0019 Q5
  are in the merge section above.

### Slice record: `w2/clangd-diagnostics` (ADR 0036)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): e7d9ccc on 023f354 plus its merge of `m1/settings` (a96da6f). Worktree
`.claude\worktrees\w2-clangd-diagnostics`. (Merge note: merged into `main` as db9d24e; the 2026-11-02 entry above.)

#### Done
- **frost::lang** (`src/lang`, new): the LSP client, clangd supervision, compile-command providers, document sync and
  the diagnostics store; ADR 0036.
- **frost::project:** the vcxproj reader keeps the ClCompile metadata.
- **frost::core's log facade**, identical to `main`'s, so merging into `main` stayed clean.
- **Tests:** `tests/lang` with fixtures; matrix row 98 `in-progress`.
- **Verification at e7d9ccc** (its own `-Quick`): win debug 1614/1619 (5 skipped), wsl debug 1525/1530 (5 skipped);
  lint, format, licence and feature checks ok (tidy not-run). The `clangd` label ran 3/3 on both OSes.

#### Known bugs
- Synthesized commands approximate MSBuild. ItemDefinitionGroups with non-configuration Conditions are skipped.
  Entries with unresolved `$(...)`/`%(...)`/`@(...)` references are dropped. A missing RuntimeLibrary is guessed
  (`/MDd` for Debug configurations, otherwise `/MD`). Imported property sheets are not evaluated.
- On Linux the synthesized clang-cl commands have no MSVC/SDK system headers (`findMsvcEnvironment` returns
  Unsupported), so code that includes Windows or STL headers reports missing-header errors there. The tests avoid
  system headers.
- `workspace/applyEdit` is always answered `{applied:false}`, and `window/showMessageRequest` always answers null (no
  action chosen).
- didSave sends text only when the server asked for includeText. clangd does not, so this is fine for clangd, but
  other servers may differ.
- When a ChangeSet does not start from the snapshot the service last recorded (or comes from another Document), the
  change is sent as full text, which is correct but larger.
- Diagnostics from a publish older than the latest document version are kept and marked stale but not re-anchored.
- The clangd `-j` heuristic is fixed (cores/2, at most 8) until the M3 ResourceGovernor sizes it.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `w2/clangd-diagnostics`'.
- The UI slice, then completion and go-to-definition (this entry's Next).

### Slice record: `m1/shell-frame` (ADR 0038)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): one commit, 9576c24, on 023f354. Worktree `.claude\worktrees\m1-shell-frame`. Its ADR
was written as 0035 and is 0038 since the merge. (Merge note: merged into `main` as b1d05bc; the 2026-11-02 entry
above.)

#### Done
- **frost::ui_shell** (`src/ui/shell`): ShellWindow, ShortcutDispatcher, ShellMenus, MainToolbar, EditorTabs,
  PlaceholderEditorFactory, ShellStatusBar, ToolWindowManager v1, Popup/ListPopup/InputPopup, NotificationCenter and
  NotificationLogView, BackgroundTaskManager and ProcessesIndicator, and the IShellSettings and IEditorFactory seams.
- **frost::i18n::tr** in core, `qtr` in ui.
- **frost::app:** AppServices, MainWindow derived from ShellWindow, and the main() wiring (the appearance install with
  a notification fallback, files from the command line).
- **ADR 0038** (`docs/adr/0038-shell-frame.md`). Feature matrix rows 55, 219 and 239-242 `in-progress`, each with its
  acceptance tests.
- **Verification at 9576c24** (its own `-Quick`): win debug 1557/1562 (5 skipped), wsl debug 1468/1473 (5 skipped),
  the checks ok.
- **Processes:** the verify run, its monitor and every test run exited. The sccache server, shared with other agents'
  builds, was left running.

#### Known bugs
- One ShellWindow per ActionRegistry: a second window over the same registry gets AlreadyExists for every shell
  action, posts a 'Some shell actions could not be registered' warning, and its shell actions still drive the first
  window (no-ops once that window is destroyed). AppServices creates one registry per window, so the app is not
  affected.
- If a load from IShellSettings fails, `ToolWindowManager::storeLayout` starts from a default ShellLayout, so toolbar
  and status-bar visibility in that store reset to visible. Only reachable with a failing settings backend.
- With no appearance installed (tests, or a failed install), stripe and toolbar icons fall back to text labels.
- When `frost_process_tests` is run directly through `run_hidden_nobox.py`, outside ctest, three
  `FrostProcess.Restart*` tests fail on both this branch and `main`'s binary (environmental). They pass under
  `verify.ps1`.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/shell-frame`'s.
- The shell's consumers in this entry's Next (W1's tool windows and actions, `m1/editor-view`, the settings adapters,
  `m1/shell-nav`); its reserved action ids are in the merge section above.

### Slice record: `w1/build-service` (ADR 0035)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): 8d77400 on top of its merge 84c332a (`w1/build-msbuild` 111aad0, merged with
`--no-ff`), on `w1/project-sln` 15ba653. Worktree `.claude\worktrees\w1-build-service`. (Merge note: merged into
`main` as eb82b95; the 2026-11-02 entry above.)

#### Done
- **BuildService** (`src/build`): configuration choice and persistence, solution and project Build/Rebuild/Clean, Busy
  refusal, the trust gate, the Save-All hook, cancel, progress, and callbacks on one service thread.
- **Diagnostics:** resolution and an index; English MSBuild output.
- **Process handling:** infrastructure-only cleanup after MSBuild exits (processes a build step started on purpose
  keep running) and the cancel-race fix, on new IProcess container calls in frost::io.
- **frost::project:** `$(VAR)` environment expansion in `.vcxproj`.
- **ADR 0035** (`docs/adr/0035-build-service.md`; 0034 was already taken). It supersedes ADR 0034 decisions 3 and 4.
- **Verification at 8d77400** (its own `-Quick`): win debug 1252/1252, wsl debug 1156/1156, the checks ok (tidy
  not-run). Not merged or pushed by the slice; `docs/progress.md` and PLAN.md were not touched.

#### Known bugs
- Progress counts only projects that print 'X.vcxproj -> output'. Clean and utility projects never count, so
  finished/expected can stay below expected for those builds.
- A diagnostic whose origin is a file without an extension (for example 'Makefile(3): error ...') is treated as a tool
  origin and is not jumpable.
- When a caller sets a UI language other than en-US, VSLANG is left alone, so localized cl/link output is still not
  parsed (I12, M4).

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `w1/build-service`'s.
- Wiring BuildService into `src/ui` and `src/app` (this entry's Next).

### Slice record: `plan/thin-path` (ADR 0029)

The branch kept no record in this log; this is its hand-off note, recorded at the merge. Three commits above 4d18dd5:
c680d56 (the user's answers applied and ADR 0029 accepted; the WIP commit is squashed in), 7add9d2 (a merge of `main`
at dcfd9ae, with the retags) and 6315d86 (W1 and the feature-first phase in section 7). Worktree
`.claude\worktrees\plan-thin-path`. (Merge note: merged into `main` as 414155a; the 2026-11-02 entry above.)

#### Done
- **ADR 0029 accepted:** the thin path to a first usable build, with a minimal debugger in U1 (decision 12: line
  breakpoints, stepping, call stack, threads, variables, watches, Evaluate, stop on crash, MSVC programs through
  PDBs, Windows and Linux) and the deferred halves of M1-M4 in M15, M16 and M17.
- **PLAN.md** section 7 with W1 and the feature-first phase; `budgets.json` gates moved with their features (targets
  unchanged); featurecheck and frost-lint for M0..M17, M0 closed; to-do markers retagged.
- **Verification at 6315d86** (its own `-Quick`): win debug 1520/1525 (5 skipped), wsl debug 1431/1436 (5 skipped),
  the checks ok.

#### Known bugs
- Other ADRs from `main` still use the old slice name `m1/large-file` in ordinary text (ADR 0018's section headings,
  the `encoding_detection.hpp` and `text_file.hpp` comments). Only the to-do markers were retagged.
- ADR 0029's "Done in this change" paragraph summarises the work across all the branch's commits, not one commit.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `plan/thin-path`'s.
- Slices merging after this tag debugger work M4 and work that ADR 0029 moved M15, M16 or M17.

### Slice record: `m1/settings` (ADR 0021)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): one commit, 481fc26, on 767312b. Worktree `.claude\worktrees\m1-settings`. (Merge
note: merged into `main` as 8a5768e; the 2026-11-02 entry above.)

#### Done
- **frost::settings** (`src/settings`, layer settings at rank 5, Qt-free, depends only on frost::io): about 51 keys
  with schemas, defaults and descriptions.
- **Scopes** default < application < `.frost/settings.json` < `.frost/workspace.json` (gitignored) < EditorConfig, and
  the executes/trust rule.
- **Byte-exact JSON editing** on yyjson 0.13.0 (MIT, vcpkg, linked PRIVATE; ADR 0021), migrations with goldens, live
  reload from FileChangeBatch and IDirectoryWatch, validation on save with notifications, and three Settings-dialog
  stand-in actions as data.
- **EditorConfig** resolution and mapping (charset and end_of_line onto `files.encoding`, `files.utf8Bom` and
  `files.lineSeparator`).
- **Matrix rows** I1 and 59 `in-progress`.
- **Verification at 481fc26** (its own `-Quick`): win debug 1332/1337, wsl debug 1244/1249 (the 5 skips per leg are
  the earlier watcher-perf, symlink and locale tests), the checks ok.

#### Known bugs
- If two `set()` calls race on the same scope file, one update can be lost (no per-file lock around
  read-modify-write).
- If the only member of a settings object is removed, comments inside the braces are dropped (the object becomes `{}`
  or `{\n}`).
- On Windows, `scopeOf()` and the watcher path matching are case-sensitive, so a save or change event reported with
  different letter case is not recognised as a settings file.
- EditorConfig: a pathological glob with many `*` and `**` can take exponential time; it is only depth-limited.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/settings`'s.
- The consumers in this entry's Next (`m1/shell-nav` and the composition root).

### Slice record: `m1/diff-merge` (ADR 0019)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): one squashed commit, 7851f95, on 6fb6826; the JGit-like early draft is not in its
history. Worktree `.claude\worktrees\m1-diff-merge`. (Merge note: merged into `main` as 88d75ac; the 2026-11-02
entry above.)

#### Done
- **frost::diff** (`src/diff`, layer text at rank 4, Qt-free): the histogram line diff (JGit's algorithm, rewritten,
  run the way git runs it) with a Myers fallback under the 2^22 work cap, git's classic slider compaction,
  intra-line refinement, a unified-diff renderer, and the three-way merge cores (diff3 chunking, the resolution
  model, conflict markers).
- **The cap regression is fixed exactly, without raising the cap:** the interner's partner path and histogram reuse
  across regions. `DefaultCap.*`, `Histogram.Reuse*` and `Interner.PartnerPathIsExact` are added, and the perf
  `CapScale` assertions are invariants now. ADR 0019 is updated.
- **Tests:** `diff_tests` (unit), `diff_git_tests` (the differential against `git diff --histogram --no-index`,
  integration), `diff_stress_tests`, `diff_perf_tests`, and the `diff_merge` fuzz target with its corpus replay.
- **Verification at 7851f95** (its own `-Quick`, on 6fb6826): wsl debug 497/497; win debug 528/531 with the two
  FrostProcess long-path failures of that old base (not this slice's code; `main` passes them).

#### Known bugs
- Very dense evenly spaced edits (for example every other line of a 10k-line file) and lightly edited files above
  roughly 300k lines per side (diff) or 250k (merge) still degrade under the default cap. This is by design (Q4,
  option (a)); results stay valid and merges stay sound.
- (Merge note: its other known bug, the two FrostProcess long-path failures, belonged to its base 6fb6826; `main`
  passes those tests, the Verification above.)

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/diff-merge`'s.
- `m1/save-manager` builds on it (rank 5 or more for a target that links frost::storage and frost::diff).

### Slice record: `m1/editor-model` (ADR 0025)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): one commit, 0b4aac1, on 877893b (`m1/display-map`). Worktree
`.claude\worktrees\m1-editor-model`. (Merge note: merged into `main` as b2a6b49; the 2026-11-02 entry above.)

#### Done
- **frost::text_editor** (`src/text/editor`, layer text, Qt-free; ADR 0025): the pure editor commands and
  frost::text::Editor, the driver that applies them to a Document and its DisplayMap. It depends on frost::text plus
  RE2 (with Abseil) and utf8proc from vcpkg; the library and its `find_package` calls are guarded by `if(NOT
  FROST_FUZZ)`. frost::text itself only gained `Snapshot::reversedChunks`.
- **Commands:** grapheme cluster and word navigation (emoji, flags, ZWJ, combining marks, CRLF), typing and smart
  keys with their undo grouping, line operations and comments, movement and selection (vertical through the
  DisplayMap, Go to Line, members through IOutlineProvider), find and replace (plain, case, whole word, regex,
  replace all as one undo step).
- **Tests:** `text_editor_tests` (unit), in `tests/text/editor`.
- **Verification at 0b4aac1** (its own `-Quick`): win debug 904/904, wsl debug 817/817, the checks ok.
- **A tooling note for agents:** the tool-call layer turns backslash-u and double-backslash sequences in tool input
  into literal characters, so a backslash-u escape written into a C++ file became a raw non-ASCII character, and
  Python heredocs given a double backslash received a single one. Use `char32_t{0x...}` or hex escapes in sources,
  and the Edit tool for exact replacements.

#### Known bugs
- `previousGraphemeBoundary` scans forward from the line start, or from at most 4 KiB back on longer lines. Inside a
  long run of regional indicators or a very long ZWJ or combining sequence that starts more than 4 KiB back, the
  boundary can be off by one cluster.
- Typing over a closer uses a stateless rule (the line's opener and closer or quote count). It can step over a closer
  the user did not auto-insert, or type a second one after manual edits.
- In a plain case-sensitive search, a pattern holding a bare LF can match the LF inside a CRLF, so the match starts
  between CR and LF.
- Case-insensitive plain search and whole word rely on RE2's simple case folding; full case folding (German sharp s,
  Turkish dotless i) is not done.
- A regex that can match a line break (Whole mode) copies the whole text into a `std::string` on every find, which is
  slow on very large files.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/editor-model`'s.
- `m1/editor-view` (in flight) binds it to the view; the orchestrator's decisions on its questions are in the merge
  section above.

### Slice record: `m1/syntax` (ADR 0023)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): one squashed commit, 681c749, on e9c3214. Worktree `.claude\worktrees\m1-syntax`.
(Merge note: merged into `main` as b706290; the 2026-11-02 entry above.)

#### Done
- **Vendored tree-sitter 0.27.1** plus seven MIT grammars (C, C++, C#, CMake, JSON, Markdown, XML) under
  `third_party/`, pinned in `third_party/vendor.lock.json`, with our own highlight and outline queries.
- **frost::syntax** (`src/syntax`, layer syntax, Qt-free): SyntaxSession and SyntaxTree (incremental parsing on a
  worker, highlight spans, the member outline, enclosing ranges), the vendor-lock tooling (`asset_fetch.py`,
  licensecheck, frost-lint), ADR 0023, `REUSE.toml` and `NOTICE`.
- **Two bugs found in its verification, fixed:** tree-sitter keeps a pointer to the TSQueryCursorOptions it is given,
  and that struct lived on the stack of the query run's constructor (an access violation in the C++ outline); the
  options now live in the QueryRun, which cannot be copied or moved. On Linux, tree-sitter-xml's `common/scanner.h`
  could not find `tree_sitter/parser.h` (clang-cl had resolved it the MSVC way, through the including file's
  directory); each grammar's `src` directory is now on its include path.
- **Verification at 681c749** (its own `-Quick -Jobs 6`): win debug 1176/1176, wsl debug 1090/1090, format,
  frost_lint, licensecheck and featurecheck ok.
- **A tooling note for agents:** in this environment Python's `write_text` on Windows writes CRLF and the Git Bash
  heredoc collapses backslashes; edit files with `newline='\n'` and with scripts written to files, or tracked files
  can end up CRLF (`verify.sh` would then break in WSL).

#### Known bugs
- The outline name of a JSON key that contains an escape (for example `"a\nb"`) may be only one fragment: the
  pattern captures `string_content`, which the escape splits into several children. A key `""` has no outline item.
- The CMake outline recognises `add_library`, `add_executable` and `add_custom_target` only in lower or upper case,
  not mixed case: CMake command names are case-insensitive and `#any-of?` is exact.
- C++: a member function with no return type that is named by a plain identifier is classed as a constructor;
  conversion operators and other special cases are not distinguished further. Free functions declared with pointer
  or reference declarators deeper than one level do not appear in the outline.
- Markdown highlights cover block structure only (headings, lists, quotes, code blocks, link definitions); emphasis,
  inline code and links inside paragraphs need the inline grammar.
- Highlights and the outline report `complete=false` until the language's queries finish compiling on the worker;
  the owner sees them after the next `poll()`.

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/syntax`'s.
- The consumers in this entry's Next (the editor's SyntaxSession wiring).

### Slice record: `m1/verify-tooling` (ADR 0027)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): 2e286e0, on top of 072dd89 (`tools/perf.ps1`), d6e14db and 59cf9c0, on 5f377e7; the
WIP commit bfd769a is squashed into 2e286e0. Worktree `.claude\worktrees\m1-verify-tooling`. (Merge note: merged into
`main` as 08b92fd; the 2026-11-02 entry above.)

#### Done
- **Parallel Windows ctest** (`-j`, `-TestJobs`) with no error boxes from tests (`--interactive-debug-mode 0`).
- **Isolation defaults by kind label** in `frost_add_test` (integration: `RESOURCE_LOCK frost_desktop`; perf:
  `RUN_SERIAL`; `RUN_SERIAL`, `RESOURCE_LOCK` and `PROCESSORS` keywords) and `mod.<directory>` module labels through
  `frost_add_test_directory`, with configure guards for both.
- **The WSL PATH filter** that removes the FindOpenGL configure cost (the F2 item of the entry "M0 decisions").
- **clang-tidy and the TSan quick leg** behind off-by-default switches (`-Tidy`, `-TsanQuick`), `tools/perf.ps1`
  and the B28 harness, and ADR 0027.
- **Measured:** the Windows Debug ctest from 135.3 s serial to 52.9 s at `-j 6`; the Linux re-configure from 30.45 s
  to 1.35 s.
- **Verification at 2e286e0** (its own `-Quick`, on 5f377e7): win debug 488/488, wsl debug 408/408, the checks ok.

#### Known bugs
- The cause of the 0xc0000142 start failures is unknown and they may recur. They now show up as failed tests in the
  log instead of boxes on the desktop.
- Its merge hazards (several labels, `add_subdirectory` of the newer test directories, `ui_native_tests` without a
  lock) are resolved in the merge (the merge section above).

#### Left
- The items in this entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/verify-tooling`'s.
- The orchestrator's decisions on its questions are in the merge section above; it asks the user nothing.

## 2026-11-01: M1 in progress

`main` has M1's first ten slices, the switch to the feature-first phase, the W1 plan and W1's first two slices, each
merged with `git merge --no-ff` in the main worktree under D23. `m1/piece-tree` at 6fb6826 (merge 455bb76),
`m1/input-keymap` at c78db87 (merge 3db37c5), `m1/undo-anchors` at 7ae2ab5 (merge 3163d0b) and `m1/theme-icons` at
73a5143 (merge 94ed47d) merged after the slice's independent reviews. `m1/process-feature-first` at d5f4082 (merge
5612d23) and `m1/storage` at 39837c1 (merge e9c3214) are the first feature-first batch (ADR 0031): a slice merges
after its own `-Quick`, and each batch gets one `-Quick` of the merged tree. The second batch is `plan/w1` at 43ee979
(merge b7ce4fb), which puts W1, the work-ready build, before the rest of U1 (ADR 0032), and `m1/io-reactor-watch` at
e0eaf75 (merge 4cce4c8). The third batch is `m1/encodings` at ab77994 (merge 6455c2b). The fourth batch is
`m1/keymap-de` at 8f80e22 (merge 9ca6a04), `m1/display-map` at 877893b (merge b36c089) and `w1/project-sln` at 15ba653
(merge dcfd9ae). The fifth batch is `w1/build-msbuild` at 111aad0 (merge afa8a35) and `m1/crash-diag` at 4ef7ae3
(merge 3de145c). The slice records follow this entry's sections as subsections, newest first: `m1/crash-diag`'s,
`w1/build-msbuild`'s, `w1/project-sln`'s, `m1/display-map`'s, `m1/keymap-de`'s, `m1/encodings`'s,
`m1/io-reactor-watch`'s, `m1/storage`'s, `m1/theme-icons`'s, `m1/undo-anchors`'s and `m1/input-keymap`'s, written
from their hand-off notes (none of these branches kept a record in this log), then `m1/piece-tree`'s two, kept as the
branch wrote them. Merge notes in them mark what has changed since.

### Done

#### Merge of `w1/build-msbuild` (afa8a35)
- **The merge** (afa8a35) of `w1/build-msbuild` (111aad0) into `main` at 023f354: frost::build, W1's MSBuild
  discovery, build driver and diagnostics parser; the slice record below says what they hold. ADR 0034 (no new
  dependency); the number is unchanged and no other branch uses it. `w1/build-service` (still running, on
  `w1/project-sln` 15ba653) merges 111aad0 inside its branch, so merging 111aad0 into `main` now keeps that later
  merge trivial.
- **One conflict**, resolved so that both sides' intent survives: `tests/CMakeLists.txt` adds `main`'s
  `tests/project` and the slice's `tests/build`, in that order. `src/CMakeLists.txt` merged without conflict:
  `build` follows the `NOT FROST_FUZZ` block of storage and project, since it depends only on io, so fuzz trees now
  build it too (neither fuzz tree was built). The `build` layer rank (7) was already in `cmake/FrostLayers.cmake`.
  `ErrorCode::Untrusted` is appended after `Internal`, so existing values keep their numbers. `git diff --check`
  flags only the fixture's `Tiny.sln`, which is CRLF on purpose (`tests/**/fixtures/**` is `-text`).
- **Orchestrator decisions** (implemented in `w1/build-service`, still running): build output is forced to English
  (`DOTNET_CLI_UI_LANGUAGE=en-US`, `VSLANG=1033`) so the error list is reliable; after a build only build
  infrastructure is ended (MSBuild nodes, mspdbsrv, VBCSCompiler), and programs that a post-build step started keep
  running, as in Visual Studio.
- **The build after the merge commit:** `night.py build-gate` (no lock, 37.2 GB commit free) and `cmake --build
  --preset win-clangcl-debug -j 8` (CMake re-ran on its own): exit 0, 384 steps (the new ErrorCode value rebuilt
  every target), no compiler warning (only Qt's own "No qtpaths executable found" configure notice).

#### Merge of `m1/crash-diag` (3de145c)
- **The merge** (3de145c) of `m1/crash-diag` (4ef7ae3) into `main` at afa8a35: frost::diag (crash minidumps
  through crashpad, frost::log, the UI stall watchdog and freeze reports, the self-tests and retention) and its
  wiring in `src/app`; the slice record below says what they hold. ADR 0022 (crashpad, cpptrace and spdlog through
  vcpkg, crashpad built by the pinned clang-cl from overlay ports); the number is unchanged and no other branch
  uses it (`main`'s ADRs after 0010 are now 0011 to 0018, 0020, 0022, 0026 and 0030 to 0034). The slice ran on
  5f377e7, the piece-tree merge, the oldest base of any slice merged so far; 4ef7ae3 replaces the WIP commit
  be509f2 saved before the PC restart.
- **Seven conflicts**, each resolved so that both sides' intent survives:
  - `src/io/src/process_posix.cpp`: `main` moved the body of `launch()` into `spawnPosixChild`, which `launch()`
    and `launchAsync()` share (ADR 0020). The slice's `ProcessSpec::posixInheritedFds` staging
    (`F_DUPFD_CLOEXEC` copies at or above 3 + k, `dup2` to 3 + i, `closefrom` from the first unpassed
    descriptor) moves into `spawnPosixChild` unchanged, so both paths pass the descriptors. `process.hpp` and the
    validation in `process_common.cpp` merged without conflict; the Windows launcher and the scripted launcher
    reject a non-empty list through `validateProcessSpec`.
  - `src/CMakeLists.txt` and `tests/CMakeLists.txt`: `main`'s input, storage, project and build and the slice's
    `diag` (under `NOT FROST_FUZZ`; rank 8, so after build) are all kept.
  - `vcpkg.json`: asio, cpptrace, crashpad, curl (Linux only, no default features), gtest, pugixml, spdlog (no
    default features), sqlite3 and xxhash, in name order.
  - `REUSE.toml`: the slice's table for its own `frost-getopt.patch` (Apache-2.0, FrostIDE) follows the
    `third_party/vcpkg-ports/**` table it overrides; `main`'s asset tables follow, so the `tests/ui/goldens/**`
    table is still the last one, as its comment says. No other path matches two of these tables.
  - `tests/features/matrix.json`: row I2 keeps `main`'s `in-progress` (storage); row I3 (crash and freeze
    reporting) takes the slice's `in-progress` with its five acceptance tests.
  - `tests/tools/CMakeLists.txt`: `main`'s `asset_fetch` suite and the slice's `symbolize` suite are both
    registered, and the header comment names both.
  - Merged without conflict: the root `CMakeLists.txt` (`find_package` of crashpad, cpptrace and spdlog under `NOT
    FROST_FUZZ`, after storage's and asio's), `CMakePresets.json` (`VCPKG_OVERLAY_PORTS`, which storage had added
    with the same value), `third_party/vcpkg-ports` (crashpad, cpptrace and vcpkg-tool-gn next to sqlite3),
    `cmake/FrostCompilerOptions.cmake` (`-Wl,--build-id=sha1` on Linux), `src/core` (frost::log and
    `Violation::terminating`), `src/app`, `tests/app`, `tests/core`, `tests/io`, and the `-Full`-only symbolize
    step in `tools/verify.ps1` and `tools/verify.sh`.
- **One of the slice's known bugs no longer applies:** `REUSE.toml`'s comment on `third_party/vcpkg-ports` names
  ADR 0014, which was not on the slice's base; it is on `main`, whose identical table the merge keeps.
- **Orchestrator decisions on the slice's questions** (conservative): crashpad's public-domain getopt is replaced
  by FrostIDE's own clean-room Apache-2.0 `getopt_long`, written from the POSIX specification and the glibc manual;
  no WER registry writes for `__fastfail` crashes (a documented gap); on Linux the process is non-dumpable and the
  minidump is still written; no crash handler when elevated; the 10^9-byte retention cap is fine; adding the handler
  pipe to PLAN.md 5.2 and to M2's listener enumeration is deferred (below). ADR 0022's text records the slice's
  choices; these decisions are recorded here.
- **The build after the merge commit:** `night.py build-gate` (no lock, 37.4 GB commit free) and `cmake --build
  --preset win-clangcl-debug -j 8` (CMake re-ran on its own; vcpkg built crashpad from the overlay port in about
  2 min with the pinned clang-cl and restored cpptrace, spdlog, zlib and the GN helper ports from the binary cache).
  It stopped at step 324 of 404 on the merge's one breakage: `main`'s `frost::io::IFileSystem` gained the pure
  virtual `listDirectoryWithDiagnostics` (ADR 0020), which was not on the slice's base, so the test double
  `LinkingFs` in `tests/diag/retention_test.cpp` was abstract (`-Wabstract-final-class`, an error under /WX).
- **The fix** (4d42a50): `LinkingFs` forwards `listDirectoryWithDiagnostics` to MemFs and adds its link entries
  as `listDirectory()` does, through one shared helper; no test, budget or warning changes. The rebuild after it:
  exit 0, 99 steps, no compiler warning.

#### Verification (batch, merged tree 79512d6)
- Before the batch run: the Windows Debug build after each merge and after the build fix (above).
- **The first run** of `tools\verify.ps1 -Quick -Jobs 8`, on 4d42a50 (with this record uncommitted) through
  `run_hidden_nobox.py` (a hidden desktop; no window opened on it), after `night.py build-gate` (no lock, 34.9 GB
  commit free), 674.7 s, exit 1: `win debug 1665/1671 (5 skipped)` with one failure,
  `FrostProcess.AppDllImportsOnlyAllowedDlls` (`m1/crash-diag`). The WSL leg (1564/1570, 6 skipped) and the checks
  passed.
- **The cause:** the test required `frost_app.dll` to have no delay imports, which held on the slice's base. On
  `main`, frost_io delay-loads ws2_32.dll into every binary that links it (`src/io/CMakeLists.txt`, ADR 0020:
  asio's Windows backend references Winsock, and `reactor.cpp`'s guard keeps the DLL from ever loading), and the
  slice's wiring links frost_io into `frost_app.dll` for the crash handler's IProcessLauncher, so the DLL has one
  delay import, `WS2_32.dll`.
- **The fix** (79512d6), so that both intents survive: the test lists every delay descriptor (a new helper,
  `delayImportedDlls`) and requires each to be ws2_32.dll. The old check looked at the first descriptor only, so
  any other delay-loaded DLL still fails, and more strictly than before. ws2_32.dll is a System32 KnownDLL (it is in
  this PC's KnownDLLs list), so where it would load from does not depend on frost's image path, which is the ADR
  0012 concern the test states; `FileWatcherWindows.WinsockIsNeverLoaded` checks that it never loads. The
  launcher's own check (`LauncherImportsOnlyKernel32`, no delay imports) is unchanged. After the rebuild,
  `frost_process_tests` `AppDll*` and `Launcher*` passed 7/7 on a hidden desktop.
- **The second run**, on 79512d6 (with this record uncommitted), after `night.py build-gate` (no lock, 35.3 GB
  commit free), 488.0 s, exit 0. Windows ran its Debug leg and the checks; WSL ran `tools/verify.sh` in the ext4
  mirror (`~/frost-src-e235f3842261`):
  `Verified: win debug 1666/1671 (5 skipped), wsl debug 1564/1570 (6 skipped), wsl licensecheck ok, format ok,
  frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run` (frost-lint 1132 files; licensecheck 27 packages on
  Windows, 23 on WSL). No test failed. The test steps took 255.1 s (Windows) and 176.0 s (WSL). For its last
  three minutes another agent's `-Quick` (`w1/build-service`, in its own worktree) ran at the same time.
- **The six MSBuild integration tests** (label `msbuild`) ran in the Windows leg against VS 2022 Build Tools 17.14,
  5.0 s together.
- **The skips:** on Windows the four `WatcherPerf` tests (outside optimised, unsanitised builds) and
  `PollingWatcher.DirectorySymlinksAreNotFollowed` (creating a directory symlink fails for this account), as in the
  last batch; `LayoutOs.UsLayoutEqualsTheSimulatedTable` ran and passed again. On WSL the four `WatcherPerf` tests,
  `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale` (no translated locale) and, new,
  `CrashpadReporter.PipeThatNeverAppearsTimesOutAndStopsTheHandler`, which is Windows-only (the slice's own WSL
  skip).
- **No test was lost in the two merges.** Against the fourth batch (win debug 1525, wsl debug 1436) the tree adds
  146 tests on Windows and 134 on WSL. That is what the two slices added over their bases: `w1/build-msbuild` 52 and
  43 (1186 and 1091 at 111aad0, against 1134 and 1048 at e9c3214) and `m1/crash-diag` 94 and 91 (579 and 496 at
  4ef7ae3, against 485 and 405 at 5f377e7).
- `-Quick` builds no ASan, TSan or fuzz tree, and `-Full`'s new symbolize step did not run. Fuzz trees build `src`
  without storage, project, diag and the UI, so the merges add frost::build, `ErrorCode::Untrusted` and
  `ProcessSpec::posixInheritedFds` to them; neither fuzz tree was built. The slices' sanitizer, fuzz, stress and
  `-Full` runs are in "Deferred to the hardening phase (ADR 0031)", with `-Full` of `main`.

#### Merge of `m1/keymap-de` (9ca6a04)
- **The merge** (9ca6a04) of `m1/keymap-de` (8f80e22) into `main` at abc014c: every default shortcut of the
  built-in IntelliJ-compatible keymap is reachable on de-DE; the slice record below says what it holds. ADR 0030;
  the number is unchanged and no other branch uses it. The slice also adds a pointer to ADR 0030 in ADR 0016's
  user decisions. 8f80e22 replaces the WIP commit a2b2570 saved before the PC restart.
- **No conflicts.** `tests/features/matrix.json` merged on its own: row 227's (keymaps) acceptance test is `ctest -R
  ^(IntelliJKeymap|KeymapReachability)\.`, as on the branch.
- **A behaviour change on Windows de-DE:** Ctrl+- was Fold Collapse and is now Comment Line (Ctrl+Shift+- Comment
  Block); Fold Collapse moves to Ctrl+ß and Ctrl+Num-. This is JetBrains' documented Linux behaviour, so Windows
  and Linux now agree (ADR 0030; reversible in one rule). Whether Comment Line should be Ctrl+# instead is asked
  under Questions for the user.
- **The build after the merge commit:** `night.py build-gate` (no lock, 39.0 GB commit free) and `cmake --build
  --preset win-clangcl-debug -j 8` (CMake re-ran on its own): exit 0, 79 steps, no compiler warning.

#### Merge of `m1/display-map` (b36c089)
- **The merge** (b36c089) of `m1/display-map` (877893b) into `main` at 9ca6a04: frost::text::DisplayMap; the slice
  record below says what it holds. ADR 0026; the number is unchanged and no other branch uses it. Like ADR 0031
  and 0032, ADR 0026 cites ADR 0029 (decision 9, the layer order), which is still only on `plan/thin-path`.
- **One conflict**, resolved so that both sides' intent survives: `src/text/CMakeLists.txt`. `frost::text` builds
  `main`'s anchor, document and undo-tree sources (ADR 0017) and codec sources with the per-OS `codec_win.cpp` or
  `codec_posix.cpp` (ADR 0018) together with the slice's `text_shaper.cpp` and four `display_*.cpp` sources, in
  one name-ordered list. The header comment describes ADR 0013, 0017, 0018 and 0026; its `TODO(M1)` keeps only
  `m1/large-file`. `tests/text/CMakeLists.txt` merged without conflict: `text_tests` gains `text_shaper_test.cpp`
  and `display_map_test.cpp`, next to the encodings executables.
- **The build after the merge commit:** `night.py build-gate` (no lock, 35.8 GB commit free) and `cmake --build
  --preset win-clangcl-debug -j 8` (CMake re-ran on its own): exit 0, 34 steps, no compiler warning (only Qt's own
  "No qtpaths executable found" configure notice).

#### Merge of `w1/project-sln` (dcfd9ae)
- **The merge** (dcfd9ae) of `w1/project-sln` (15ba653) into `main` at b36c089: frost::project, W1's static
  `.sln`, `.slnx`, `.vcxproj` and `.vcxproj.filters` readers, SolutionModel and the trust store; the slice record
  below says what they hold. ADR 0033 (pugixml 1.16, MIT, from the vcpkg baseline, PRIVATE to `frost_project`);
  the number is unchanged and no other branch uses it (`main`'s ADRs after 0010 are now 0011 to 0018, 0020, 0026,
  0030, 0031, 0032 and 0033). ADR 0034 (`w1/build-msbuild`) is not on any branch yet. (Merge note: it came with
  `w1/build-msbuild`'s merge afa8a35, above.)
- **No conflicts.** The root `CMakeLists.txt` finds pugixml next to SQLite and xxHash under `NOT FROST_FUZZ`;
  `src/CMakeLists.txt` adds `project` under the same guard as `storage` (fuzz trees install no vcpkg ports);
  `tests/CMakeLists.txt` adds `tests/project` after `tests/storage`; `vcpkg.json` lists asio 1.32.0, gtest 1.18.0,
  pugixml 1.16, sqlite3 3.54.1 and xxhash 0.8.4, in name order. The `project` layer rank (6) was already in
  `cmake/FrostLayers.cmake`. `git diff --check` flags only the fixture's CRLF files (`App.vcxproj`,
  `App.vcxproj.filters`, `SdkApp.sln`), which are CRLF on purpose; `tests/**/fixtures/**` is `-text`.
- **Orchestrator decision on the slice's trust question:** the trust file location
  (`%LOCALAPPDATA%\FrostIDE\data\trusted-roots.txt`, XDG on Linux) is fine until M2's settings.
- **The build after the merge commit:** `night.py build-gate` (no lock, 34.1 GB commit free) and `cmake --build
  --preset win-clangcl-debug -j 8` (CMake re-ran on its own; vcpkg installed pugixml 1.16): exit 0, 35 steps, no
  compiler warning (only Qt's own "No qtpaths executable found" configure notice).

#### Verification (batch, merged tree dcfd9ae)
- Before the batch run: the Windows Debug build after each of the three merges (above).
- `tools\verify.ps1 -Quick -Jobs 8` on dcfd9ae (with this record uncommitted) through `run_hidden_nobox.py` (a hidden
  desktop; no window opened on it), after `night.py build-gate` (no lock, 33.5 GB commit free), 510.3 s, exit 0.
  Windows ran its Debug leg and the checks; WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`):
  `Verified: win debug 1520/1525 (5 skipped), wsl debug 1431/1436 (5 skipped), wsl licensecheck ok, format ok,
  frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run` (frost-lint 1029 files; licensecheck 19 packages on
  Windows, 14 on WSL). No test failed. The test steps took 265.5 s (Windows) and 175.1 s (WSL).
- **The skips are the earlier batches' minus one:** on both OSes the four `WatcherPerf` tests (outside optimised,
  unsanitised builds); on Windows `PollingWatcher.DirectorySymlinksAreNotFollowed` (creating a directory symlink
  fails for this account); on WSL `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale` (no translated locale).
  `LayoutOs.UsLayoutEqualsTheSimulatedTable`, which skipped in the last two batch runs and in `m1/keymap-de`'s own
  run, ran and passed this time (the active keyboard layout was en-US). Every test of the three slices ran.
- **No test was lost in the three merges.** Against the encodings merge (win debug 1454, wsl debug 1365) the tree
  adds 71 tests on Windows and 71 on WSL. That is what the three slices added over their bases:
  `m1/keymap-de` 10 on each OS (764 and 677 at 8f80e22, against 754 and 667 at 0110eff), `m1/display-map` 24 on
  each OS (856 and 769 at 877893b, against 832 and 745 at 2fef6ee) and `w1/project-sln` 37 on each OS (1171 and
  1085 at 15ba653, against 1134 and 1048 at e9c3214).
- `-Quick` builds no ASan, TSan or fuzz tree. Fuzz trees build `src` without storage, project and the UI, so the
  merges add the display map (`src/text`) and the layout rule 3.6 (`src/input`) to them; neither fuzz tree was
  built. The slices' sanitizer, fuzz, stress and Release runs are in "Deferred to the hardening phase (ADR 0031)",
  with `-Full` of `main`.

#### Merge of `m1/encodings` (6455c2b)
- **The merge** (6455c2b) of `m1/encodings` (ab77994) into `main` at 767312b: frost::text's load and save codec
  pipeline (22cbc80) and the fixes from the slice's two reviews (ab77994); the slice record below says what they
  hold. ADR 0018; the number is unchanged and no other branch uses it (`main`'s ADRs after 0010 are now 0011 to
  0018, 0020, 0031 and 0032). `tests/features/matrix.json` merged on its own: row 60 (encodings, BOM, EOL) is
  `in-progress` with `EncodingRoundTrip.FixturesByteExact` as its acceptance test, as on the branch.
- **Four conflicts**, each in a CMake list and each resolved so that both sides' intent survives:
  - `src/text/CMakeLists.txt`: `main`'s anchor, anchor-set, selection, document and undo-tree sources (ADR 0017)
    and the slice's codec, detection, line-separator and text-file sources with its per-OS `codec_win.cpp` or
    `codec_posix.cpp` (ADR 0018) are all built, in one name-ordered list. The header comment describes both ADRs;
    its `TODO(M1)` keeps only `m1/large-file`, since the undo and anchor work it also named is on `main`.
  - `tests/text/CMakeLists.txt`: the header comment lists `main`'s undo and anchor additions to `text_tests`,
    `text_stress_tests` and `text_perf_tests` and the slice's four `text_encoding_*` executables; the test targets
    themselves merged without conflict.
  - `tests/text/support/CMakeLists.txt`: `frost_text_test_support` keeps `main`'s UndoOracle and UndoDriver (and
    their private include of `src/text/src`); the slice's `frost_text_encoding_test_support` sits next to it. The
    header comment describes both libraries.
  - `tests/fuzz/CMakeLists.txt`: `main`'s `io_ignore_rules` target (ADR 0020) and the slice's `text_decode` target
    and its corpus are both registered; the header comment lists all three fuzz targets.
- **The FrostProcess failures that the slice's verification lists as known no longer apply.** It ran on its base
  6fb6826, which predates `m0/longpath`, so `FrostProcess.RunsFromPathLongerThanMaxPath` and
  `FrostProcess.PlatformPluginPathBoundary` fail there. `main` has the E1 launcher (ADR 0012); the merged tree's
  results are below.
- **Orchestrator decisions on the slice's questions** (conservative, matching Visual Studio):
  - Invalid UTF-8 keeps the lossless legacy fallback (the system ANSI page on Windows, 1252 on Linux; the status
    bar offers "Reload in UTF-8"), not VS Code-style U+FFFD (ADR 0018 D7's open question).
  - Files with no line break get CRLF on Windows and LF on Linux (D9, until `m1/settings`), and 1252 as the Linux
    legacy candidate is fine (D14's questions for `m1/settings`).
  - No upstream TSan report about the gconv false positive for now (deferred, below).
  - ADR 0018's text still calls these open; the decisions are recorded here.
- **The build after the merge commit:** `night.py build-gate` (no lock, 35.4 GB commit free) and `cmake --build
  --preset win-clangcl-debug -j 8` (CMake re-ran on its own): exit 0, 59 steps, no compiler warning (only Qt's own
  "No qtpaths executable found" configure notice).

#### Verification (batch, merged tree 6455c2b)
- Before the batch run: the Windows Debug build after the merge (above).
- `tools\verify.ps1 -Quick -Jobs 8` on 6455c2b (with this record uncommitted) through `run_hidden_nobox.py` (a
  hidden desktop; no window opened on it), after `night.py build-gate` (no lock, 37.8 GB commit free), 583.6 s,
  exit 0. Three other agents' `-Quick` runs (`w1/project-sln`, `w1/build-msbuild`, `m1/display-map`, in their own
  worktrees and WSL mirrors) ran at the same time, so the timings are not comparable with earlier batches. Windows
  ran its Debug leg and the checks; WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`):
  `Verified: win debug 1448/1454 (6 skipped), wsl debug 1360/1365 (5 skipped), wsl licensecheck ok, format ok,
  frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run` (frost-lint 961 files; licensecheck 18 packages on
  Windows, 13 on WSL). No test failed. The test steps took 346.8 s (Windows) and 172.4 s (WSL).
- **The skips are the previous batch's:** on both OSes the four `WatcherPerf` tests (outside optimised, unsanitised
  builds); on Windows `PollingWatcher.DirectorySymlinksAreNotFollowed` and `LayoutOs.UsLayoutEqualsTheSimulatedTable`
  (the active keyboard layout was again not en-US); on WSL `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale`.
  None of them is in `m1/encodings`' tests, which all ran.
- **No test was lost in the merge.** Against the io merge (win debug 1304, wsl debug 1216) the tree adds 150 tests on
  Windows and 149 on WSL, exactly the slice's `-Quick` tests: `text_encoding_tests` 142 on Windows and 141 on WSL
  (one case is Windows-only), `text_encoding_integration_tests` 3, `text_encoding_perf_tests` 4 and
  `fuzz_replay.text_decode` 1. `text_encoding_stress_tests` (2 tests, label stress) is not in `-Quick`.
- `-Quick` builds no ASan, TSan or fuzz tree. The merge adds the `text_decode` fuzz target, whose corpus replay
  ran; no fuzz tree was built. The slice's sanitizer, fuzz, stress and Release perf runs are in "Deferred to the
  hardening phase (ADR 0031)", with `-Full` of `main`.

#### W1, the work-ready build (ADR 0032)
- **Plan:** W1, the work-ready build, comes before the rest of U1 (ADR 0032; the user's requirement statement of
  2026-11-01). W1: open `.sln`/`.slnx`, a Solution Explorer from static `.sln`/`.slnx`/`.vcxproj`/`.filters`
  readers, edit and save, Build/Rebuild/Clean through the installed MSBuild with Configuration|Platform, Save All
  before build, a trust prompt, build output, and an error list with click-to-jump and editor marks. Then clangd
  live diagnostics, navigation and completion, then the rest of U1. Docs only.
- **Lean slices running:** `w1/project-sln` (ADR 0033) and `w1/build-msbuild` (ADR 0034). Neither number is on
  `main` or on another branch yet; both slices are still at their base e9c3214. (Merge notes: `w1/project-sln`
  finished at 15ba653 and merged as dcfd9ae, above, with ADR 0033, and `w1/build-msbuild` finished at 111aad0 and
  merged as afa8a35, above, with ADR 0034.)
- **ADR 0032 cites ADR 0029** (the thin path), which is still only on `plan/thin-path`, as ADR 0031 does (below).
  No other ADR uses 0032.

#### Merge of `plan/w1` (b7ce4fb)
- **The merge** (b7ce4fb) of `plan/w1` (43ee979) into `main` at 5a754de: ADR 0032. Docs only, no conflicts. The
  number is unchanged and no other branch uses it (`main`'s ADRs after 0010 are now 0011 to 0017, 0031 and 0032).
- **The slice's verification at 43ee979:** frost_lint ok; docs only, no build.

#### Merge of `m1/io-reactor-watch` (4cce4c8)
- **The merge** (4cce4c8) of `m1/io-reactor-watch` (e0eaf75) into `main` at b7ce4fb: frost::io's asynchronous half
  (the slice record below says what it holds). ADR 0020; the number is unchanged and no other branch uses it
  (`main`'s ADRs after 0010 are now 0011 to 0017, 0020, 0031 and 0032). The slice also amends ADR 0002 (a
  comment-only edit of `scheduler.hpp`) and ADR 0003.
- **Two conflicts**, each resolved so that both sides' intent survives:
  - The root `CMakeLists.txt`: `main`'s `find_package` of unofficial-sqlite3 and xxHash under `NOT FROST_FUZZ` (ADR
    0014) and the slice's `find_package(asio CONFIG)` with its fatal messages (fuzz trees use the headers the normal
    preset installed; ADR 0020) are both kept, storage's block first.
  - `vcpkg.json`: the dependencies are asio 1.32.0, gtest 1.18.0, sqlite3 3.54.1 (no default features,
    `omit-load-extension`) and xxhash 0.8.4, in name order.
  - Merged without conflict: `src/io` (the reactor, the watchers, async pipes, the ignore rules and the scan),
    `tests/io/CMakeLists.txt`, `tests/cmake` (the `io_no_static_init` check), `tests/fuzz/CMakeLists.txt` (the
    `io_ignore_rules` target and its corpus) and `tests/support/watch_mirror.hpp`. `tests/storage`, which uses
    `IProcessLauncher` for its crash-consistency child, built unchanged against the slice's `process.hpp`.
- **Orchestrator decisions on the slice's questions:** Phase B (async child pipes, the gitignore matcher and
  `scanTree` for `m1/shell-nav`) stays in this slice; asio's static initialisers (pinned by `io_no_static_init`)
  and the delay-loaded ws2_32 guard are accepted; `WorkingDirectoryBeyondMaxPath` now compares the child's working
  directory by identity (volume serial plus FileId) instead of by string, because the child may see the 8.3
  spelling. That checks the same thing at least as strictly, so it is not a weakening.
- **The build after the merge commit:** `night.py build-gate` (no lock, 39.3 GB commit free), `cmake --preset
  win-clangcl` (vcpkg installed asio 1.32.0) and `cmake --build --preset win-clangcl-debug -j 8`: exit 0, 226
  steps, no compiler warning (only Qt's own "No qtpaths executable found" configure notice).

#### Verification (batch, merged tree 4cce4c8)
- Before the batch run: `git diff --check` clean, and the Windows Debug build after the io merge (above). The
  `plan/w1` merge is docs only, so it got no build of its own.
- `tools\verify.ps1 -Quick -Jobs 8` on 4cce4c8 (with this record uncommitted) through `run_hidden_nobox.py` (a
  hidden desktop; no window opened on it), after `night.py build-gate` (no lock, 36.3 GB commit free), 469.5 s,
  exit 0. Windows ran its Debug leg and the checks; WSL ran `tools/verify.sh` in the ext4 mirror
  (`~/frost-src-e235f3842261`): `Verified: win debug 1298/1304 (6 skipped), wsl debug 1211/1216 (5 skipped), wsl
  licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run` (frost-lint 870 files;
  licensecheck 18 packages on Windows, 13 on WSL). No test failed. The test steps took 231.5 s (Windows) and 157.9 s
  (WSL).
- **The skips are the slice's own five on each OS, plus one from the environment on Windows:**
  - Both OSes: the four `WatcherPerf` tests skip outside optimised, unsanitised builds (`kAssertBudgets`), as the
    slice's run did; their reports are in "Deferred to the hardening phase (ADR 0031)".
  - Windows: `PollingWatcher.DirectorySymlinksAreNotFollowed` (creating a directory symlink fails for this
    account). WSL: `OsError.ErrnoTextStaysEnglishUnderATranslatedLocale` (no translated locale is installed).
  - Windows only, new in this run: `LayoutOs.UsLayoutEqualsTheSimulatedTable` (`tests/input`, untouched by this
    batch) skips when the active keyboard layout is not en-US (00000409), as it was during this run. It ran and
    passed in every earlier record.
- **No test was lost in the two merges.** Against the storage merge (win debug 1134/1134, wsl debug 1048/1048), the
  tree adds 170 tests on Windows and 168 on WSL. That is what `m1/io-reactor-watch` added over its base 0110eff
  (924 and 835 at e0eaf75, against the input-keymap tree's 754 and 667). `plan/w1` is docs only.
- `-Quick` builds no ASan, TSan or fuzz tree. The merge adds asio to fuzz trees (through the normal tree's installed
  headers) and the `io_ignore_rules` fuzz target; neither fuzz tree was built. Sanitizer and fuzz runs of the io
  slice are in "Deferred to the hardening phase (ADR 0031)", with `-Full` of `main`.

#### The feature-first phase (ADR 0031)
- **Process:** the feature-first phase until U1 (ADR 0031, the user's decision of 2026-11-01). One agent per slice,
  simple functional tests, `-Quick`, batch merges. Design critics, independent reviews, sanitizers, fuzzing, stress
  runs, perf gates, goldens and `-Full` move to a hardening phase after U1. CLAUDE.md's definition of done and
  PLAN.md 6.7 point to it. Docs only. Existing tests, budgets and warnings are not weakened; what each slice skips
  goes to this entry's "Deferred to the hardening phase (ADR 0031)" list.
- **ADR 0031 cites ADR 0029** (the thin path to U1, the M4 exit), which is not on `main` yet: it is on
  `plan/thin-path` (acf560a, 4d18dd5). The number is the branch's own; no other ADR uses it.
- **The in-flight M1 slices were saved before the PC restart** as local WIP commits (feature-first phase,
  unverified, not pushed): `m1/syntax` (3c86bd1), `m1/keymap-de` (a2b2570), `m1/io-reactor-watch` (e7daf37),
  `m1/crash-diag` (be509f2), `m1/verify-tooling` (bfd769a), `m1/encodings` (0893943) and `m1/diff-merge`
  (4d14df6). `m1/display-map` had nothing to save: its branch has no commit of its own yet (it is at 2fef6ee). Lean
  agents are finishing them, one per slice; each merges in a later batch after its own `-Quick`. (Merge notes:
  `m1/io-reactor-watch` finished at e0eaf75 and merged as 4cce4c8, `m1/encodings` finished at ab77994 and merged as
  6455c2b, `m1/keymap-de` finished at 8f80e22 and merged as 9ca6a04, `m1/display-map` finished at 877893b and
  merged as b36c089, and `m1/crash-diag` finished at 4ef7ae3 and merged as 3de145c, all above.)

#### Merge of `m1/process-feature-first` (5612d23)
- **The merge** (5612d23) of `m1/process-feature-first` (d5f4082) into `main` at 94ed47d: ADR 0031 and its pointers
  in CLAUDE.md (definition of done, working agreement) and PLAN.md 6.7. Docs only, no conflicts. ADR 0031; the
  number is unchanged and no other ADR on `main` uses it.
- **The slice's verification at d5f4082:** frost_lint ok, `git diff --check` clean; docs only, no build.

#### Merge of `m1/storage` (e9c3214)
- **The merge** (e9c3214) of `m1/storage` (39837c1) into `main` at 5612d23: frost::storage on SQLite 3.54.1 and
  xxHash 0.8.4 (ae718c4), the fixes from the slice's two reviews (4472c64), which an independent re-review
  approved, and the documentation of its one low item (39837c1); the slice record below says what they hold. ADR
  0014; the number is unchanged and no other ADR on `main` uses it (`main`'s ADRs after 0010 are now 0011 to 0017
  and 0031).
- **Three conflicts**, each resolved so that both sides' intent survives:
  - `REUSE.toml`: both sides added tables after the generic Apache-2.0 rules. `main`'s pinned-asset tables (ADR
    0015) and the slice's `third_party/vcpkg-ports/**` table (MIT, "override", ADR 0014) are both kept, the ports'
    table first, so the `tests/ui/goldens/**` table stays the last one, as its comment says. No path matches both.
  - `src/CMakeLists.txt` and `tests/CMakeLists.txt`: `main`'s `text` and `input` (and `tests/fuzz`) and the slice's
    `storage` are all kept, in the order text, input, storage (then fuzz). `src/storage` keeps its `NOT FROST_FUZZ`
    guard; fuzz trees build only `tests/fuzz`, so `tests/storage` needs none. text, input and storage share rank 4
    and storage depends only on io.
  - Merged without conflict: the root `CMakeLists.txt` (`find_package` of unofficial-sqlite3 and xxHash outside fuzz
    trees), `CMakePresets.json` (`VCPKG_OVERLAY_PORTS` in the base preset), `vcpkg.json` (sqlite3 3.54.1 with
    `omit-load-extension`, xxhash 0.8.4) and `tests/features/matrix.json` (rows 218, Local History, and I2
    `in-progress`).
- **The FrostProcess failures that the slice's verification lists as known no longer apply.** It ran on its pre-M0
  base 593e15f, where `FrostProcess.RunsFromPathLongerThanMaxPath` and `FrostProcess.PlatformPluginPathBoundary`
  fail and one test is skipped until `m0/longpath`. `main` has `m0/longpath` (ADR 0012); the merged tree's results
  are below.
- **The build after the merge commit:** `night.py build-gate` (no lock, 42.2 GB commit free), `cmake --preset
  win-clangcl` (vcpkg installed sqlite3 3.54.1 from the overlay port and xxhash 0.8.4) and `cmake --build --preset
  win-clangcl-debug -j 8`: exit 0, 56 steps, no compiler warning (only Qt's own "No qtpaths executable found"
  configure notice).

#### Verification (batch, merged tree e9c3214)
- Before the batch run: frost_lint ok, `git diff --check` clean, and the Windows Debug build after the storage
  merge (above).
- `tools\verify.ps1 -Quick -Jobs 8` on e9c3214 (with this record uncommitted) through `run_hidden_nobox.py` (a
  hidden desktop; no window opened on it), after `night.py build-gate` (no lock, 42.0 GB commit free), 367.4 s,
  exit 0. Windows ran its Debug leg and the checks; WSL ran `tools/verify.sh` in the ext4 mirror
  (`~/frost-src-e235f3842261`): `Verified: win debug 1134/1134, wsl debug 1048/1048, wsl licensecheck ok, format
  ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run` (frost-lint 815 files; licensecheck 17
  packages on Windows, 12 on WSL). No test failed or was skipped. The test steps took 196.4 s (Windows) and
  113.9 s (WSL).
- **No test was lost in the three merges** (theme-icons, process, storage). Against the undo-anchors merge (win
  debug 832/832, wsl debug 745/745), the tree adds 302 tests on Windows and 303 on WSL. That is what the two code
  slices added over their base 593e15f (368 and 334 tests): `m1/theme-icons` 137 and 133 (505 and 467 at
  73a5143), `m1/storage` 165 and 170 (533 and 504 at 4472c64). The process slice is docs only.
- `-Quick` builds no ASan, TSan or fuzz tree. Fuzz trees build neither storage (`NOT FROST_FUZZ`) nor `src/ui`, so
  these merges add nothing to them. The slices' own sanitizer results stand: storage's at 4472c64 (win asan
  189/189, wsl asan 194/194, wsl tsan 194/194) and theme-icons' at 73a5143 (the slice record). `-Full` of `main`
  is in "Deferred to the hardening phase (ADR 0031)".

#### Merge of `m1/theme-icons` (94ed47d)
- **The merge** (94ed47d) of `m1/theme-icons` (73a5143) into `main` at 2fef6ee: themes, icons, fonts, compact mode,
  IDE zoom and HiDPI (0e4b02d) and the fixes from the slice's two reviews (73a5143); the slice record below says
  what they hold. ADR 0015; the number is unchanged and no other ADR on `main` uses it (`main`'s ADRs after 0010
  are now 0011 to 0013 and 0015 to 0017). ADR 0015 stays "proposed" while its six questions are open (Questions
  for the user).
- **Two conflicts**, both add/add of a CMake list that each side created, each resolved so that both sides' intent
  survives:
  - `src/ui/CMakeLists.txt`: `main`'s `add_subdirectory(input)` (frost::ui_input, ADR 0016) and the slice's `model`
    and `theme_compiler` subdirectories and its `frost::ui` library (sources in `src/ui/src`, headers in
    `src/ui/include/frost/ui`, AUTOMOC) are all kept; the header comment names all four libraries. No header name
    is shared between `src/ui/input/include/frost/ui` and `src/ui/include/frost/ui`.
  - `tests/ui/CMakeLists.txt`: `main`'s `ui_input_tests` and `ui_input_native_tests` (`FROST_QT_TEST_PLATFORM`
    "windows") and the slice's eight test executables, its fuzz harness libraries and `frost_ui_test_support` are
    all kept; the header comment lists all ten executables.
  - Merged without conflict: the root `CMakeLists.txt` (`tools/theme_compiler`, `tools/icon_baker` and `resources`
    under `FROST_BUILD_UI`), `tests/features/matrix.json` (rows 243 and 244 `in-progress`, with the slice's
    acceptance tests), `REUSE.toml`, `NOTICE`, `.gitattributes`, `tools/frost_lint.py` (the `qt-svg` rule),
    `tools/licensecheck.py` (the asset gate over `resources/assets.lock.json`) and `tests/tools`. `src/CMakeLists.txt`
    and `tests/CMakeLists.txt` already had the slice's `add_subdirectory(ui)` from the input-keymap merge.
- **The FrostProcess failures that the slice's verification lists as known no longer apply.** It ran on its pre-M0
  base 593e15f, where `FrostProcess.RunsFromPathLongerThanMaxPath` and `FrostProcess.PlatformPluginPathBoundary`
  fail and one test is skipped until `m0/longpath`. `main` has `m0/longpath` (ADR 0012); the merged tree's results
  are below.

#### Verification (merged tree 94ed47d)
- Not run on its own: the PC restart stopped the `-Quick` of 94ed47d before it finished, and no result was kept.
  The batch verification of e9c3214 above, whose tree contains 94ed47d, covers it.

#### Merge of `m1/undo-anchors` (3163d0b)
- **The merge** (3163d0b) of `m1/undo-anchors` (7ae2ab5) into `main` at 0110eff: frost::text's Document (81afd3a)
  and the fixes from the slice's two reviews (7ae2ab5), which an independent re-review of the fix commit approved;
  the slice record below says what they hold. ADR 0017; the number is unchanged and no other ADR on `main` uses it
  (`main`'s ADRs after 0010 are now 0011 to 0013, 0016 and 0017).
- **No conflicts.** The slice branched from 6fb6826, the piece-tree head, and `main`'s `m1/input-keymap` merge
  touches none of its files. `tests/features/matrix.json` merged on its own: row 38 (multi-caret, column mode) is
  `in-progress`, as on the branch.
- **The FrostProcess failures that the slice's verification lists as known no longer apply.** Its base 6fb6826
  predates `m0/longpath`, so `FrostProcess.RunsFromPathLongerThanMaxPath` and
  `FrostProcess.PlatformPluginPathBoundary` fail there and one test is skipped, as in the piece-tree records below.
  `main` has `m0/longpath` (ADR 0012); the merged tree's results are below.

#### Verification (merged tree 3163d0b)
- Before the merge commit: frost_lint ok (478 files), licensecheck ok, featurecheck ok, `git diff --check` clean.
- After the merge commit, `night.py build-gate` (it waited about a minute for another session's measurement,
  `m1-encodings-r2-wsl-asan`, then 27.7 GB commit free), then `cmake --build --preset win-clangcl-debug -j 12`:
  exit 0 in 21 s, no compiler warning (only Qt's own "No qtpaths executable found" configure notice).
- `tools\verify.ps1 -Quick -Jobs 12` on 3163d0b through `run_hidden_nobox.py` (a hidden desktop; no window opened
  on it), after `night.py build-gate` (no wait, 28.5 GB commit free), 259.2 s, exit 0. Windows ran its Debug leg
  and the checks; WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`):
  `Verified: win debug 832/832, wsl debug 745/745, wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok,
  featurecheck ok, tidy not-run`. No test failed or was skipped. Against the input-keymap merge (win debug 754/754,
  wsl debug 667/667) the slice adds 78 tests on each OS. That is what it added over its base 6fb6826: 516 and 482
  at 7ae2ab5, against 438 and 404 at the piece-tree head, so no test was lost in the merge. The test steps took
  136.0 s (Windows) and 81.6 s (WSL). The run rebuilt the text library and its tests in the WSL mirror, so it is
  not a B28 sample; re-measuring B28 is in Next (now "Deferred to the hardening phase").
- `-Quick` builds no ASan, TSan or fuzz tree. The merge had no conflicts and changes no build flag, so the slice's
  own sanitizer results at 7ae2ab5 (151/151 on each of Windows ASan, WSL ASan+UBSan and WSL TSan) and its `-Full`
  fuzz legs at 81afd3a (win fuzz 2/2, wsl fuzz 2/2) stand. Fuzz trees now build `frost_text_test_support` with the
  undo oracle and driver, and `frost_input` from the previous merge; neither tree was rebuilt for this merge, and
  `verify -Full`'s fuzz legs cover them.

#### Merge of `m1/input-keymap` (3db37c5)
- **The merge** (3db37c5) of `m1/input-keymap` (c78db87) into `main` at 5f377e7: frost::input and frost::ui_input
  (4cee32c) and the fixes from the slice's two reviews (c78db87); the slice record below says what they hold. ADR
  0016; the number is unchanged and no other branch uses it (`main` has 0011 to 0013, `m1/storage` holds 0014,
  `m1/theme-icons` 0015, `m1/undo-anchors` 0017, `m1/encodings` 0018 and `m1/diff-merge` 0019).
- **Three conflicts**, each resolved so that both sides' intent survives:
  - `src/CMakeLists.txt` and `tests/CMakeLists.txt`: `main`'s `text` (and `tests/fuzz`) and the slice's `input` are
    all kept, in the order text, input, fuzz. The slice's `add_subdirectory(ui)` under `FROST_BUILD_UI` merged
    cleanly in both.
  - `tests/features/matrix.json`: row I6 takes the slice's state (`in-progress`, acceptance `ctest -R
    ShortcutLayouts`); row I7 keeps `main`'s (`green`, with the M0 long-path acceptance tests).
  - Merged without conflict: the `input` rank (4) in `cmake/FrostLayers.cmake`, which cites ADR 0016 decision 1,
    and `FROST_QT_TEST_PLATFORM` in `tests/support/qt_test_main.cpp`.
- **The keymap fuzz target is not registered yet.** ADR 0016 (Consequences) and the slice's `TODO(M1)`s in
  `tests/input/CMakeLists.txt` and `keymap_fuzz_replay_test.cpp` leave it to the integrator, once
  `cmake/FrostFuzz.cmake` is on the base, to register `input_keymap` with `frost_add_fuzz_target` and delete the
  `KeymapFuzzReplay` test. FrostFuzz.cmake is on `main` now, but a new fuzz target needs a fuzz-tree build and smoke
  on both OSes, which `-Quick` does not cover, so it is a follow-up (Next, now "Deferred to the hardening phase").
  Until then `KeymapFuzzReplay` replays `tests/fuzz/corpus/input_keymap` in every normal tree, as on the branch.
- **The FrostProcess failures that the slice's verification lists as known no longer apply.** It ran on its pre-M0
  base 593e15f, where `FrostProcess.RunsFromPathLongerThanMaxPath` and `FrostProcess.PlatformPluginPathBoundary`
  fail until `m0/longpath`. `main` has `m0/longpath` (ADR 0012); the merged tree's results are below.

#### Verification (merged tree 3db37c5)
- Before the merge commit: frost_lint ok (448 files), licensecheck ok, featurecheck ok. `git diff --check` flags only
  two byte-exact fuzz seeds, `seed-06-bom-crlf.keymap` (CRLF on purpose) and `seed-17-long-line.keymap` (trailing
  padding on purpose); `tests/fuzz/corpus/**` is `-text` in `.gitattributes`.
- After the merge commit, `night.py build-gate` (no lock, 37.6 GB commit free), then `cmake --build --preset
  win-clangcl-debug -j 12`: exit 0 in 17 s, no compiler warning (only Qt's own "No qtpaths executable found"
  configure notice).
- `tools\verify.ps1 -Quick -Jobs 12` on 3db37c5 through `run_hidden_nobox.py` (a hidden desktop; no window opened
  on it), after `night.py build-gate` (no lock, 34.5 GB commit free), 176.1 s, exit 0. Windows ran its Debug leg
  and the checks; WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`):
  `Verified: win debug 754/754, wsl debug 667/667, wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok,
  featurecheck ok, tidy not-run`. No test failed or was skipped. Against the piece-tree merge (win debug 485/485,
  wsl debug 405/405) the slice adds 269 tests on Windows and 262 on WSL. That is what it added over its base
  593e15f: 637 and 596 at c78db87, against 368 and 334 on the base (the piece-tree slice's 438 and 404 at e25c036
  less the 70 it added), so no test was lost in the merge. The run includes `KeymapFuzzReplay`, the
  `ShortcutLayouts` suites, `input.NoLayoutSwitching` and its planted-name check, and on Windows `NativeKeyDelivery`
  (Qt's "windows" plugin with a window that is created but never shown).
- `-Quick` builds no ASan, TSan or fuzz tree. Fuzz trees build all of `src`, so the merge adds `frost_input` to
  them. The Windows fuzz tree (`win-clangcl-fuzz`, /MT, ASan) was therefore built on its own after `build-gate`:
  exit 0, `frost_input.lib` compiled and linked with no warning; `fuzz.text_piece_tree` was not re-run (its sources
  are unchanged). The Linux fuzz tree was not built (the WSL mirror has none); `verify -Full`'s fuzz legs cover it.
  The slice's own ASan, ASan+UBSan and TSan results at c78db87 stand: the conflict resolution changes only
  `add_subdirectory` lines and two matrix rows.

#### Merge of `m1/piece-tree` (455bb76)
- **The merge** (455bb76) of `m1/piece-tree` (6fb6826) into `main` at 2da44fb, the `m0` sign-off: frost::text's
  piece tree (c8757f3), the review fixes and repeated exit runs (e25c036), and the ADR renumbering (6fb6826).
- **ADR 0013, not 0012.** The branch first wrote its record as ADR 0012. `m0/longpath` took that number for the
  long-path launch, so 6fb6826 renamed the record to `docs/adr/0013-text-buffer.md` and updated every reference
  in the branch (comments and file names only). The merged tree cites the text buffer as ADR 0013 throughout; no
  text-buffer reference to 0012 is left.
- **Two conflicts**, each resolved so that both sides' intent survives:
  - The root `CMakeLists.txt`, in the clang-cl ASan block. `main` collects the sanitizer flags in
    `FROST_SANITIZE_COMPILE_OPTIONS` and `FROST_SANITIZE_LINK_OPTIONS`, which `frost_apply_compile_options` adds
    per target so that frost.exe's launcher can opt out (ADR 0012). The slice linked the static-CRT ASan inputs
    (`FROST_ASAN_STATIC_LINK_OPTIONS`) into the /MT fuzz trees with a global `add_link_options`. The merged block
    keeps `main`'s per-target variables and sets `FROST_SANITIZE_LINK_OPTIONS` to the static inputs when
    `CMAKE_MSVC_RUNTIME_LIBRARY` is `MultiThreaded`, and to the dynamic ones otherwise. The fuzz targets and
    `frost_text_test_support` call `frost_apply_compile_options`, so every FrostIDE target in a fuzz tree still
    gets ASan. The comment on the static inputs in `cmake/toolchains/windows-clangcl.cmake` now says where they go.
  - `docs/progress.md`: this entry, with the slice's records moved under it.
  - Merged without conflict: the fuzz presets in `CMakePresets.json`, `text` and `fuzz` in `src/CMakeLists.txt`
    and `tests/CMakeLists.txt`, the verify scripts' fuzz legs, `tests/support/test_env.hpp`, and row 35 of
    `tests/features/matrix.json` (`in-progress`).
- **The FrostProcess failures that the slice's records list as known no longer apply.** They count
  `FrostProcess.RunsFromPathLongerThanMaxPath` and `FrostProcess.PlatformPluginPathBoundary` as failures and
  `FrostProcess.ReportsLongPathLimitCleanly` as the skip, all waiting for `m0/longpath`. `main` has `m0/longpath`
  (ADR 0012): `RunsFromPathLongerThanMaxPath` passes, and 4c5533d replaced the other two with tests that assert at
  least as much (entry "M0 complete"). `main`'s sign-off `-Full` at c87c56a had no failure and no skip, and
  neither has the merged tree's `-Quick` (Verification, below).
- **What the slice verified** (its records below): `verify -Full` twice at e25c036, green apart from those
  failures; the 10M-op differential in Windows Release with 0 mismatches (at c8757f3 on both OSes); a clean
  10-minute fuzz on each OS. 6fb6826 changed comments and file names only.

#### Verification (merged tree 455bb76)
- Before the merge commit (455bb76): frost_lint ok (356 files), licensecheck ok, featurecheck ok, `git diff
  --check` clean.
- `tools\verify.ps1 -Quick -Jobs 8` on 455bb76 through `run_hidden_nobox.py` (a hidden desktop; no window opened
  on it), after `night.py build-gate` (no lock, 38.8 GB commit free), 148.9 s, exit 0. Windows ran its Debug leg
  and the checks; WSL ran `tools/verify.sh` in the ext4 mirror (`~/frost-src-e235f3842261`):
  `Verified: win debug 485/485, wsl debug 405/405, wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok,
  featurecheck ok, tidy not-run`. Against `main`'s sign-off (win debug 415/415, wsl debug 335/335) the slice adds
  70 tests on each OS. No test failed or was skipped, the FrostProcess long-path tests included.
- The conflict resolution changes only the /MT fuzz tree, which `-Quick` does not build. So the Windows fuzz tree
  was checked on its own (`win-clangcl-fuzz` in `build\win-clangcl-fuzz`, hidden desktop, after `build-gate`):
  configure, build and `ctest -L fuzz`, fuzz 2/2 (`fuzz.text_piece_tree` 61.1 s, no artifact). The fuzz target
  links the static thunk and libFuzzer whole-archive, without `/INCLUDE:__asan_seh_interceptor`, as on the branch.
  Every compile command there has `-fsanitize=address` and `-fsanitize=fuzzer-no-link`.

### Next
- **W1, the work-ready build** (ADR 0032), before the rest of U1: `w1/project-sln`'s static readers, SolutionModel and
  trust store (ADR 0033) and `w1/build-msbuild`'s MSBuild discovery, build driver and diagnostics parser (ADR 0034)
  are on `main`. Left: the Solution Explorer view and opening a `.sln`/`.slnx` from the command line, File > Open and
  the recent list (on SolutionModel; environment-variable expansion joins it if the user's answer under Questions for
  the user needs it), and `w1/build-service` (running, on 15ba653 with 111aad0 merged in: `checkBuildTrust` before
  the first build, the build output and the error list with jumpable paths, English build output, ending only build
  infrastructure after a build, and the known bugs in `w1/build-msbuild`'s slice record), merged in batches as each
  passes its own `-Quick`. Then clangd live diagnostics (how they get compile commands depends on the
  open question about the "command-list file", under Questions for the user), then navigation and completion, then the
  rest of U1.
- **The in-flight M1 slices** (Done, "The feature-first phase"): `m1/syntax`, `m1/verify-tooling` and
  `m1/diff-merge`, merged in batches as each passes its own `-Quick`.
- **The frost::text display-map consumers** (ADR 0026): `m1/editor-model` and `m1/editor-view` (mapping, painting
  with DisplayRowCursor, hit testing, vertical caret movement, block decorations for the compare and merge views'
  fillers), and M15's folds and inlays on the final fold and inlay interfaces.
- **The frost::text encodings consumers** (ADR 0018): the SaveManager slice (row I2: `encodeForSave` and the lossy
  save guard keep BOM, EOL and encoding), the status bar's encoding and line-separator items (`formatStatus`,
  "Reload in", "Convert to"), `m1/settings` (the default legacy page and the separator for files without breaks,
  D9 and D14) and `m1/large-file` (the mapped path for files of 128 MB and more; a single-validation path and fused
  passes for mapped originals if its B15 measurement needs them; bounding a file that grows between `status` and
  the read: the `TODO(M1)`s in `src/text`).
- **The frost::io consumers** (ADR 0020, Consequences): `m1/shell-nav` (`scanTree`), the File System view (one
  recursive watch per project root, fanning batches out to settings and the SaveManager), the SaveManager slice
  (row I2: refusing a read-only target on POSIX and sweeping stale `.frost-tmp-*` files, moved there from `src/io`)
  and logging the resolved launch through frost::log, which `m1/crash-diag` put on `main` (row I3; the io
  `TODO(M1)`s, in "Deferred to the hardening phase (ADR 0031)").
- **The frost::storage consumers** (ADR 0014): SaveManager, autosave and external-change merge (row I2), the Local
  History UI (row 218), and FSEG segments, the cache-key path normaliser and ProjectModelCache (`TODO(M2)` in
  `src/storage/CMakeLists.txt`).
- **The frost::input consumers** that ADR 0016's Consequences names: `m1/shell-frame` with `m1/settings`
  (IKeymapSettings, the dispatch filter, menus), `m1/editor-view` and `m1/editor-model` (the editor actions and
  contexts), `m1/shell-nav` (the Find Action popup and the Keymap page; Find Action must search the shortcuts'
  display text, so that on de-DE "ctrl+-" finds Comment Line and "ctrl+ß" parses: `m1/keymap-de`'s known bug) and
  `m1/automation-bench` (the real native key path on the real desktop, which needs your approval while you are at
  the PC).
- **The frost::ui consumers** that ADR 0015's Consequences names: `m1/shell-frame` (`install()` from `main()`,
  InstallReport warnings, `resources/` in `tests/app`, B1 on screen, the shell on IslandHost and IslandFrame),
  `m1/settings` (the IAppearanceSettings adapter with live reload) and `m1/editor-view` (editor colour keys and
  `ensureEditorFonts()`); `m1/large-file`'s mapping API for the atlases (ADR 0015 question 4).
- The other M1 slices: `m1/large-file` (mapped originals, B15 at the buffer level, the background release of big
  trees: `TODO(M1)` in `snapshot.hpp`) and the editor model (the slice record "M1 first slice", "Next").
- The M1 work from ADR 0011's Consequences and ADR 0012's `TODO(M1)`s (entry "M0 complete").

### Deferred to the hardening phase (ADR 0031)
Carried forward on 2026-11-02 into the entry "2026-11-02: M1 in progress", whose list is the running one.

### Blockers
- None. The piece-tree slice's only listed blocker, `m0/longpath`, is on `main`; the hand-off notes of
  `m1/input-keymap`, `m1/undo-anchors`, `m1/theme-icons`, `m1/storage`, `plan/w1`, `m1/io-reactor-watch`,
  `m1/encodings`, `m1/keymap-de`, `m1/display-map`, `w1/project-sln`, `w1/build-msbuild` and `m1/crash-diag` name
  none.
- B1 warm (about 195 ms on screen against p50 150 ms) stays M1 work (ADR 0011, A3; entry "M0 complete").

### Questions for the user
`w1/build-msbuild` and `m1/crash-diag` ask none; the orchestrator's decisions on their questions are in their merge
sections above.

From `w1/project-sln` (ADR 0033), asked in chat; the static reader's behaviour stands meanwhile:
- **Your SDK's generated `.vcxproj` files:** do they name sources through environment variables or imported `.props`
  files? If so, environment-variable expansion goes into the Solution Explorer slice. Today such files are missing
  from the tree and listed in `SolutionModel::warnings()`.

From `m1/keymap-de` (ADR 0030), asked in chat; the defaults are kept meanwhile:
- **Build Solution:** keep IntelliJ's Ctrl+F9, or also bind Visual Studio's Ctrl+Shift+B (it would replace Go to Type
  Declaration)?
- **German Comment Line:** keep Ctrl+- (the key right of '.'), or prefer Ctrl+#?

From `plan/w1` (ADR 0032), asked at its merge:
- **The "command-list file":** what is the command-list file that your new-project script generates (is it
  `compile_commands.json`?). It decides how live diagnostics get their compile commands after W1: from that file,
  from the generated CMake side, or derived from the projects.

From `m1/storage` (ADR 0014, Consequences; the conservative choice is implemented), asked at its merge:
- **ADR 0014 Q1:** where should SaveManager live, given that `src/text` and `src/storage` share FrostLayers rank 4
  (and equal ranks may not depend on each other)?
- **ADR 0014 Q2:** Linux file modes for the user-data stores (local history, hot-exit journal): keep the umask's
  (current, as SQLite creates them) or force 0600?
- **ADR 0014 Q3:** should disk full get its own ErrorCode rather than IoError?
- **ADR 0014 Q4:** who owns the journal fuzz target once fuzz trees install vcpkg ports?
- **ADR 0014 Q5:** how long should quarantined copies be kept before M3's cache budget prunes them (currently: until
  then)?
- **ADR 0014 Q6:** should Invalidate Caches offer to clear local history (currently: never, it is user data)?

From `m1/theme-icons` (ADR 0015), asked at its merge:
- **Glacier Classic** is a dark, flat theme in the spirit of Darcula (no island gaps or radius). Should it be light
  and flat instead, or both? (One JSON file either way.)
- **The default on a fresh profile:** Glacier Dark with Sync with OS off (Rider's default); the first-run wizard can
  offer Sync later. Or Sync with OS on by default?
- **IDE zoom steps** 80/90/100/110/125/150/175/200%, with any stored value from 50 to 300% accepted. OK?
- **ADR 0015 Q1:** Glacier Classic is held to the 1.20:1 island-contrast rule (it passes at 1.25:1). Should a flat
  theme be exempt?
- **ADR 0015 Q2 (licence):** goldens that show rendered Lucide icons are annotated "Apache-2.0 AND ISC AND MIT"
  with the FrostIDE, Lucide and Feather copyright lines. Acceptable, or should goldens use only FrostIDE-authored
  icons?
- **ADR 0015 Q3:** unchecked check-box borders meet WCAG 1.4.11's 3:1 in every theme (stricter than "borders only
  in High Contrast"). Confirm.
- **ADR 0015 Q4:** icon atlases are read into memory, not memory-mapped (PLAN.md 3.4 says mapped); they are 10 to
  240 KB. Confirm.
- **ADR 0015 Q5:** TSan runs of the Qt widget tests suppress reports raised inside libQt6Core, Gui and Widgets
  (`called_from_lib`), so they see less than default TSan. Approve, or skip TSan for those executables until an
  instrumented Qt exists?
- **ADR 0015 Q6:** the golden comparator allows up to 16 pixels differing by up to 32 per channel (every golden
  matches with 0 difference today). Confirm, or tighten (for example to 8 pixels)?

From `m1/undo-anchors` (ADR 0017), asked at its merge:
- **Undo defaults** (non-blocking; they become settings later): consecutive typing, backspace and delete merge into
  one undo step within 1000 ms while the caret stays put; history is capped at 1000 steps per document. Keep, or
  other values?
- **Edits from elsewhere** (another split on the same file, formatting, reload): carets move after text inserted
  exactly at them, and selections grow to include text inserted at their edges. Keep, or should carets stay before
  inserted text? One function sets this.
- **IME:** preedit text sits in the buffer tagged as composition (never in undo, never saved: `committedText()` is
  what saves use), so highlighting sees it while composing. The alternative is a view-only inlay; `m1/editor-view`
  decides. Any preference?
- **Only if B28** (`-Quick` p50 5 min) is still over budget at M1 exit: may `AnchorSet.MatchesNaiveModel` and the
  `UndoProperty` cases move to the stress label? That would weaken `-Quick`, so it needs your approval; nothing is
  moved now.

### User decisions (2026-11-01, evening)
The user answered the open questions; their words are quoted.
- **TextSummary provenance (ADR 0013):** "Accept record", option (a). Nothing changes; ADR 0013's Sources say so.
- **IntelliJ keymap provenance (ADR 0016 decision 8):** "Yes it's acceptable and okay".
- **The xkb test oracle's licence:** "Do what you recommend. I'm not sure here. Please elaborate what my options
  are." Adopted as recommended: an ADR approves libxkbcommon and xkeyboard-config by name (MIT plus HPND-style
  notices; HPND is not added to the general allowlist). They are used test-only now, as an independent oracle for the
  simulated layouts in `m1/keymap-de`, and later as the Linux active-layout reader. Qt's Linux platform plugins
  already load libxkbcommon, so the later reader adds no new shipped library.
- **The restart's cost (ADR 0012):** about 10 ms against the 5 ms quoted: "It's OK".
- **Elevated starts from 260+ paths (ADR 0012):** "Keep refusal".
- **The shell's 260-unit limit (ADR 0012):** "If this 260+ limit is a hard limit on windows for built in windows
  things we shouldn't be putting this much resources into circumventing it". Long install paths are frozen as built:
  no further work on install directories of 260+ units, and an OS limit that Windows' own tools cannot pass is
  documented, not worked around. The built launcher and restart stay; their tests take about 2.3 s of a Windows test
  run. Long paths of project files (deep source trees) are ordinary file I/O and stay supported.
- **German keyboards (ADR 0016):** "Support german keyboard layout". `m1/keymap-de` makes every default shortcut
  reachable on de-DE, including comment line and block.
- **Linux punctuation shortcuts in M1:** "It's okay for M1".
- **French (Standard, AZERTY):** "Can be done later". Recorded as a later item in ADR 0016.
- **PLAN.md section 3.2:** "Yes add them". `src/input` and `src/ui/input` are now listed.
- **The roadmap:** "Yes I want something usable before a fully polished version". The milestones are being
  reordered into a thin path to a first usable build (ADR 0029, in progress).

### Slice record: `m1/crash-diag` (ADR 0022)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): one commit, 4ef7ae3, on 5f377e7, which replaces the WIP commit be509f2 saved before
the PC restart. Worktree `.claude\worktrees\m1-crash-diag`. (Merge note: merged into `main` as 3de145c and pushed;
the M1 entry above.)

#### Done
- **frost::diag** (`src/diag`, layer diag, Qt-free; ADR 0022): crashpad built by the pinned clang-cl through vcpkg
  overlay ports, cpptrace without libdwarf, and spdlog.
- **Crash minidumps** to `<cache>/crashes/crashpad` (`%LOCALAPPDATA%\FrostIDE\caches`; XDG on Linux), never
  uploaded. The crashpad handler is started by `frost_app.dll` through IProcessLauncher after the first frame (the
  `frost.exe` launcher still imports only KERNEL32). After a short-link restart only the restarted process starts
  one, and an elevated process starts none.
- **Fatal paths** go to the reporter with the reason, the expression, file:line and the stack addresses:
  FROST_PRE/POST/ASSERT and enforce, `std::terminate`, `abort`, the CRT's invalid-parameter and purecall handlers,
  and Qt fatal messages. No Linux core dump.
- **frost::log** in core (an spdlog sink; `<cache>/logs/frost-<UTC>-<pid>-<n>.log`, 8 MiB rotation, 4 files per
  process, `FROST_LOG_LEVEL`, a 50 ms flush at exit).
- **The UI stall watchdog:** stalls over 50 ms, stack samples, JSON freeze reports, an early report after 2 s, at
  most 20 per session and 10 s apart; nested loops, modal and size-move loops, menus and ScopedUiIdle are excluded;
  B21's stall count.
- **`--selftest=crash` and `--selftest=freeze`**, with no WER or CRT boxes.
- **Retention:** 30 days, then oldest first above 10^9 bytes; only its own file patterns; links are never
  followed.
- **`tools/symbolize.py`**, unit-tested; its verify step is wired into `-Full` only.
- **`ProcessSpec::posixInheritedFds`** (frost::io), for the handler's socket on Linux.
- **Matrix row I3** `in-progress`.
- **Verification at 4ef7ae3** (its own `-Quick`, on 5f377e7): win debug 579/579, wsl debug 495/496 (1 Windows-only
  skip), format, frost_lint, licensecheck and featurecheck ok.

#### Known bugs
- A crash inside the `frost.exe` launcher before `frost_app.dll` loads produces no report.
- `__fastfail` crashes (/GS, CFG, heap corruption) go to WER instead of crashpad.
- Possible flakiness under heavy load: the StallHooks timing tests (30 ms gives no stall, 120 ms exactly one) and
  the GUI handler-start tests.
- Linux freeze sampling interrupts the UI thread with a real-time signal, so UI-thread code must tolerate EINTR.
- `REUSE.toml`'s comment on `third_party/vcpkg-ports` names ADR 0014. (Merge note: ADR 0014 is on `main`; no longer
  a bug.)

#### Left
- The items in the M1 entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/crash-diag`'s.
- The orchestrator's decisions on its questions are in the merge section above; it asks the user nothing.

### Slice record: `w1/build-msbuild` (ADR 0034)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031) and W1's second (ADR 0032): one commit, 111aad0, on e9c3214. Worktree
`.claude\worktrees\w1-build-msbuild`. (Merge note: merged into `main` as afa8a35 and pushed; the M1 entry above.)

#### Done
- **frost::build** (`src/build`, layer build, Qt-free; ADR 0034, no new dependency).
- **MSBuild discovery** through vswhere (`-sort -prerelease -products * -requires Microsoft.Component.MSBuild
  -format json -utf8`): every instance including Build Tools, newest first, an instanceId override, and a clean
  NotFound without Visual Studio. It finds VS 2022 Build Tools 17.14 on this PC.
- **startMsBuild and BuildSession:** Build, Rebuild and Clean of a `.sln`/`.slnx`, of one project through the
  solution's targets (`-t:Lib:Rebuild`, names sanitized as MSBuild does), or of a `.vcxproj` alone with the
  `Solution*` properties. Configuration and Platform are escaped. `-m -nologo -nodeReuse:false -v:minimal
  -clp:NoSummary;ForceNoAlign;DisableConsoleColor`; `MSBUILDTERMINALLOGGER=off`, `MSBUILDDISABLENODEREUSE=1` and a
  private `_MSPDBSRV_ENDPOINT_` per build; UTF-8 output through `DOTNET_CLI_UI_LANGUAGE`.
- **Output and cancel:** stdout and stderr are streamed line by line on worker threads, with serialized callbacks;
  cancel ends the whole tree through a Job Object. The result holds success, cancelled, the exit code, the
  duration, the counts and the unique diagnostics.
- **The diagnostics parser** (both OSes) for cl, link (including LINK fatal errors), MSBuild, rc and clang-cl: every
  location form, subcategories, the `[project]` suffix, `N>` prefixes, spaces and non-ASCII.
- **Trust:** `ErrorCode::Untrusted` is appended to frost::core after `Internal` (existing values keep their
  numbers); builds refuse unless trusted.
- **Linux:** discovery and the driver return Unsupported.
- **Tests:** 46 unit tests on Windows (43 on Linux), and 6 integration tests against the real MSBuild, labelled
  `msbuild`: `Tiny.sln` builds, links, runs and cleans; warning C4996 and error C2065 with file, line, code and
  severity; `Lib:Rebuild` only; a `.vcxproj` alone; `Tiny.slnx`; cancel ends MSBuild, cmd and ping within 10 s.
- **Verification at 111aad0** (its own `-Quick`, on e9c3214): win debug 1186/1186, wsl debug 1091/1091, format,
  frost_lint, licensecheck and featurecheck ok.

#### Known bugs
- Until `w1/build-service` lands: everything left in the job is ended when MSBuild exits, so a program that a
  post-build step starts is closed.
- Until `w1/build-service` lands: `cancel()` right as MSBuild finishes can report `cancelled=true` for a completed
  build.
- `Diagnostic.file` is the origin as printed (LINK, MSBUILD, cl, main.obj, relative paths); `w1/build-service`
  resolves jumpable paths.
- Messages from non-English cl and MSBuild installs are untested (I12).
- The empty work folder `tests/build/msbuild-work` stays in the build tree after the MSBuild tests.

#### Left
- The items in the M1 entry's "Deferred to the hardening phase (ADR 0031)", marked as `w1/build-msbuild`'s.
- The orchestrator's decisions (English build output, ending only build infrastructure) are in the merge section
  above and are implemented in `w1/build-service`; it asks the user nothing.

### Slice record: `w1/project-sln` (ADR 0033)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031) and W1's first (ADR 0032): one commit, 15ba653, on e9c3214. Worktree
`.claude\worktrees\w1-project-sln`. (Merge note: merged into `main` as dcfd9ae and pushed; the M1 entry above.)

#### Done
- **frost::project** (`src/project`, layer project, Qt-free; pugixml 1.16, MIT, ADR 0033). Never runs MSBuild.
- **Static readers:** `.sln` (Format 12; BOM, UTF-16, CRLF and LF; projects, folders, nesting, solution items,
  solution configurations and the per-project mapping including Build.0); `.slnx` (build types x platforms,
  nested folders, items, project overrides, dependencies); `.vcxproj` (GUID, namespace, configuration type and
  toolset per configuration, configurations, every source, header, resource, image, natvis and custom-build item;
  `../` paths; `*`, `?` and `**` wildcards with Exclude; `$(ProjectDir)`, `$(MSBuildThisFileDirectory)`,
  `$(SolutionDir)` and in-file properties expanded; ProjectReference); `.vcxproj.filters` (nested and empty
  filters).
- **SolutionModel** for the Solution Explorer: stable node ids and reload-stable keys, children, kinds and paths;
  C#, VB, F# and shared-items projects as "not supported yet" nodes; missing or unreadable projects as
  "unavailable" nodes (never a failed open); the owning project, dependencies and the configuration mapping.
- **The trust store** in `%LOCALAPPDATA%\FrostIDE\data\trusted-roots.txt` (XDG on Linux), with
  `checkBuildTrust(store, model)` for the build slice.
- **Tests:** 37 functional tests (`project_unit_tests`) over a hand-written SDK-shaped fixture
  (`tests/project/fixtures/sdk_solution`, no proprietary content).
- **Verification at 15ba653** (its own `-Quick`): win debug 1170/1171 (1 environment skip), wsl debug 1085/1085,
  format, frost_lint, licensecheck and featurecheck ok.

#### Known bugs
- Files added only by imported `.props`/`.targets`, or named through environment variables or
  `$(Configuration)`/`$(Platform)`, are missing from the tree (listed in `SolutionModel::warnings()`).
- The `.slnx` default platform mapping is a heuristic (x86 to Win32; Any CPU to x64 or the first platform).
- Two projects with the same GUID: GUID lookups return the first.
- ProjectReferences to projects outside the solution are listed only in `ProjectInfo::externalReferences`, not in
  the tree.

#### Left
- The items in the M1 entry's "Deferred to the hardening phase (ADR 0031)", marked as `w1/project-sln`'s.
- Its question (the SDK's environment variables and imported `.props`) is under the M1 entry's Questions for the
  user; the orchestrator's decision on the trust file location is in the merge section above.

### Slice record: `m1/display-map` (ADR 0026)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): it had no commit of its own before the PC restart, then one commit, 877893b, on
2fef6ee. Worktree `.claude\worktrees\m1-display-map`. (Merge note: merged into `main` as b36c089 and pushed; the M1
entry above.)

#### Done
- **frost::text::DisplayMap** (ADR 0026): four layers on SumTree in their final order. Identity fold and inlay
  layers with their final query interfaces for M15; soft wrap off, by column or by width, with greedy breaks, wide
  characters, ZWJ sequences and combining marks kept whole, and tabs measured from the row start; block
  decorations above or below a line, anchored in a private AnchorSet so they follow edits and undo.
- **Incremental updates:** `apply(ChangeSet, Snapshot)` re-wraps only the edited lines and returns a sorted
  display-row patch.
- **The ITextShaper seam** with the deterministic MonospaceTextShaper double.
- **The API for editor-model and editor-view:** `toDisplayPoint`, `toOffset` and `toPoint` with Bias; `rowInfo` and
  DisplayRowCursor for painting; `xAt`; cluster-aware `hitTest`; `moveVertically` with a goal x that skips block
  rows; `checkInvariants`.
- **Tests:** 25 functional tests in `text_tests` (about 0.2 s).
- **Verification at 877893b** (its own `-Quick`): win 855/856 (1 environment skip), wsl 769/769.

#### Known bugs
- `reset(Snapshot)` drops all block decorations; callers must re-add them.
- The width tables are approximate, hand-written UCD ranges.
- `setWrap` and `setShaper` re-wrap synchronously and return a whole-map RowEdit (slow on very large files with
  wrap on).

#### Left
- The items in the M1 entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/display-map`'s.
- No questions for the user.

### Slice record: `m1/keymap-de` (ADR 0030)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): saved as the WIP commit a2b2570 before the PC restart, then finished by a lean agent
as one commit, 8f80e22, on 0110eff. Worktree `.claude\worktrees\m1-keymap-de`. (Merge note: merged into `main` as
9ca6a04 and pushed; the M1 entry above.)

#### Done
- **Every default shortcut** of the built-in IntelliJ-compatible keymap is reachable on de-DE, on Windows and on
  Linux under its M1 mapping (the user's decision "Support german keyboard layout", ADR 0016).
- **Layout rule 3.6** (positional preference at punctuation, `src/input` `layout.hpp`) makes the '-' key right of
  '.' the '/' shortcut key on de-DE: Comment Line is Ctrl+-, Comment Block Ctrl+Shift+-, and Fold Collapse moves to
  Ctrl+ß and Ctrl+Num-. This matches JetBrains' documented Linux behaviour; Windows and Linux now agree.
- **Synthesized key events** without scan codes match the hardware key for punctuation (`src/ui/input`
  `key_translation.cpp`).
- **Tests:** KeymapReachability (211 actions, 223 sequences; 0 gaps on de-DE and en-US; pl-PL and fr-FR checked,
  no gaps), GermanDailyShortcuts, PositionalPreference with a 10k-seed property test. The old unreachable-binding
  assertions moved to synthetic tables without a '/' key (moved, not weakened).
- **ADR 0030** also records the user's by-name approval of libxkbcommon and xkeyboard-config (MIT plus HPND-style
  notices; HPND is not on the general allowlist). Nothing is linked or shipped yet.
- **Verification at 8f80e22** (its own `-Quick`): win debug 763/764 (1 skipped:
  `LayoutOs.UsLayoutEqualsTheSimulatedTable`, the active layout was not en-US), wsl debug 677/677.

#### Known bugs
- Find Action parses shortcut queries with US key names: on de-DE "ctrl+-" finds Fold Collapse while the menu shows
  Comment Line as Ctrl+-, and "ctrl+ß" does not parse (the fix goes into `m1/shell-nav`: search the display text;
  Next).
- A behaviour change on Windows de-DE: Ctrl+- was Fold Collapse and is now Comment Line.
- Fold Expand (Ctrl+´), Quick Switch Scheme (Ctrl+^) and VCS Quick Popup (Alt+^) sit on dead keys on de-DE and may
  compose on a real keyboard; Ctrl+Num+ is the Fold Expand alternative.

#### Left
- The items in the M1 entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/keymap-de`'s.
- Its two questions (Build Solution's shortcut, the German Comment Line) are under the M1 entry's Questions for the
  user.

### Slice record: `m1/encodings` (ADR 0018)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. Built as 22cbc80 on 6fb6826
(the piece-tree head) and reviewed twice; the review fixes were saved as the WIP commit 0893943 before the PC
restart, then finished in the feature-first phase (ADR 0031) as ab77994, which replaces the WIP on the branch and
adds `EncodingFiles.VisualStudioSourcesWithProductionDefaults`.
Worktree `.claude\worktrees\m1-encodings`. (Merge note: merged into `main` as 6455c2b and pushed; the M1 entry above.)

#### Done
- **The load/save codec pipeline** (frost::text, ADR 0018): UTF-8 with or without BOM, UTF-16 and UTF-32 in both
  byte orders with or without BOM, and 16 Windows code pages behind `ITextCodec` (MultiByteToWideChar on Windows,
  glibc iconv on Linux; no new dependency).
- **Detection** with a confidence, candidates and an override ("Reload in"), and "Convert to".
- **Line separators:** EOL statistics and the dominant separator; existing breaks are never rewritten on save.
- **Fidelity:** an exact fidelity record per load; lossy loads are guarded on save; `formatStatus` for the status
  bar.
- **Visual Studio sources:** VS-style C++ sources (UTF-8 with and without BOM, windows-1252, CRLF) open losslessly
  and save byte-exact, also after an edit (`EncodingFiles.VisualStudioSourcesWithProductionDefaults`).
- **Review fixes (ab77994):** decoded documents keep at most 1/16 + 64 KiB spare capacity; codec OS failures report
  `Internal`; only a really missing converter skips a code page; each page opens once; IANA aliases for 437 and 850.
- **Verification at ab77994** (its own `-Quick`): win 585/588 (1 skipped; 2 failures,
  `FrostProcess.RunsFromPathLongerThanMaxPath` and `FrostProcess.PlatformPluginPathBoundary`, which exist at its
  base 6fb6826 and are fixed on `main` by the E1 launcher, ADR 0012), wsl 553/553.

#### Left
- The items in the M1 entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/encodings`'.
- No questions for the user; the orchestrator's decisions on the slice's questions are in the merge section above.

### Slice record: `m1/io-reactor-watch` (ADR 0020)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A lean slice of the
feature-first phase (ADR 0031): saved as the WIP commit e7daf37 before the PC restart, then finished by a lean agent
as one commit, e0eaf75, on 0110eff. Worktree `.claude\worktrees\m1-io-reactor-watch`. (Merge note: merged into
`main` as 4cce4c8 and pushed; the M1 entry above.)

#### Done
- **The asio reactor** (vcpkg asio 1.32.0, BSL-1.0, header-only, PRIVATE to frost_io): IOCP on Windows, epoll on
  Linux, run by 1 or 2 threads; completions resume coroutines through frost::sched; an ordered shutdown. ws2_32 is
  delay-loaded and WSAStartup is never called.
- **IFileWatcher.** Windows: ReadDirectoryChangesExW, then ReadDirectoryChangesW, then polling, with 8.3 expansion
  and Rescan on overflow. Linux: one shared inotify instance with watch and instance limit detection, and a
  fallback to polling on network file systems. Both: coalescing, debouncing, rename pairing, an atomic save
  reported as Modified, the temp-file filter, exclusions and non-recursive watches.
- **MemFsWatcher** on SimExecutor's clock, the deterministic double.
- **The io `TODO(M1)`s:** the 8.3 working-directory fallback for launches from long working directories, and
  English strerror text.
- **Landed early** (the spec's Phase B, finished in the WIP): async child pipes (`IProcessLauncher::launchAsync`,
  IAsyncProcess), a clean-room gitignore matcher checked against git, and `scanTree` (the gitignore-aware parallel
  scan) for `m1/shell-nav`.
- **Tests:** `io_unit_tests` and `io_integration_tests`; stress, perf and fuzz targets are written but left to the
  hardening phase.
- **Verification at e0eaf75:** win debug 919/924 (5 skipped), wsl debug 830/835 (5 skipped), format, frost_lint,
  licensecheck and featurecheck ok.

#### Left
- The items in the M1 entry's "Deferred to the hardening phase (ADR 0031)", marked as `m1/io-reactor-watch`'s.
- No questions for the user; the orchestrator's decisions on the slice's questions are in the merge section above.

### Slice record: `m1/storage` (ADR 0014)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A slice reviewed under the
old full process, before the switch (ADR 0031, Consequences): build, two independent reviews, fixes (4472c64), an
independent re-review of the fix commit (approve; weakening: no), and the re-review's one low item documented
(39837c1). Worktree `.claude\worktrees\m1-storage`, branch from `m0/foundation` (593e15f). (Merge note: merged into
`main` as e9c3214 and pushed; the M1 entry above.)

#### Done
- **A new `src/storage` layer** (frost::storage, FrostLayers rank 4, Qt-free, depends on io): SQLite 3.54.1 as an
  overlay port (blessing) and xxHash 0.8.4 (BSD-2-Clause), both linked PRIVATE.
- **Database, Statement and Transaction:** a Result API over SQLite in WAL mode with run-time build checks, hardened
  connections (DEFENSIVE, no DQS, untrusted schema, no extensions), a steady-deadline busy handler with interrupts,
  a WAL hook that never fails a commit, the win32-longpath VFS on Windows and one SQLite-to-ErrorCode table.
- **Stores:** schema versioning (application_id plus user_version, one transaction per migration), `openStore` with
  quick_check and quarantine of corrupt files (byte-identical copies under SQLite's EXCLUSIVE lock), removal in the
  safe order, and close within B20's 200 ms.
- **The hot-exit journal** (one EXCLUSIVE-locked journal per process, whole-entry puts in chunks with XXH3-128
  checksums, clean marks, a recovery scan that skips live journals and offers damaged ones for salvage) and **local
  history** (content-addressed revisions with byte-verified dedup, retention by age and size).
- **Fingerprints:** XXH3-64 and XXH3-128, canonical big-endian form and hex.
- **The fixes from the two reviews** (4472c64): POSIX quarantine and removal no longer act by name on a file another
  instance replaced while they waited (`SQLITE_FCNTL_HAS_MOVED`); `Transaction::rollback` never finishes while
  autocommit is 0; the history crash oracle compares content; Windows removal is deadline-bounded through
  `io::removeFile`. The re-review's one low was documentation: 39837c1 states the residual POSIX removal race
  accurately.
- **Tests** (`tests/storage`): unit (wrapper, schema and goldens, quarantine, journal, local history, a retention
  oracle, a fault-injecting VFS), integration (crash consistency by killing a child through IProcessLauncher,
  concurrency) and stress. Rows 218 and I2 of `tests/features/matrix.json` are `in-progress`.
- **Verification at 4472c64** (39837c1 changes comments and docs only): win debug 530/533 (1 skipped, plus the two
  FrostProcess long-path failures known on its pre-M0 base), wsl debug 504/504, storage win asan 189/189, wsl asan
  194/194, wsl tsan 194/194; lint, format, licence and featurecheck ok; with the HAS_MOVED check disabled and a
  single ROLLBACK attempt, the 5 tests aimed at them fail (mutation check); perf-smoke not run, coverage not
  measured. (Merge note: those failures and the skip no longer apply on `main`; the M1 entry above.)

#### Left
- The low item and the tooling gaps in the M1 entry's "Deferred to the hardening phase (ADR 0031)".
- The six questions in the M1 entry's "Questions for the user", marked as `m1/storage`'s.

### Slice record: `m1/theme-icons` (ADR 0015)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A standard-tier slice under
the old full process: design, critic, build, two independent reviews, fixes. The fix agent was cut off by the usage
limit, and a finisher ran the final verification and committed the fixes (73a5143). Worktree
`.claude\worktrees\m1-theme-icons`, branch from `m0/foundation` (593e15f). (Merge note: merged into `main` as
94ed47d and pushed; the M1 entry above.)

#### Done
- **A new `src/ui` layer** (frost::ui, Qt allowed), with the Qt-free frost::ui_model and frost::ui_theme_compiler
  (Qt Core) beside it.
- **FrostStyle**, a QProxyStyle over Fusion. The palette is applied in `polish` and through
  `QApplication::setPalette`, so OS palettes cannot leak in.
- **Themes:** a build-time `.theme.json` to blob compiler (`frost-themec`; no JSON parsing at start-up, a bounded
  duplicate-key pre-scan). Glacier Dark, Light, Classic and High Contrast, plus Sync with OS on Windows and Linux;
  when the chosen theme fails to load, glacier-dark is the fallback. Contrast tests: the island rule 1.20:1, and
  WCAG for High Contrast and for check-box borders.
- **Icons:** `frost-icon-baker` bakes Lucide SVGs (ISC) into atlases at 7 scales; Qt Svg is used only in the baker
  (frost-lint's `qt-svg` rule).
- **Fonts:** JetBrains Mono 2.304 and Inter 4.1 (OFL-1.1), pinned by SHA-256 in `resources/assets.lock.json` and
  checked by licensecheck's asset gate.
- **Compact mode and IDE zoom**, and offscreen screenshot goldens in both main themes at 100, 125, 150 and 175%;
  all match with 0 difference today.
- **Verification at 73a5143** (`verify -Full -Jobs 6`): win debug 502/505, release 505/508, asan 506/509 (each only
  the two FrostProcess long-path failures known on its pre-M0 base, plus 1 skip), wsl debug 467/467, asan 471/471,
  tsan 470/470 (the Qt widget tests 64/64, 0 TSan reports); lint, format, licence and featurecheck ok. (Merge note:
  those failures and the skip no longer apply on `main`; the M1 entry above.)

#### Left
- Five low items, recorded in ADR 0015: List and Tree selection contrast compositing, reserved theme keys, and a
  note on the 14 px design size.
- The nine questions in the M1 entry's "Questions for the user", marked as `m1/theme-icons`'s.

### Slice record: `m1/undo-anchors` (ADR 0017)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A core-tier slice: design,
critic, build, two independent reviews, fixes, then an independent re-review of the fix commit (approve; weakening:
no). Worktree `.claude\worktrees\m1-undo-anchors`, branch from `m1/piece-tree` (6fb6826). (Merge note: merged into
`main` as 3163d0b and pushed; the M1 entry above.)

#### Done
- **frost::text Document** (`src/text`, 81afd3a): biased anchors that resolve against later snapshots;
  SelectionSet (multi-caret, normalised and merged, with an origins map); and a transactional undo tree: groups,
  typing coalescing by time and kind, redo branches, IME preedit kept out of history, ChangeSets with a base
  version, `UndoState.stateId` for the dirty flag, and `committedText()`.
- **Review fixes** (7ae2ab5): coalesced delete runs keep whole pieces, so the history memory per deleting keystroke
  is no longer about 90 times that of typing; AnchorSet's path thresholds use measured factors; pruned redo branches
  are freed later rather than in the keystroke that prunes them; and the restore scan's finds no longer allocate.
- **Verification at 7ae2ab5:** `verify -Quick` win debug 513/516 (the two FrostProcess long-path failures then known
  on its pre-M0 base, and 1 skip), wsl debug 482/482; Windows Release text 139/139, stress 6/6, perf 3/3 twice with
  B5 asserted (typing p99 48.2 and 42.8 us, undo/redo p99 100.1 and 54.4 us); Windows ASan, WSL ASan+UBSan and WSL
  TSan 151/151 each on the text tests. (Merge note: those failures and the skip no longer apply on `main`; the M1
  entry above.)

#### Left
- Six low items, recorded in ADR 0017.
- The four questions in the M1 entry's "Questions for the user", marked as `m1/undo-anchors`'s.

### Slice record: `m1/input-keymap` (ADR 0016)

The slice kept no record in this log; this is its hand-off note, recorded at the merge. A standard-tier slice under
the old full process: design, critic, build, two independent reviews, fixes. Worktree
`.claude\worktrees\m1-input-keymap`, branch from `m0/foundation` (593e15f). (Merge note: merged into `main` as
3db37c5 and pushed; the M1 entry above.)

#### Done
- **frost::input** (`src/input`, layer rank 4, Qt-free): the shortcut matcher (chords, two-stroke sequences, AltGr
  disambiguation from Qt's per-side `nativeModifiers` bits on Windows, dead keys, double Shift suppressed during
  IME composition, autorepeat after a swallowed sequence); simulated de-DE, pl-PL and fr-FR (KBDFR, the legacy
  AZERTY) layouts for the M1 exit's shortcut tests, so no layout is ever switched on the machine; the
  IntelliJ-compatible keymap as our own data (shortcut facts from JetBrains' public help pages and reference cards
  only; no JetBrains files or code); the action registry and the Find Action model.
- **frost::ui_input** (`src/ui/input`): the thin QKeyEvent adapter, with a scan-code fallback for synthesized
  events.
- **Verification at c78db87:** win debug 634/637 (the two FrostProcess long-path failures then known on its pre-M0
  base, and 1 skip), wsl debug 596/596, win asan (slice tests) 270/270, wsl asan+ubsan and tsan (slice tests)
  263/263 each. (Merge note: those failures and the skip no longer apply on `main`; the M1 entry above.)
- **Release numbers** (REF-W1): Find Action 0.42 ms (bound 2 ms); the B5 matcher feed 17 ns per event (bound 2 us).

#### Left
- Six low items (the slice's count), recorded in ADR 0016: its Consequences and known approximations, and the
  `TODO(M1)`, `TODO(M2)` and `TODO(M11)` markers in `src/input` and `tests/input`.
- The six questions above, marked `m1/input-keymap`.

### Slice record: `m1/piece-tree`, acting on two independent reviews

Two independent reviews of c8757f3: one approves, one asks for changes. Neither found a correctness,
memory-safety or concurrency defect in `src/text`. Together they report one medium and seven low findings, all in
test tooling, docs and the clean-room record. Every finding is fixed or recorded; one question for you is below.
Committed on `m1/piece-tree`, not merged or pushed. Every Windows test ran through `run_hidden_nobox.py`; no
window opened on your desktop. (Merge note: merged into `main` and pushed; the M1 entry above.)

#### Done
- **Medium, the stress test's reproduction command.** `TextStress.Differential` runs 64 queries per full check,
  but the command it printed re-ran the seed through `TextDifferential.Quick` with its fixed 16. The verify RNG
  draws differently with 16 queries, so a failure found by the later draws could pass under the printed command.
  `text_ops` now has `quickSettings`, which parses `FROST_TEXT_SEED`, `_OPS`, `_CHECK_EVERY`, the new
  `FROST_TEXT_QUERIES` and `_THREADS` and rejects bad values (InvalidArgument; zero threads too, which used to run
  no seed), and `reproduceCommand(SeedRun)`, which sets every SeedRun field. Both tests print it. New tests:
  `TextDifferential.ReproduceCommandRoundTrips` and `TextDifferential.SettingsDefaultsAndBadValues`.
- **Low, the buffer limit was never fuzzed.** One differential seed in seven (`seedBufferLimit`, `seed % 7 == 3`)
  and 30 of the 256 values of a fuzz input's first byte now run with a small `maxBufferBytes` (`limitBuffer`).
  Edits, batches, loads and the invalid op's near-limit branch then meet check 5. `DriverStats::overLimit` counts
  these rejections, `TextDifferential.Quick` expects some, and the stress test prints the count. No committed
  corpus file changes meaning: none has a first byte in the new range. The first mapping, `(seed / 24) % 8 == 7`,
  chose no seed in the default-scale stress runs (seeds 1,000,000-1,000,049; the first `-Full` printed
  "0 rejections by the buffer limit"). Any 7 consecutive seeds include one of the new mapping's seeds.
- **Low, the fuzz smoke was not hermetic.** The new setup fixture `fuzz.<name>.reset`
  (`cmake/FrostFuzzResetWork.cmake`) empties `fuzz-work/<name>` before each run, so every run starts from the
  committed corpus. Fuzz trees now count two tests (`win fuzz 2/2`, `wsl fuzz 2/2`).
- **Low, docs:** `snapshot.hpp` and ADR 0013 D7 now say that `chunks()` and `linesFrom()` copy the snapshot (four
  counter operations per call). The ADR's tree height for 256 MiB is now 4, not 3: 16^3 = 4096 leaves is the most
  height 3 holds, and `TextPerf.LoadThroughput` now prints the height. D8 now says where the LSP-replay property is
  checked: at each full check, not after every op. Two comment fixes: `mergeSiblings` holds "at least B", and the
  harness takes bytes `[2, 2+L)`.
- **Low, clean-room record:** `text_summary.hpp` and ADR 0013's Sources now record where TextSummary's
  line-metric design came from, and the review's note that the line fields resemble those of Zed's GPL rope
  crate. That crate was not opened. See the question below.
- **Mutation checks** (Debug, each reverted afterwards; `git diff src/text/src` is empty). Each mutant made the new
  tests fail:
  - (A) the command without `FROST_TEXT_QUERIES`, and (B) Quick ignoring that variable: the round-trip test fails.
  - Removing the size check from (C) `applyEdits`, (D) `replace` or (E) `fromUtf8`: Quick fails 3, 29 and 12
    seeds, the first being seed 3, 3 and 10. All three were also killed under the first mapping, where the
    printed command reproduced C's first failing seed (171) on its own.
  - (F) no limited seeds: Quick fails its `overLimit > 0` expectation.
  The reviewer's surviving mutant was C. These runs used the final mapping: Quick had 486 limit rejections, and
  the 1M-op stress run in Release had 2218.
- **Verification** (`run_hidden_nobox.py`, `-Jobs 4`, `night.py build-gate` before every build and long run):
  - `verify.ps1 -Full`, twice: first on the first mapping (21 min 49 s), then on the final code (20 min 26 s).
    Both gave win debug 435/438 (1 skipped), release 441/444 (1 skipped), asan 442/445 (1 skipped), fuzz 2/2; wsl
    debug 404/404, asan 411/411, tsan 410/410, fuzz 2/2. format, frost_lint (348 files), licensecheck and
    featurecheck were ok. The only failures are the known `FrostProcess.RunsFromPathLongerThanMaxPath` and
    `FrostProcess.PlatformPluginPathBoundary`; the skip is `FrostProcess.ReportsLongPathLimitCleanly`.
  - The final `verify.ps1 -Quick`, after the long runs and the docs: win debug 435/438 (1 skipped; the same two
    known failures), wsl debug 404/404, all checks ok, 105 s wall. Only this log entry changed after it.
  - Every text stress case stays under its 120 s ceiling. The longest are WSL ASan Differential at 55.6 s, WSL
    TSan Differential at 48.9 s and Windows ASan Differential at 31.2 s. `TextDifferential.Quick` takes 6.0 s in
    Windows Debug and 5.7 s in WSL Debug.
  - (Merge note: the known failures and the skip do not apply on `main`, which has `m0/longpath`; the M1 entry
    above.)
- **M1 exit runs, repeated on the final code** (run beside each other; ADR 0013 has the details):
  - The 10M-op differential in Windows Release: **0 mismatches**, 1,126,249 full checks, 21,367 rejections by the
    buffer limit, 161.4 s.
  - A **clean 10-minute fuzz on each OS**, each starting from an empty work corpus. Windows: 381289 runs,
    634 exec/s. Linux: 386400 runs, 642 exec/s. No artifacts on either.
- `TextPerf.LoadThroughput` at 256 MiB now prints height 4 (the ADR's corrected value).

#### Next
- Merge `m1/piece-tree` once M0 is signed off and you have answered the provenance question (CLAUDE.md: text,
  core and io had their independent review; these fixes touch only comments in `src/text`). (Merge note: merged
  after M0's sign-off with the question still open, on the basis of option (a); the M1 entry above.)
- Then `m1/undo-anchors`, `m1/encodings`, `m1/large-file` and the editor model, as in the record below.

#### Blockers
- None new. The two known FrostProcess failures still wait for `m0/longpath`. (Merge note: `m0/longpath` is on
  `main`; they no longer apply.)

#### Questions for the user
- **TextSummary provenance (clean room).** The second review noted that TextSummary's line metrics
  (`firstLineChars`, `lastLineChars`, `lastLineUtf16`, `longestLineChars`, `longestLine`) and the non-CRLF combine
  rule match, field for field, the `TextSummary` of Zed's rope crate. That crate is GPL, and PLAN.md section 3
  lists it as off-limits. The review found no evidence of copying, and found the CR, CRLF and lone-CR rules,
  `lineFeeds`, per-byte counting and tie-breaking original. The slice's source record says the crate was not
  opened. The record now states this in ADR 0013 and in a provenance comment in `text_summary.hpp`. Is that record
  enough, or do you want something else? For example, a redesign of the line fields, or a citation of a public
  description such as Zed's "Rope & SumTree" blog post, which this session has not read. (Merge note: asked again,
  with options, in the M1 entry above.)

### Slice record: M1 first slice, the frost::text piece tree (`m1/piece-tree`)

M1 started while M0 waits for sign-off (your instruction on 2026-11-01). Branch `m1/piece-tree` from
`m0/foundation` (593e15f), worktree `.claude\worktrees\m1-piece-tree`; committed, not merged or pushed. Design,
sources and measurements: ADR 0013. Every Windows test ran through `run_hidden_nobox.py` (a private desktop with
error boxes off); no window opened on your desktop.

#### Done
- **frost::text** (`src/text`, layer rank 4): the generic persistent `SumTree<Traits, B>` (ref-counted B+tree,
  B = 8), the piece tree over append-only UTF-8 storage, `TextSummary` (64 B), O(1) lock-free `Snapshot`s,
  `TextBuffer` with insert/erase/replace, atomic `applyEdits` and `restore`, each returning `TextChange`s normalised
  to line-break boundaries (D8), conversions between byte offsets, `Point`, `Utf16Point` and UTF-16 offsets with
  documented round-trip rules, chunk and line iteration, `summaryOf`, `checkInvariants`, and stateless typing
  coalescing. Nothing from the spec's cut list was cut.
- **Core and io:** `Utf16Column` in `strong_type.hpp`; `io::validUtf8Prefix`, now behind `isValidUtf8`, tested
  against an independent encoder over every scalar value (`ValidUtf8PrefixMatchesEncoder`).
- **Tests** (`tests/text`): a brute-force oracle with its own bit-pattern UTF-8 decoder and an `OpDriver` shared by
  the differential, stress and fuzz tests; 62 `text_tests` (summary monoid, `SumTree` at B = 2, 3 and 8, 30 edge
  cases, the 200-seed differential, the concurrent-reader test), `text_stress_tests` and `text_perf_tests`. The
  shared test environment (`stressFull`, `kSanitized`, ...) moved to `tests/support/test_env.hpp`.
- **Fuzzing:** `FROST_FUZZ`, `cmake/FrostFuzz.cmake`, presets `win-clangcl-fuzz` (/MT) and `linux-clang-fuzz`, the
  first target `text_piece_tree` with a 30-file corpus, the corpus replay `fuzz_replay.text_piece_tree` (label unit)
  in every normal tree, and the fuzz legs in `verify -Full` on both OSes (60 s per target).
- **Verification** (`run_hidden_nobox.py`, `-Jobs 4`, after `night.py build-gate`):
  - `verify.ps1 -Quick`: win debug 433/436 (1 skipped), wsl debug 402/402, format, frost_lint, licensecheck and
    featurecheck ok. Run twice: 136 s wall (rebuilding the reformatted text sources) and, on the final tree, 132 s
    (rebuilding everything that includes `snapshot.hpp` after a comment change).
  - `verify.ps1 -Full`: win debug 433/436 (1 skipped), release 439/442 (1 skipped), asan 440/443 (1 skipped), fuzz
    1/1; wsl debug 402/402, asan 409/409, tsan 408/408, fuzz 1/1; all checks ok. 27 min 9 s wall, most of it the
    first build of this worktree's ASan trees on both OSes. It ran before the last change, which only adds a comment
    to `snapshot.hpp` (the `TODO(M1)` on releasing big trees) and fills in the docs; the second `-Quick` covers it.
  - The only failures are the known `FrostProcess.RunsFromPathLongerThanMaxPath` and
    `FrostProcess.PlatformPluginPathBoundary` (they fail until `m0/longpath` lands); the skip is
    `FrostProcess.ReportsLongPathLimitCleanly`. (Merge note: `m0/longpath` has landed; the M1 entry above.)
- **Text test times and scales** (ADR 0013 has the table): `TextDifferential.Quick` 8.2 s in Windows Debug (ceiling
  15 s; 4 threads), 6.1 s in WSL Debug; every other unit case at most 0.9 s in Windows Debug. All text unit cases
  together: 11.7 s Windows Debug, 8.3 s WSL Debug. Stress cases in `-Full`, longest first: WSL ASan Differential
  66.7 s (200k ops), Windows ASan Differential 55.8 s (500k), WSL TSan Differential 52.6 s (300k), Windows ASan
  ConcurrentReaders 45.1 s; every case is under its 120 s ceiling.
- **B28 effect:** the text tests add about 12 s (Windows) and 8 s (WSL) to `verify -Quick`'s test steps, inside the
  30 s per OS the slice allows; the test steps now take 58.6 s and 25.2 s. ADR 0010 measured `verify -Quick` at
  p50 82.3 s after a one-.cpp change; this slice's two runs took 136 s and 132 s, each after a change that rebuilt
  the text library and its four test executables on both OSes, so they are not B28 samples. Both stay far inside
  B28's p50 5 min. Re-measuring B28 with ADR 0010's harness is part of its M1 follow-up.
- **Perf** (Windows Release, REF-W1, you at the PC): `TextPerf.EditLatency` p50 9.7 us, p99 22.2 us against the
  0.25 ms text-side sub-budget of B5 (ADR 0013, decision 9; asserted in optimised builds); load 0.98 GB/s, F1
  projection 2.03 s and 39.9 MB of tree nodes (the B15 risk in ADR 0013, decision 6); seek p99 3-16 us.
- **Fuzz throughput** (60 s smoke, ASan): Windows 1484 exec/s on the first run, 940 exec/s in `-Full` (a larger
  work corpus); Linux (ASan + UBSan) 1079 exec/s; target 200. Caps unchanged.
- **M1 exit runs** (after `night.py build-gate`): the 10M-op differential in Release, **0 mismatches on both OSes**
  (Windows 217.5 s through `run_hidden_nobox.py`; WSL 166.1 s in the ext4 mirror), and a **clean 10-minute fuzz on
  each OS** in separate `FROST_FUZZ_SECONDS=600` trees (Windows 541 exec/s, Linux 575 exec/s; no artifacts). Both
  M1 exit criteria for the piece tree are met; the other M1 exit items wait for later slices.
- `tests/features/matrix.json` row 35 (text buffer, large files, undo) is `in-progress`.

#### Next
- An independent review of `m1/piece-tree` (it touches core, io and text) before any merge (CLAUDE.md). (Merge
  note: done, two reviews; the record above.)
- `m1/undo-anchors` (anchors, selections, the undo tree, precise restore changes by node identity), then
  `m1/encodings` and `m1/large-file` (mapped originals, B15 at the buffer level, the background release of big
  trees: `TODO(M1)`), and the editor model (reverse cursors, seek-forward, graphemes).

#### Blockers
- None for this slice. M0 sign-off (G1) and the M0 questions below are still open; `m0/longpath` decides the two
  known FrostProcess failures. (Merge note: M0 is signed off and `m0/longpath` is merged; entry "M0 complete".)

#### Questions for the user
- None new. ADR 0013 records the slice's decisions; none weakens a test, budget or warning.

## 2026-11-01 (afternoon): M0 complete

M0's exit is met. `m0/decisions` and `m0/longpath` are merged into `m0/foundation`, and this exit record follows
them: PLAN.md's M0 status and next-step lines, notes in ADRs 0011 and 0012, and pointers in the entries below. The
launch from a path over 260 characters, the last unmet criterion, is met by ADR 0012, on two decisions by the user.
The sign-off follows ADR 0011's G1: `verify -Full` on both platforms at `m0/foundation`'s final head, then, if it
is green, the annotated tag `m0` with its footer, `main` created from the tagged commit, and both pushed.
`verify -Full` ran green at c87c56a (Verification, below). The commit that records it, "M0: sign-off record"
(docs only), is the one tagged `m0` and the start of `main` (ADR 0011, G1 note). The work was done in the main
worktree, starting from `m0/foundation` at 593e15f, which was clean.

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
- **c87c56a**, the exit record's fixes. A read-only check of 09f428f found 12 docs issues, all fixed in c87c56a,
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
- **The sign-off record** (the commit tagged `m0`): the `-Full` footer of c87c56a here and in the demo pack,
  docs only.
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
- `verify -Full` at `m0/foundation`'s final head (the sign-off): c87c56a, `tools\verify.ps1 -Full -Jobs 8` on a
  hidden desktop, 17:07-17:14, after `night.py build-gate` (no lock, 38.9 GB commit free), exit 0:
  `Verified: win debug 415/415, win release 418/418, win asan 419/419, wsl debug 335/335, wsl asan 339/339, wsl
  tsan 338/338, wsl licensecheck ok, format ok, frost_lint ok, licensecheck ok, featurecheck ok, tidy not-run`.
  I7's six acceptance tests show `[ OK ]` in the Release and ASan legs, none skipped. Evidence:
  `E:\FrostBuilds\perf\m0\signoff\` (`README.txt`, `verify_full.log`, the two ctest logs). The tagged sign-off
  record is c87c56a plus this footer, docs only.

### Next
- The sign-off (ADR 0011, G1), right after this commit: tag it `m0` (annotated, with the `-Full` footer and a
  note that `-Full` ran on its parent, c87c56a), create `main` from it, and push `m0/foundation`, `main`,
  `m0/decisions`, `m0/longpath` and the tag, without a force. Then the demo pack marks rows 15, 16, 18, 21 and
  26 as met.
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
- None for M0: `-Full` is green at the final head (c87c56a).
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
  searches instead (F2, below; done on 2026-11-02). B28 reads the full default build, and its method fixes the probe files and
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

#### F2
- Done (2026-11-02, `m1/verify-tooling`, ADR 0027): the Linux configure's two FindOpenGL `find_path` searches for
  `GL/glu.h` and `GL/xmesa.h` were slow because of the `/mnt` PATH entries; `verify.sh`'s WSL PATH filter removes
  their cost (a Linux re-configure from 30.45 s to 1.35 s).

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

### M4 terminal investigation in `m4/terminal` (unmerged, 2026-11-02)
- Implemented DECSET 9001 negotiation and Windows key records, plus kitty keyboard flags 1 and 4 and
  distinct Shift+Enter. The pinned Microsoft ConPTY package is installed under the SDK, deployed beside
  the executable and loaded by absolute path, with a logged system fallback; NOTICE and the SBOM gate
  record its MIT licence. Windows focused protocol, Qt and bundled-ConPTY tests pass. On Linux ext4,
  protocol tests and the real PTY Shift+Enter test pass.
- The isolated, no-prompt Claude startup stopped at its workspace trust screen; it requested kitty flags
  5 and DECRQM 2026/1016 (ADR 0051). Actual prompt-state and modifier delivery to Claude, full kitty
  enhancement/layout fidelity and real IME delivery are not established. No prompt or trust decision was
  submitted. The branch remains unmerged pending independent review and full verification.
- Astra review found that consumed PTY output could remain allocated indefinitely when the backlog
  never emptied. The buffer now compacts that prefix; a 4 MiB nonempty-backlog test checks content and
  allocated capacity on Windows and Linux. Win32 keyboard records now preserve every UTF-16 unit;
  a Qt test covers multiple units and a real bundled-ConPTY child confirms a surrogate pair arrives.
  The separate current-main snapshot now wires the Workbench, current project trust, shutdown and
  Explorer action, and stages both ConPTY files. Windows targeted checks: app 21/21, workbench 23/23,
  tool windows 32/32, terminal unit 46/46, terminal UI 14/14 and PTY 4/4; stage script tests 6 passed
  and one skipped. The staged executable passed its offscreen private-desktop self-test, and a test
  run from the staged directory observed its bundled DLL and OpenConsole process. Native Linux tests
  cover the terminal protocol and PTY, but the latest application integration has not run on Linux.
  The staged snapshot still needs independent review and the merged-tree verification gate.
