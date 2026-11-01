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
Release, 235-249 in Debug and 218-249 in ASan trees (fact 16; ADR 0003). Below those ranges, from directories of 226
units in Release and 225 in Debug (where the platform plugin's path passes 250 units), and again from 250 units on
(where frost::io starts frost.exe through `\\?\`), frost reached `main`, but Qt strips `\\?\` and loads plugins
through their plain paths (facts 9-12). While long paths were ineffective, a start-up check then refused the long
plugin path with a message; with `LongPathsEnabled = 1` that check stood down and Qt itself failed to load the
plugin (fact 16). So the M0 exit criterion was not met (question E1), its test skipped while the switch was off, and
`FrostProcess.PlatformPluginPathBoundary` failed in the ASan tree (question E4).

Probe 7 found the way through: once a plugin is loaded by its `\\?\` path, Qt's later plain-path load of the same file
returns that module, at any length (facts 26-27). It also holds with long paths ineffective for the process (spike
fact 19). What was missing was a frost.exe with no static import that embeds RT_MANIFEST 2.

The options put to the user were "Approve stripping" (deploy Qt's DLLs and the ASan runtime without RT_MANIFEST 2,
probe 10 and branch `m0/e4-asan-runtime`), "Use the launcher design" and "Neither for now". The user's decision, on
2026-11-01 at about 11:00, was **"Use the launcher design"**, described to them as: *"Keep Qt's DLLs untouched;
frost.exe becomes a small launcher that loads frost_app.dll via `\\?\`. More work and still needs its own spike."*
(the code span only keeps Markdown from eating the backslashes; the words are the option's own). The spike ran the
same morning (`launcher-spike\README.txt`); its corrections to the first design (design A and
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
   2. the mode rule and the error mode. A start is a command-line mode when an argument before the first `--` starts
      with `-`, as frost_app.dll's parser reads options (`launcherCommandLineMode()`); `frost -- -file.txt` is a GUI
      start. Command-line modes set `SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX` for
      good; a GUI start sets `SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX` only while the launcher loads, and
      gives frost_app.dll the parent's mode back. Without it, a mismatched import (127) or a broken image (193) makes
      the loader raise a hard error, and csrss's modal "Entry Point Not Found" or "Bad Image" box would hold a
      terminal's `frost --version` until someone clicks (measured by the review of 4c5533d). The mode is
      process-wide because the loader maps imports on its worker threads too;
   3. `AddDllDirectory(\\?\<install dir>)`, then `SetDefaultDllDirectories(USER_DIRS | SYSTEM32)`
      (`searchInstallDirectoryExtended()`), always, whatever the length. The directory comes from
      `extendedDirectoryOf()`: GetModuleFileNameW reports the image path as the parent spelled it, and `\\?\` takes
      it literally, so a plain spelling is normalised first (GetFullPathNameW: `.`, `..`, `/`, trailing dots), a
      `\\.\` spelling becomes `\\?\`, and a `\\?\` one is kept. frost_app.dll's `installDirectory()` uses the same
      form. If AddDllDirectory fails, the default stays DEFAULT_DIRS (fail closed), and the self-test's
      process-hardening check, which carries the step as `ProcessHardening::installDirectoryExtended`, fails;
   4. in ASan trees only (compile definition `FROST_LAUNCHER_PRELOAD_ASAN_RUNTIME`), load
      `\\?\<dir>\clang_rt.asan_dynamic-x86_64.dll` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | _SYSTEM32` (0x900), so
      ASan initialises before the UCRT and Qt (spike fact 2);
   5. load `\\?\<dir>\frost_app.dll` with 0x900, resolve `frost_main` and return its result. frost_app.dll has no
      `/DEPENDENTLOADFLAG` (a value would override the launcher's flags for its imports; 0xA00 there broke plain
      starts from 236 units, spike fact 7) and is linked with `/MANIFEST:NO` (CMake's `vs_link_dll` would embed an
      RT_MANIFEST 2, spike fact 14).
   If a step cannot load its DLL, the launcher writes "frost: cannot load <path>: <reason> (win32 error N)" to
   stderr, or else to the parent's console (AttachConsole and CONOUT$, WriteConsoleW), shows a MessageBoxW (user32
   loaded from System32 only then) only in GUI mode, and exits with 4. When the file itself exists, the report reads
   "cannot load <path> or a DLL it needs: ...": the loader gives a missing, mismatched or broken import the same
   error as the DLL itself and does not name it.
4. **In frost_app.dll**, start-up runs in this order: arguments are parsed first; command-line modes (usage errors,
   --version, --help, --selftest) then apply `suppressCrashDialogs()`, the dialog guard (`guardCommandLineDialogs()`,
   a WH_CBT hook that refuses `#32770` windows on the main thread, so Qt's Release "no Qt platform plugin" box returns
   at once) and the fatal handler (`installCommandLineFatalHandler()`: a Qt fatal error prints "frost: Qt fatal
   error: ..." and exits with 4). `--version` and `--help` keep their output, exit codes and console handling. They
   construct no QApplication and load no plugin, but they need all of frost_app.dll's static imports (Qt6Core,
   Qt6Gui, Qt6Widgets; spike fact 11), and besides `qVersion()` they call `qInstallMessageHandler` (the fatal
   handler) and `SetWindowsHookExW` (the dialog guard). Delay-loading Qt is impossible as linked (fact 31).
   `preparePlatformPlugin()` pins Qt's platform plugin through its `\\?\` path with `USER_DIRS | SYSTEM32` before
   QApplication exists, in every mode. The candidates come from `QT_QPA_PLATFORM_PLUGIN_PATH` first, when it names an
   absolute path (`platformPluginOverrideDirectory()` normalises it as Qt and Win32 resolve it, because `\\?\` would
   not), then from `<install dir>\platforms`. A missing or unloadable plugin fails the self-test (exit 1) or the GUI
   start (message, exit 4). Only when no candidate exists but `QT_PLUGIN_PATH`, `QT_QPA_PLATFORM_PLUGIN_PATH` or
   `qt.conf` may point Qt elsewhere is nothing pinned; Qt may then fail itself, and the command-line guards keep that
   from showing a box. `preloadOtherPlugins()` pins the other deployed plugins when the longest plugin path passes the
   loader's limit (`dir + 1 + longest > 250`, over every listed DLL), best-effort, with failures in the self-test's
   `other-plugins` check. It loads only DLLs whose import directory names the Qt6Core the process loaded
   (`Qt6Core.dll` or `Qt6Cored.dll`, read from the file mapped as an image resource, so no code runs); a plugin of
   Qt's other build (qsvgd.dll beside qsvg.dll would bring in a second Qt) or a DLL that is no Qt plugin is skipped
   and named, as Qt would refuse it too.
5. **Self-test checks** (Windows): `process-hardening` (now including step 3.3), `dependent-load-flags` (0xa00 for
   frost.exe, 0 for frost_app.dll), `long-paths-effective` (a warning, reworded to "not effective for this
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
| `FrostProcess.PlatformPluginPathBoundary` (plugin path of 250 units loads, 251 refused, from directories of 225 and 226 units; failed in ASan) | `StartupLoaderLimit.PlainPathOfAManifestDllLoadsUpTo250Units`; `FrostProcess.StartsFromEveryDirectoryLength`; `StartupPluginPreload.NeededOnlyBeyondTheLoadersPlainPathLimit`; `StartupLoadPlatformPlugin.PinnedPluginSatisfiesQtsPlainPathLoad` | The loader's limit itself is measured on every run: the SDK's unmodified offscreen plugin loads unpinned from a plain path of exactly 250 units and fails with 206 at 251, so a lower real limit fails a test instead of making Qt skip plugins. The start side: the old plugin boundary falls on directories of 224, 225 and 226 units (`\platforms\qoffscreen(d).dll` is 25 or 26 units), which are matrix cells, among 22 lengths from 200 to 400 that must pass plain and `\\?\` within 30 s each in all three trees, ASan included, and report the form they started through. The 250/251 rule survives as the preload's gate; the pin is checked at 300 units, against an unpinned plain load that must fail there. |
| `FrostProcess.RunsFromPathLongerThanMaxPath` (skipped while LongPathsEnabled=0) | the same test without the skip, with a plain start added | Runs everywhere. A plain start needs the parent's long paths; without them CreateProcessW's refusal is expected. |
| `FrostProcess.RunsThroughShortNamesOfALongPath` (skipped when the short form exceeded 250 units) | the same test, skipping only where no 8.3 names exist | Expects the pin's report ("loaded Qt's platform plugin", `~1\platforms\`). |
| `FrostProcess.SelfTestPasses`: "dependent-load-flags: load configuration DependentLoadFlags = 0xa00" | "DependentLoadFlags frost.exe 0xa00, frost_app.dll 0x0", plus `load-form`, `dialog-guard`, `plugin-paths` and `late-load` | Both images are read; more checks. |
| `StartupPluginPaths.AcceptsThePlatformPluginUpToTheLimit`, `RejectsOneUnitBeyondTheLimitAndNamesTheShortestCandidate`, `IgnoresLengthWhenLongPathsAreEffective`, `CountsUtf16UnitsAndReportsUtf8`, `StartupPlatformPlugin.PresenceNotRequiredButLengthStillChecked` (the length rule of the removed `evaluatePluginPaths`) | `StartupPluginPreload.NeededOnlyBeyondTheLoadersPlainPathLimit`; `FrostProcess.StartsFromEveryDirectoryLength` (UTF-16 lengths with a non-ASCII first component); `StartupPluginPaths.PresenceCheckDefersToQtPluginPath` | frost no longer refuses long plugin paths; the remaining rule (when to preload) is pinned at 250/251, and the real start is pinned at every length. |
| `StartupPluginPaths.OneCandidateWithinTheLimitIsEnough` | `StartupPlatformPlugin.CandidatesFollowQtsOrder`; `StartupLoadPlatformPlugin.SkipsMissingCandidates` | Qt's fallback order (`direct2d;windows`) and that a later candidate is used when an earlier one is absent. |
| `StartupPluginPaths.OtherPluginsBeyondTheLimitOnlyWarn` | `FrostProcess.PreloadsOtherPluginsFromALongPath`, `PreloadFailureOnlyWarns` | Long non-platform plugins are now loaded (qsvg with its Qt6Svg dependency at 300 units), not only warned about, and the other build's plugin and a non-Qt DLL beside it are left alone. A plugin that cannot be loaded is still only a warning: the self-test names it in `other-plugins` and passes. |
| `StartupPluginPaths.ReplacesUnpairedSurrogatesInTheMessage` | `StartupPlatformPlugin.MissingMessageReplacesUnpairedSurrogates` | The same property on the message that remains, plus a four-byte character. |
| `StartupPlatformPlugin.FoundInPlatformsDirectoryCaseInsensitively` | `StartupLoadPlatformPlugin.SkipsMissingCandidates` (letter case), `CandidatesFollowQtsOrder`, `MissingWhenNoPluginForThePlatformQtWillUse` | Candidates are now generated, not matched against a listing, so lookalike paths cannot arise; letter case is checked on a real file. |

New tests: `FrostProcess.SelfTestReportsUnloadablePlatformPlugin`, `SelfTestNeverShowsQtsDialog`,
`LateLoadOfADllBesideTheExeWorksFromALongPlainPath`, `PreloadFailureOnlyWarns`, `SelfTestOnScreenUpToMaxPath`,
`LauncherReportsMissingAppDll`, `LauncherReportsMissingQtDll`, `LauncherSetsTheErrorModeForItsLoads`,
`LauncherImportsOnlyKernel32`, `AppDllHasNoDependentLoadFlags`, `AppDllEmbedsNoManifest`;
`StartupLoaderLimit.PlainPathOfAManifestDllLoadsUpTo250Units`, `StartupLauncherMode.OptionsBeforeDoubleDashSelectACommandLineMode`,
`StartupInstallDirectory.ExtendedDirectoryNormalisesThePlainSpelling`,
`StartupPlatformPlugin.OverrideDirectoryIsNormalisedAsQtResolvesIt`, `PinsFromQtQpaPlatformPluginPathFirst`,
`DebugQtUsesTheDSuffix`, `StartupLoadPlatformPlugin.ReportsWin32ErrorForAnUnloadableFile`,
`StartupHardeningBits.RoundTripEveryField`, `StartupHardeningDeathTest.ExtendedInstallDirectoryServesLaterLoads`,
`StartupDialogGuardDeathTest.RefusesDialogWindowsOnTheMainThread`, `StartupFatalHandlerDeathTest.QtFatalExitsWithStatus4`
and `OtherMessagesReachThePreviousHandler`; `FrostRuntimeDlls.CopiesOncePerDirectory` gained a shared library.
`LauncherSetsTheErrorModeForItsLoads` copies frost.exe beside a stand-in frost_app.dll
(`tests/app/launcher_probe_app.cpp`, KERNEL32.dll only) that reports the error mode during its own load; the parent
hands down `SEM_NOGPFAULTERRORBOX` alone, so the critical-error flags it sees are the launcher's. The stand-in loads
cleanly, so no hard error can arise even if the launcher regresses.

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

  Data and script: `launcher-impl\timing_ab.json` and `timing_ab.py`. The review of 4c5533d re-ran the comparison
  (5 warm-up and 100 interleaved rounds, another workflow's build running, 27% CPU, so noisier): Release
  `--selftest` +0.98 ms, `--version` +0.17 ms; Debug `--selftest` +0.93 ms, `--version` +0.12 ms; a start through a
  `\\?\` image path adds +3.0 to +3.8 ms (review scratch `review1\pt_timing.json`). So the launcher adds about
  0.1-1.0 ms per start. B1 itself is not met before or after: its warm p50 budget is 150 ms, and frost.exe's warm
  p50 was already 264.9 ms before any editor work (`docs/progress.md`, A3). The launcher makes that about 1 ms
  worse; design A's requirement was that B1 not be made worse. The real B1, the on-screen first-frame probe, is
  unmeasured here because it needs the user's approval while they are at the PC.
- **E4**: the ASan case of the removed `PlatformPluginPathBoundary` now passes for a real reason. Its 225- and
  226-unit plain starts died because clang's ASan runtime was a static import of frost.exe; the launcher has no
  such import and loads the runtime by its `\\?\` path, so `StartsFromEveryDirectoryLength` passes at 225, 226 and
  every other length in the ASan tree. Branch `m0/e4-asan-runtime` (stripping the runtime's manifest) is not needed
  and stays unmerged.
- **The GUI from long image paths** (review of 4c5533d, every run on a hidden desktop; scripts and logs in its
  scratch folder `review1\`, `pg*_gui` and `pn_nvidia`): with Qt's native platform plugin (windows), the GUI and
  `--selftest=onscreen` run up to an image path of 259 UTF-16 units (a 249-unit directory plain, 245 through `\\?\`,
  whose prefix counts) and die from 260 units on with 0xC0000409 within about 0.13 s, before any window appears, in
  Release, Debug and ASan. The fault is a fail-fast (FAST_FAIL_INVALID_ARG) in nvldumdx.dll, NVIDIA's user-mode
  driver loader, inside its DllMain, which qwindows.dll reaches through d3d9.dll. A copy of python.exe that creates a
  hardware Direct3D 11 device, without frost or Qt, dies the same way from 260 units and works at 255, so the limit
  is the driver's: it depends only on the image path's length, not on LongPathsEnabled. `QT_OPENGL=desktop`,
  `software` and `angle` do not help. The single-image frost died earlier, with 0xC0000106. Confirmed only on
  hidden desktops; a run on the interactive desktop needs the user's approval.
  `FrostProcess.SelfTestOnScreenUpToMaxPath` now covers the native platform up to 259 units on a private desktop.
- **Review fixes**: the review's spelling probe, re-run against the fixed Debug launcher (scratch
  `fixcheck\fc_spelling-debug.jsonl`): `\\.\`, `..`, `.`, trailing-dot, doubled-separator and forward-slash spellings
  of the image path all pass `--selftest` and `--version`, where 4c5533d exited 4 for `\\.\` and failed
  process-hardening for the others. The single-image baseline at 240 units:
  `launcher-impl\review-fixes\baseline_67838d1_debug_240.txt`. After the fixes, `frost_process_tests` (24 tests)
  and `app_startup_tests` (25) pass in Debug, Release and the ASan tree's RelWithDebInfo under `verify.ps1 -Full`
  (`review-fixes\verify_full.log`; the Release and ASan ctest logs beside it, with the spelling probe's script and
  results).

## Consequences
- frost's command-line modes and its offscreen self-test start from every measured directory length (22 lengths from
  200 to 400 units, plain and `\\?\`, in Release, Debug and ASan), with or without LongPathsEnabled. The M0
  criterion is met for them and tested on every run.
- The GUI is not: on this machine it dies from an image path of 260 units on, in NVIDIA's driver (Measurements). The
  M0 criterion is therefore not met for the IDE itself, and matrix row I7 stays `in-progress` until the user decides
  how frost should avoid that driver's limit (for example by re-starting itself through a short alias of its
  directory when its image path reaches MAX_PATH) or accepts it as a known limitation.
- Two images to deploy: frost_app.dll must sit beside frost.exe; without it, or without one of its DLLs, the
  launcher reports whether frost_app.dll (or the ASan runtime) is missing or loads but lacks a DLL it needs, and
  exits with 4 instead of the loader's 0xC0000135. It cannot name the missing import: the loader does not say.
- Each start costs about 0.1-1.0 ms more (Measurements), on top of a B1 that already fails.
- Late loads by bare name search the `\\?\` install directory and System32 only (`USER_DIRS | SYSTEM32`). A DLL
  beside frost.exe therefore loads from a long plain start (`LateLoadOfADllBesideTheExeWorksFromALongPlainPath`);
  under DEFAULT_DIRS it failed with 206 (spike fact 18).
- The launcher uses the release CRT in every configuration, so the Debug launcher has no CRT debug checks: it is a
  dozen loader calls, and the debug CRT's own assertion box would be out of `suppressCrashDialogs()`'s reach (spike
  fact 5).
- The ASan tree's Debug configuration is unusable (above); verification uses RelWithDebInfo.
- Not tested automatically: the launcher's GUI-mode message box, the GUI start itself (`frost` without arguments)
  and anything on the interactive desktop. The native platform plugin is tested through `--selftest=onscreen` on a
  private desktop up to an image path of 259 units (`SelfTestOnScreenUpToMaxPath`); from 260 it fails here (above).
  The spike did not cover the native platform at all (its README lists it as not measured); spike fact 15 is the
  offscreen self-test, and only probe fact 27 (qtprobe, not frost) loaded the windows platform after a preload.
- The GUI keeps Windows Error Reporting and its default error mode after the launcher's loads; a broken plugin that
  frost_app.dll pins in a GUI start can still raise the loader's own box before frost's message.
- TODO(M1): log a failed non-platform preload in the GUI once frost::core has logging (`src/app/src/main.cpp`).
- PLAN.md's M0 status paragraph (section 7) and its "Next step" note still say that the launch over 260 characters is
  not met while LongPathsEnabled=0 and that one ASan test fails; both are left to the branch that edits PLAN.md.

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
