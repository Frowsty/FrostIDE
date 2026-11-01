# ADR 0012: frost.exe launcher and frost_app.dll; start from any path length

- Status: accepted (the user's decision of 2026-11-01, quoted below)
- Date: 2026-11-01
- Plan reference: PLAN.md section 7, M0 exit ("launch from a path with non-ASCII characters, spaces and >260
  characters"); PLAN.md section 3.5 (start-up hardening); inventory I7; questions E1, E4 and E5 in
  `docs/progress.md`; supersedes the long-path consequence of ADR 0003

## Context
The Windows loader refuses a plain path longer than 250 UTF-16 units for any DLL that embeds an isolation-aware
manifest (RT_MANIFEST resource 2), with ERROR_FILENAME_EXCED_RANGE (206, NTSTATUS 0xC0000106). Every Qt DLL and
plugin embeds one, and so does clang's ASan runtime. `LongPathsEnabled = 1` does not move the limit (measured with
the switch on, probe fact 1); the `\\?\` form of the same path loads at any length (fact 2). A DLL's static imports
load in the form of the directory the loader found them in (fact 7). Evidence:
`E:\FrostBuilds\perf\m0\longpaths-on\probe\README.txt` (facts 1-32) and
`E:\FrostBuilds\perf\m0\longpaths-on\launcher-spike\README.txt` (facts 1-22).

Before this ADR, frost.exe imported Qt statically. On a plain start (frost::io and Explorer start it that way below
MAX_PATH), the loader killed it before `main` with 0xC0000106 and no message, from directories of 236-249 units in
Release, 235-249 in Debug and 218-249 in ASan trees (fact 16; ADR 0003). Between and beyond those ranges a start-up
check refused plugin paths over 250 units, because Qt strips `\\?\` and loads plugins through their plain paths
(facts 9-12). So the M0 exit criterion was not met (question E1), its test skipped while the switch was off, and
`FrostProcess.PlatformPluginPathBoundary` failed in the ASan tree (question E4).

Probe 7 found the way through: once a plugin is loaded by its `\\?\` path, Qt's later plain-path load of the same file
returns that module, at any length (facts 26-27). It also holds with long paths ineffective for the process (spike
fact 19). What was missing was a frost.exe with no static import that embeds RT_MANIFEST 2.

The options put to the user were "Approve stripping" (deploy Qt's DLLs and the ASan runtime without RT_MANIFEST 2,
probe 10 and branch `m0/e4-asan-runtime`), "Use the launcher design" and "Neither for now". The user's decision, on
2026-11-01 at about 11:00, was **"Use the launcher design"**, described to them as: *"Keep Qt's DLLs untouched;
frost.exe becomes a small launcher that loads frost_app.dll via \\?\. More work and still needs its own spike."* The
spike ran the same morning (`launcher-spike\README.txt`); its corrections to the first design (design A and
section 12 of its review) are part of the decision below.

## Decision
1. **Two images on Windows.** frost.exe is the launcher (`src/app/src/launcher.cpp`); the application is
   frost_app.dll (target `frost_app_shared`, output name `frost_app`, import library `frost_app_shared.lib` so it
   cannot clash with the static `frost_app.lib`), which exports `int frost_main(unsigned hardeningBits)`
   (`src/app/src/main.cpp`). On Linux frost stays one executable.
2. **The launcher** is built with the static release CRT in every configuration (`MSVC_RUNTIME_LIBRARY
   MultiThreaded`), without sanitizers (`frost_add_executable(... NO_SANITIZE)`), with `/ENTRY:wmainCRTStartup`,
   the GUI subsystem, `/MANIFEST:NO` (frost.rc embeds the process manifest as RT_MANIFEST 1) and
   `/DEPENDENTLOADFLAG:0xA00`. Its only static import is KERNEL32.dll, in every tree, ASan included. It does not use
   frost_app.lib's or frost_core.lib's objects: the Qt-free start-up core (`src/app/src/startup_core.cpp`) is
   compiled twice, `/MT` into the launcher and `/MD` into frost::app, because lld-link's `/failifmismatch` forbids
   mixing them.
3. **Launcher sequence** (`wmain`):
   1. `hardenProcess()` while only ntdll, kernel32 and kernelbase are mapped;
   2. `AddDllDirectory(\\?\<install dir>)`, then `SetDefaultDllDirectories(USER_DIRS | SYSTEM32)`
      (`searchInstallDirectoryExtended()`), always, whatever the length. If AddDllDirectory fails, the default stays
      DEFAULT_DIRS (fail closed), and the self-test's process-hardening check, which carries the step as
      `ProcessHardening::installDirectoryExtended`, fails;
   3. in ASan trees only (compile definition `FROST_LAUNCHER_PRELOAD_ASAN_RUNTIME`), load
      `\\?\<dir>\clang_rt.asan_dynamic-x86_64.dll` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | _SYSTEM32` (0x900), so
      ASan initialises before the UCRT and Qt (spike fact 2);
   4. load `\\?\<dir>\frost_app.dll` with 0x900, resolve `frost_main` and return its result. frost_app.dll has no
      `/DEPENDENTLOADFLAG` (a value would override the launcher's flags for its imports; 0xA00 there broke plain
      starts from 236 units, spike fact 7) and is linked with `/MANIFEST:NO` (CMake's `vs_link_dll` would embed an
      RT_MANIFEST 2, fact 14).
   If a step cannot load its DLL, the launcher writes "frost: cannot load <path>: <reason> (win32 error N)" to
   stderr, or else to the parent's console (AttachConsole and CONOUT$, WriteConsoleW), shows a MessageBoxW (user32
   loaded from System32 only then) only in GUI mode (no argument starting with '-'), and exits with 4.
4. **In frost_app.dll**, start-up runs in this order: arguments are parsed first; command-line modes (usage errors,
   --version, --help, --selftest) then apply `suppressCrashDialogs()`, the dialog guard (`guardCommandLineDialogs()`,
   a WH_CBT hook that refuses `#32770` windows on the main thread, so Qt's Release "no Qt platform plugin" box returns
   at once) and the fatal handler (`installCommandLineFatalHandler()`: a Qt fatal error prints "frost: Qt fatal
   error: ..." and exits with 4). `--version` and `--help` behave as before: same output, exit codes and console
   handling, Qt-free apart from `qVersion()`, no delay-load (it is impossible as linked, fact 31).
   `preparePlatformPlugin()` pins Qt's platform plugin through its `\\?\` path with `USER_DIRS | SYSTEM32` before
   QApplication exists, in every mode. It is required: a missing or unloadable plugin fails the self-test (exit 1)
   or the GUI start (message, exit 4). `preloadOtherPlugins()` does the same for the other deployed plugins when
   the longest plugin path passes the loader's limit (`dir + 1 + longest > 250`), best-effort, with failures in the
   self-test's `other-plugins` check.
5. **Self-test checks** (Windows): `process-hardening` (now including step 3.2), `dependent-load-flags` (0xa00 for
   frost.exe, 0 or 0x900 for frost_app.dll), `long-paths-effective` (a warning, reworded to "not effective for this
   process (needs LongPathsEnabled=1 and longPathAware); FrostIDE starts either way"), `image-path` (information:
   the form and length frost.exe was started through), `load-form` (frost_app.dll and Qt6Core loaded through `\\?\`
   paths), `dialog-guard`, `plugin-paths`, `other-plugins` and `late-load` (a DLL beside frost.exe that nothing
   loaded, loaded later by bare name).
6. **Build system.** `FROST_SANITIZE`'s flags are applied per target by `frost_apply_compile_options` instead of
   globally, so the launcher can opt out. `frost_add_library(... SHARED)` exists, and the per-directory runtime-DLL
   copy (`frost_copy_runtime_dlls`) treats shared libraries as members of their directory: ordered after its copy,
   their imported DLLs in it. The ASan tree's Debug configuration does not build (clang-cl refuses `/MDd` with
   ASan); its RelWithDebInfo and Release do. If UBSan is wanted on Windows, it must come with ASan or in trap mode:
   its standalone runtime is static-only (spike fact 4).
7. **No deployed DLL is modified.**

### Removed and relaxed tests, and their replacements
| Before | After | Why it asserts at least as much |
|---|---|---|
| `FrostProcess.ReportsLongPathLimitCleanly` (skipped here; with LongPathsEnabled=0, frost must refuse a long path with a message) | `FrostProcess.StartsWithLongPathsIneffective`, `StartsFromEveryDirectoryLength` | The refusal no longer exists. With long paths ineffective for frost (frost.exe's manifest copy without longPathAware, at a short path), frost must start and pass every check except the edited manifest's, in 13 cells up to 400 units. |
| `FrostProcess.PlatformPluginPathBoundary` (250 units loads, 251 refused; failed in ASan) | `FrostProcess.StartsFromEveryDirectoryLength`; `StartupPluginPreload.NeededOnlyBeyondTheLoadersPlainPathLimit`; `StartupLoadPlatformPlugin.PinnedPluginSatisfiesQtsPlainPathLoad` | 22 lengths from 200 to 400 (249, 250 and 251 among them), plain and `\\?\`, must pass within 30 s each and report the form they started through; the 250/251 rule survives as the preload's gate; the pin is checked at 300 units. |
| `FrostProcess.RunsFromPathLongerThanMaxPath` (skipped while LongPathsEnabled=0) | the same test without the skip, with a plain start added | Runs everywhere. A plain start needs the parent's long paths; without them CreateProcessW's refusal is expected. |
| `FrostProcess.RunsThroughShortNamesOfALongPath` (skipped when the short form exceeded 250 units) | the same test, skipping only where no 8.3 names exist | Expects the pin's report ("loaded Qt's platform plugin", "~1\platforms\"). |
| `FrostProcess.SelfTestPasses`: "dependent-load-flags: load configuration DependentLoadFlags = 0xa00" | "DependentLoadFlags frost.exe 0xa00, frost_app.dll 0x0", plus `load-form`, `dialog-guard`, `plugin-paths` and `late-load` | Both images are read; more checks. |
| `StartupPluginPaths.AcceptsThePlatformPluginUpToTheLimit`, `RejectsOneUnitBeyondTheLimitAndNamesTheShortestCandidate`, `IgnoresLengthWhenLongPathsAreEffective`, `CountsUtf16UnitsAndReportsUtf8`, `StartupPlatformPlugin.PresenceNotRequiredButLengthStillChecked` (the length rule of the removed `evaluatePluginPaths`) | `StartupPluginPreload.NeededOnlyBeyondTheLoadersPlainPathLimit`; `FrostProcess.StartsFromEveryDirectoryLength` (UTF-16 lengths with a non-ASCII first component); `StartupPluginPaths.PresenceCheckDefersToQtPluginPath` | frost no longer refuses long plugin paths; the remaining rule (when to preload) is pinned at 250/251, and the real start is pinned at every length. |
| `StartupPluginPaths.OneCandidateWithinTheLimitIsEnough` | `StartupPlatformPlugin.CandidatesFollowQtsOrder`; `StartupLoadPlatformPlugin.SkipsMissingCandidates` | Qt's fallback order (`direct2d;windows`) and that a later candidate is used when an earlier one is absent. |
| `StartupPluginPaths.OtherPluginsBeyondTheLimitOnlyWarn` | `FrostProcess.PreloadsOtherPluginsFromALongPath` | Long non-platform plugins are now loaded (qsvg with its Qt6Svg dependency at 300 units), not only warned about. |
| `StartupPluginPaths.ReplacesUnpairedSurrogatesInTheMessage` | `StartupPlatformPlugin.MissingMessageReplacesUnpairedSurrogates` | The same property on the message that remains, plus a four-byte character. |
| `StartupPlatformPlugin.FoundInPlatformsDirectoryCaseInsensitively` | `StartupLoadPlatformPlugin.SkipsMissingCandidates` (letter case), `CandidatesFollowQtsOrder`, `MissingWhenNoPluginForThePlatformQtWillUse` | Candidates are now generated, not matched against a listing, so lookalike paths cannot arise; letter case is checked on a real file. |

New tests: `FrostProcess.SelfTestReportsUnloadablePlatformPlugin`, `SelfTestNeverShowsQtsDialog`,
`LateLoadOfADllBesideTheExeWorksFromALongPlainPath`, `LauncherReportsMissingAppDll`, `LauncherReportsMissingQtDll`,
`LauncherImportsOnlyKernel32`, `AppDllHasNoDependentLoadFlags`, `AppDllEmbedsNoManifest`;
`StartupPlatformPlugin.DebugQtUsesTheDSuffix`, `StartupLoadPlatformPlugin.ReportsWin32ErrorForAnUnloadableFile`,
`StartupHardeningBits.RoundTripEveryField`, `StartupHardeningDeathTest.ExtendedInstallDirectoryServesLaterLoads`,
`StartupDialogGuardDeathTest.RefusesDialogWindowsOnTheMainThread`, `StartupFatalHandlerDeathTest.QtFatalExitsWithStatus4`
and `OtherMessagesReachThePreviousHandler`; `FrostRuntimeDlls.CopiesOncePerDirectory` gained a shared library.

## Measurements
Windows 11 Home 10.0.26300, LongPathsEnabled=1, clang-cl 23.1.4, Qt 6.12.0, worktree of `m0/longpath` on 67838d1.
Every frost run went through a private desktop (tests: `tests/support/private_desktop.hpp`; scripts:
`run_hidden_nobox.py`); no window appeared on the user's desktop.

- **Spike** (`launcher-spike\README.txt`): the M0 criterion held in 132 of 132 cells (Release, Debug, ASan; 22
  lengths from 200 to 400; plain and `\\?\`), and in 54 of 54 with long paths ineffective (fact 19). At `wmain` the
  launcher has exactly frost.exe, ntdll, kernel32 and kernelbase mapped (fact 6). ProcessImageLoadPolicy set after
  hardening is compatible (fact 9); the first design's statement that `main.cpp` calls SetProcessMitigationPolicy
  was wrong: it never did, and this ADR adds none.
- **This change**: `frost_process_tests` (21 tests, 44 + 13 matrix cells) and `app_startup_tests` (20 tests) pass in
  Debug (2.5 s for the 44-cell matrix), Release and the ASan tree's RelWithDebInfo. The single-image frost.exe of
  67838d1, copied into a 240-unit directory and started plain, still ends with 0xC0000106 (Debug), where the launcher
  passes. Raw test results: `E:\FrostBuilds\perf\m0\longpaths-on\launcher-impl\`.
- **Cost (B1)**: start-to-exit wall time, before (single frost.exe at 67838d1) against after, each from its own
  copy in a 140-unit directory, 5 warm-up and 60 interleaved rounds (`launch_fast`, as spike fact 20), under
  `night.py measure-begin/-end`, PC in use (Discord, wallpaper64, two Claude sessions; 17% CPU). `--selftest`
  offscreen creates the QApplication and exposes the main window, the closest offscreen stand-in for B1's warm
  first frame (the on-screen B1 probe needs the user's approval while they are at the PC):

  | Build, mode | p50 before | p50 after | paired p50 (after - before) |
  |---|---|---|---|
  | Release `--selftest` | 28.82 ms | 29.64 ms | +0.68 ms |
  | Release `--version` | 17.83 ms | 18.10 ms | +0.17 ms |
  | Debug `--selftest` | 43.00 ms | 43.40 ms | +0.61 ms |
  | Debug `--version` | 19.81 ms | 19.79 ms | +0.07 ms |

  The expected +0.3 to +0.7 ms per process holds; B1's warm p50 budget is 150 ms. Data and script:
  `launcher-impl\timing_ab.json` and `timing_ab.py`.
- **E4**: the ASan case of the removed `PlatformPluginPathBoundary` now passes for a real reason. Its 225- and
  226-unit plain starts died because clang's ASan runtime was a static import of frost.exe; the launcher has no
  such import and loads the runtime by its `\\?\` path, so `StartsFromEveryDirectoryLength` passes at 225, 226 and
  every other length in the ASan tree. Branch `m0/e4-asan-runtime` (stripping the runtime's manifest) is not needed
  and stays unmerged.

## Consequences
- frost starts from every directory length, plain or `\\?\`, with or without LongPathsEnabled; the M0 criterion is
  met and tested on every machine (matrix row I7 is green).
- Two images to deploy: frost_app.dll must sit beside frost.exe; without it, or without one of its DLLs, the
  launcher reports which DLL failed and exits with 4 instead of the loader's 0xC0000135.
- Each start costs about 0.1-0.7 ms more (above), well inside B1.
- Late loads by bare name search the `\\?\` install directory and System32 only (`USER_DIRS | SYSTEM32`). A DLL
  beside frost.exe therefore loads from a long plain start (`LateLoadOfADllBesideTheExeWorksFromALongPlainPath`);
  under DEFAULT_DIRS it failed with 206 (spike fact 18).
- The launcher uses the release CRT in every configuration, so the Debug launcher has no CRT debug checks: it is a
  dozen loader calls, and the debug CRT's own assertion box would be out of `suppressCrashDialogs()`'s reach (spike
  fact 5).
- The ASan tree's Debug configuration is unusable (above); verification uses RelWithDebInfo.
- Not tested automatically: the launcher's GUI-mode message box, and Qt's GUI start with the windows platform plugin
  from a long path (both would draw on the desktop; spike facts 12 and 15 cover them on a private desktop).
- TODO(M1): log a failed non-platform preload in the GUI once frost::core has logging (`src/app/src/main.cpp`).
- PLAN.md's M0 status paragraph still describes the failing ASan test; it is left to the branch that edits PLAN.md.

## Alternatives considered
- **Strip RT_MANIFEST 2 from the deployed DLLs** (design B; probe 10; for the ASan runtime alone, branch
  `m0/e4-asan-runtime`): works, but modifies vendor binaries (their Authenticode signatures must go) and must be
  redone for every Qt update. Rejected by the user's decision.
- **Re-execute frost through a `\\?\` image path** (a launcher hop): a second process start costs 4-6 ms (fact 29),
  and the `\\?\` image path alone does not get Qt's plugins loaded (fact 16).
- **Start through the 8.3 short name** (E1 (d)): depends on the volume; E: generates none (fact 18), nor do Dev
  Drive and ReFS.
- **Delay-load Qt in frost.exe**: lld-link refuses, because frost imports data from all three Qt DLLs (fact 31).
- **QT_FATAL_WARNINGS=1 against Qt's box**: helps only when no plugin is found at all, and makes every warning fatal
  (fact 22). The dialog guard and fatal handler cover every case without global effects.
- **Trust LongPathsEnabled**: the switch does not move the loader's limit (fact 1), and frost cannot set it.
- **Use `\\?\` only beyond a length threshold**: equal in cost within noise (spike facts 21-22), and the threshold
  hangs on a hand-kept constant (the longest deployed DLL name).
- **Design A as first written**: `/DEPENDENTLOADFLAG:0xA00` on frost_app.dll breaks plain starts from 236 units, and
  its preload flags (`DLL_LOAD_DIR | DEFAULT_DIRS`) lose plugins whose dependencies sit beside frost.exe (spike facts
  7 and 16).
