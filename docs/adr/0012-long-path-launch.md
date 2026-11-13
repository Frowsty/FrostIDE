# ADR 0012: frost.exe launcher and frost_app.dll; start from any path length

- Status: accepted (the user's two decisions of 2026-11-01, quoted below)
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

The launcher met the criterion for the command-line modes and the offscreen self-test, but its review (of 4c5533d)
found a second limit that is not the loader's: with Qt's native platform plugin, the GUI and `--selftest=onscreen`
died from an image path of 260 UTF-16 units (MAX_PATH) on, in NVIDIA's user-mode driver (Measurements, "The GPU
driver's limit"). The options put to the user were "Restart via short link (Recommended)", "Use 8.3 short names"
and "Known limitation". The user's decision, on 2026-11-01 at about 14:15, was **"Restart via short link
(Recommended)"**, described to them as: *"When frost.exe's path is 260+, the launcher makes a junction (no admin
needed) under `%LOCALAPPDATA%\FrostIDE` pointing at the install folder and restarts frost through it. Costs about 5
ms, only in that case. The GUI then works from any path; tests prove it."* (again, the code span is only for the
backslash). Decision 8 below is its design. The quoted cost was too low. The restart measured +6.4 to +7.8 ms
(paired p50) with a stand-in frost_app.dll that has no Qt, and +8.5 to +10.3 ms (p50 against p50; paired p50 +9.0
and +9.7 ms) for the real `--selftest=onscreen`, the closest measured stand-in for a GUI start. So the GUI pays about
10 ms, twice the 5 ms the option named (Measurements). No budget covers the restart; the figure is reported to the
user with the review fixes of 2f40f9f. The words "from any path" hold with the exceptions that decision 8 adds,
among them install directories longer than a junction can name (about 4,089 units) and elevated GUI and on-screen
starts from 260 units; Consequences ("New refusals") lists them all.

## Decision
1. **Two images on Windows.** frost.exe is the launcher (`src/app/src/launcher.cpp`); the application is
   frost_app.dll (target `frost_app_shared`, output name `frost_app`, import library `frost_app_shared.lib` so it
   cannot clash with the static `frost_app.lib`), which exports
   `int frost_main(unsigned hardeningBits, const wchar_t* restartedFrom)` (`src/app/src/main.cpp`; `restartedFrom`
   is null unless the launcher was restarted and the restart marker names its own image file, decision 8). On Linux
   frost stays one executable, and the restart is compiled out.
2. **The launcher** is built with the static release CRT in every configuration (`MSVC_RUNTIME_LIBRARY
   MultiThreaded`), without sanitizers (`frost_add_executable(... NO_SANITIZE)`), with `/ENTRY:wmainCRTStartup`,
   the GUI subsystem, `/MANIFEST:NO` (frost.rc embeds the process manifest as RT_MANIFEST 1) and
   `/DEPENDENTLOADFLAG:0xA00`. Its only static import is KERNEL32.dll, in every tree, ASan included. It does not use
   frost_app.lib's or frost_core.lib's objects: the Qt-free start-up core (`src/app/src/startup_core.cpp`) is
   compiled twice, `/MT` into the launcher and `/MD` into frost::app, because lld-link's `/failifmismatch` forbids
   mixing them.
3. **Launcher sequence** (`wmain`):
   1. `hardenProcess()` while only ntdll, kernel32 and kernelbase are mapped; then the restart marker
      `FROST_RESTARTED_FROM` is read and removed from the environment, and once the image path is known it is kept
      only if it names this image file (decision 8);
   2. the mode rule and the error mode. A start is a command-line mode when an argument before the first `--` starts
      with `-`, as frost_app.dll's parser reads options (`launcherCommandLineMode()`); `frost -- -file.txt` is a GUI
      start. Command-line modes set `SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX` for
      good; a GUI start sets `SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX` only while the launcher loads, and
      gives frost_app.dll the parent's mode back. Without it, a mismatched import (127) or a broken image (193) makes
      the loader raise a hard error, and csrss's modal "Entry Point Not Found" or "Bad Image" box would hold a
      terminal's `frost --version` until someone clicks (measured by the review of 4c5533d). The mode is
      process-wide because the loader maps imports on its worker threads too;
   3. when the image path is 260 units or longer and the start may load a GPU driver, the restart through a short
      link (decision 8), whose exit status the launcher returns; nothing below runs in that process;
   4. `AddDllDirectory(\\?\<install dir>)`, then `SetDefaultDllDirectories(USER_DIRS | SYSTEM32)`
      (`searchInstallDirectoryExtended()`), always, whatever the length. The directory comes from
      `extendedDirectoryOf()`: GetModuleFileNameW reports the image path as the parent spelled it, and `\\?\` takes
      it literally, so a plain spelling is normalised first (GetFullPathNameW: `.`, `..`, `/`, trailing dots), a
      `\\.\` spelling becomes `\\?\`, and a `\\?\` one is kept. frost_app.dll's `installDirectory()` uses the same
      form. If AddDllDirectory fails, the default stays DEFAULT_DIRS (fail closed), and the self-test's
      process-hardening check, which carries the step as `ProcessHardening::installDirectoryExtended`, fails;
   5. in ASan trees only (compile definition `FROST_LAUNCHER_PRELOAD_ASAN_RUNTIME`), load
      `\\?\<dir>\clang_rt.asan_dynamic-x86_64.dll` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | _SYSTEM32` (0x900), so
      ASan initialises before the UCRT and Qt (spike fact 2);
   6. load `\\?\<dir>\frost_app.dll` with 0x900, resolve `frost_main` and return its result. frost_app.dll has no
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
5. **Self-test checks** (Windows): `process-hardening` (now including step 3.4), `dependent-load-flags` (0xa00 for
   frost.exe, 0 for frost_app.dll), `long-paths-effective` (a warning, reworded to "not effective for this
   process (needs LongPathsEnabled=1 and longPathAware); FrostIDE starts either way"), `image-path` (information:
   the form and length frost.exe was started through, and after a restart "; restarted through this short link
   from <first image path> (<form>, <units> UTF-16 units)"), `load-form` (frost_app.dll and Qt6Core loaded through
   `\\?\` paths), `dialog-guard`, `plugin-paths`, `other-plugins` and `late-load` (a DLL beside frost.exe that nothing
   loaded, loaded later by bare name).
6. **Build system.** `FROST_SANITIZE`'s flags are applied per target by `frost_apply_compile_options` instead of
   globally, so the launcher can opt out. `frost_add_library(... SHARED)` exists, and the per-directory runtime-DLL
   copy (`frost_copy_runtime_dlls`) treats shared libraries as members of their directory: ordered after its copy,
   their imported DLLs in it. The ASan tree's Debug configuration does not build (clang-cl refuses `/MDd` with
   ASan); its RelWithDebInfo and Release do. If UBSan is wanted on Windows, it must come with ASan or in trap mode:
   its standalone runtime is static-only (spike fact 4).
7. **No deployed DLL is modified.**
8. **The restart through a short link** (`launcher.cpp`'s `restartThroughShortLink()`; `startup_core.hpp`'s
   `launcherMayReachGpuDriver()` and `prepareShortLink()`; the user's second decision):
   - *Trigger.* GetModuleFileNameW's path, with any `\\?\` prefix (the driver counts it too), is 260 units or longer,
     and the start may create a Direct3D device: the GUI (no option before `--`) or `--selftest=onscreen`, unless
     `QT_QPA_PLATFORM`'s first named entry is `offscreen`; `--selftest` restarts too when `QT_QPA_PLATFORM` names
     another platform, as main.cpp then uses it. `--version`, `--help`, usage errors and the offscreen self-test
     create no QApplication on a native platform and start in place: they work from 400 units
     (`StartsFromEveryDirectoryLength`), and from 300 units they create no link
     (`OtherModesStartInPlaceFromALongPath`).
     The test runs before any DLL of frost's is loaded, so the first launcher costs one process start and its link
     check.
   - *Where.* `%LOCALAPPDATA%\FrostIDE\i\<16 hex digits>`, the digits being the 64-bit FNV-1a hash (public domain) of
     the install directory's final path (GetFinalPathNameByHandleW: 8.3 components, letter case and links
     resolved), so each install directory has one link, whatever its spelling. LOCALAPPDATA is read from the
     environment, not with SHGetKnownFolderPath: shell32 would be the launcher's first DLL beyond KERNEL32 (3.3 ms to
     load in a fresh process, junction probe `pj_fs.json`); the parent controls the environment as it controls the
     image path, the current directory and the DLL search order anyway; and tests point it at a scratch folder.
     `prepareShortLink()` checks that the value starts with a drive letter, a colon and a separator, then normalises
     it as Win32 opens a plain path (GetFullPathNameW: `.`, `..`, `/`, repeated separators, trailing dots and spaces),
     because the `\\?\` form of the file operations would take those literally (`RootIsNormalisedAsWin32OpensIt`),
     and refuses it on a network drive or when too long. It does not check who may write there (*Security*).
     `<root>\FrostIDE\i\<16>\frost.exe` is the root plus 38 units, so a LOCALAPPDATA of 222 units or more is refused.
   - *Creating and checking the link* without administrator rights: `CreateDirectoryW`, then
     `FSCTL_SET_REPARSE_POINT` with `IO_REPARSE_TAG_MOUNT_POINT`, substitute name `\??\<target>` and the target as
     print name. Whatever is at the link's path is opened with `FILE_FLAG_OPEN_REPARSE_POINT` (never followed) and
     inspected through that handle (FileAttributeTagInfo, FSCTL_GET_REPARSE_POINT): a junction to the install
     directory is kept as it is; a junction to anything else or an empty directory is rewritten in place; another
     name surrogate (a symbolic link, a WSL symlink) is deleted itself (FileDispositionInfo through the handle) and
     created anew, never its target; a file, a directory with contents and a reparse point that is no name surrogate
     (cloud files, dedup, AF_UNIX: deleting it would delete data) are refused with "remove it and start frost
     again, or install FrostIDE in a directory with a shorter path" and left untouched. After every change the link
     is opened and checked again; a sharing violation, access denial or pending delete (another frost setting the
     same link up) is retried every 10 ms for up to about 1 s. The verified link is held open with GENERIC_READ and
     FILE_SHARE_READ only, until the restarted frost exits; what that stops and what it does not is under *Security*.
   - *Refusals* (status 4, the message on stderr or the parent's console, and a box in GUI mode, as for a missing
     DLL): a launcher running elevated (*Security*); LOCALAPPDATA unset, relative, on a network drive or too long; an
     install directory on a network share, mapped drive or volume without a drive letter (a junction can only point
     at local volumes) or longer than about 4,089 units (a junction's reparse data holds at most 16 KiB); something
     frost does not remove at the link's path. Every message starts "frost.exe's path is N UTF-16 units long, and
     graphics drivers end a program started from 260 units (MAX_PATH) or more, so frost restarts through a short
     link, but".
   - *Security.* An earlier version of this section, and of the code comments, said that the held handle stopped
     anyone from retargeting the junction. That was wrong (both reviews of 2f40f9f), and this is what holds:
     - *Who can write the link.* The folders inherit the ACL of the folder LOCALAPPDATA names; frost checks neither
       their owner nor their ACL. Under the real profile folder that ACL admits the user, SYSTEM and Administrators
       only, so no other standard user can write there. A LOCALAPPDATA that the parent points at a folder other users
       may write would let them plant or change the link; the parent controls the environment as it controls the
       image path.
     - *What holding it stops.* Sharing modes refuse openers for write-data, append-data or delete access
       (GENERIC_WRITE, DELETE: error 32), so while frost holds the link nobody can remove or rename it, nor rename a
       folder above it (junction probe `pj_fs.json`; `CreatesAJunctionToTheInstallDirectoryAndKeepsIt`).
     - *What it does not stop.* Sharing modes never check FILE_WRITE_ATTRIBUTES, WRITE_DAC or WRITE_OWNER, and
       FILE_WRITE_ATTRIBUTES is all FSCTL_SET_REPARSE_POINT and FSCTL_DELETE_REPARSE_POINT need. A process that may
       write the link can therefore retarget or strip it at any moment: between the check and CreateProcessW, and
       while the restarted frost loads frost_app.dll, Qt and the plugins through it. No share mode prevents that.
       `HoldStopsRemovalButNotAReparseDataWriter` retargets and strips a held link, and shows the next start repairing
       it.
     - *Folders above the link.* The restart starts the link's *resolved* path (GetFinalPathNameByHandleW of the held
       handle), so a junction or symbolic link above it, in a redirected LOCALAPPDATA say, cannot redirect the start
       (`IsNamedByItsResolvedPath`). Every folder in the resolved path contains the link, so none can be made a
       junction (error 145), and none can be renamed while the link is held.
     - *Same-user tampering is out of scope*: a process running as the user can change the link, as it can replace
       frost.exe or a DLL beside it, so the link gives it no code execution it did not have.
     - *Elevated starts refuse.* That last argument fails for an elevated frost: a program of the same user that
       runs without elevation could retarget the link and so choose the code an elevated frost loads (the usual
       junction-based UAC bypass), which a protected install directory would otherwise prevent. So a launcher whose
       token is the elevated half of a split token (TokenElevationTypeFull, "Run as administrator") refuses the
       restart before it touches the link: "... but it runs elevated (as administrator), and programs that run
       without elevation could change that link and so the code frost loads. Start frost without elevation, or
       install FrostIDE in a directory with a shorter path." Tokens without a linked token (a standard user, UAC
       off, the built-in Administrator, a service) and the filtered half restart (`shortLinkAllowedFor()`). The
       token is read with GetTokenInformation through the API set `api-ms-win-security-base-l1-1-0.dll`, which
       resolves to the KERNELBASE.dll every process has mapped, so the launcher still imports KERNEL32.dll alone
       (`StartupElevation.OnlyTheElevatedHalfOfASplitTokenRefusesTheShortLink` checks it against advapi32's answer).
       The refusal itself is untested: no test can elevate without a prompt.
   - *The restart.* CreateProcessW with the link's image path as lpApplicationName (absolute; no search), a copy of
     GetCommandLineW() (so argv[0] still names the first path, as the parent wrote it), the same current directory
     and environment plus `FROST_RESTARTED_FROM=<first image path>`, the parent's error mode (restored around the
     call), the launcher's priority class, and GetStartupInfoW's data: desktop, title, show state, position, and the
     standard handles, made inheritable and passed with `bInheritHandles` (handles the parent let frost inherit pass
     on too; the C runtime's `lpReserved2` descriptor table is dropped). Under STARTF_USEHOTKEY or STARTF_USEMONITOR
     (0x400, absent from the SDK's headers: an HMONITOR in hStdOutput, from ShellExecuteEx's SEE_MASK_HMONITOR or a
     taskbar start) the handle fields pass as the parent set them (`startupFlagsAllowStandardHandles()`). The child
     stays in the parent's jobs. The
     launcher cannot use frost::io's IProcessLauncher, the project's rule for child processes: it links neither
     frost::io nor frost::core (`/MT`, KERNEL32.dll only), so this is the one CreateProcessW outside frost::io. In a
     command-line mode without standard handles (a terminal start without redirection) the launcher first attaches to
     the parent's console and ignores its Ctrl+C events while it waits, so the restarted frost_app.dll attaches to
     that console through it and decides about Ctrl+C itself; the GUI does not attach, so no console window is held
     open.
   - *Waiting.* The launcher waits in every mode and exits with the restarted process's status. That keeps the
     parent's process handle meaning "frost runs": `frost --selftest=onscreen`'s status and output reach a script
     unchanged, and B20 (exit to process gone) still measures frost's own exit, plus 1.2 ms (Measurements). Exiting
     at once would end every waiting parent early and hand B20 a process that is already gone.
   - *The marker and the loop guard.* A launcher keeps the marker only if it names its own image file
     (`sameImageFile()`: both paths normalised as `extendedDirectoryOf()` normalises them and opened, then the volume
     serial numbers and 128-bit file IDs compared; two opens, only when the marker is set). The first launcher sets it
     to its own path, and the restarted one runs the same file through the link, so a real restart always passes. A
     marker that names another file, left in a parent's environment or set by hand, is dropped: it neither stops the
     restart nor reaches `frost_main()`, so frost never names a directory it does not run from
     (`RestartedLauncherNeverRestartsAgain`, `StartupRestartMarker.CountsOnlyForTheSameImageFile`). A launcher that
     kept the marker never restarts. If its own image path is still 260 units or longer, which a real restart cannot
     produce (the link's path is kept short) but a parent that sets the marker to frost's own long path can, it
     refuses with status 4 ("frost was restarted through the short link X from Y, but that path is still N UTF-16
     units long, ..."). The marker is still no stronger than the link it came through (same-user tampering, above).
   - *What frost shows.* After a restart every path frost names itself is the link's (Qt's application directory,
     the plugin paths in messages). The launcher hands the first image path to `frost_main()`, and the places where a
     user reads frost's location name it too: the self-test's `image-path` check, and a GUI start-up failure, whose
     message and box end "FrostIDE is installed in <real directory>. It runs through the short link <link>, because
     graphics drivers end a program started from a path of 260 UTF-16 units or more."
   - *Stale links.* A link whose install directory is gone stays behind, an empty junction of a few hundred bytes;
     TODO(M2) in `startup_core.hpp`: the uninstaller, or a sweep once frost has a settings store, removes them.

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
| `FrostProcess.SelfTestOnScreenUpToMaxPath` (the native platform from the build directory and from 259 units, plain and `\\?\`; it stopped below the driver's limit) | `FrostProcess.SelfTestOnScreenFromEveryImagePathLength` | The same three cells, plus image paths of 250, 260, 261, 300 and 400 units in both forms; from 260 the restart must happen (one junction, the restarted image path and the first one named) and every check must pass; below 260 no link may be created. |

New tests: `FrostProcess.SelfTestReportsUnloadablePlatformPlugin`, `SelfTestNeverShowsQtsDialog`,
`LateLoadOfADllBesideTheExeWorksFromALongPlainPath`, `PreloadFailureOnlyWarns`,
`SelfTestOnScreenFromEveryImagePathLength`, `OtherModesStartInPlaceFromALongPath`,
`RestartReportsTheRestartedProgramsFailure`, `RestartPassesStatusAndStandardStreamsThrough`,
`RestartedLauncherNeverRestartsAgain`, `RestartRefusesWithoutAUsableShortLink`, `GuiStartFromALongPathShowsItsWindow`,
`GuiFailureAfterARestartNamesTheInstallDirectory`,
`LauncherReportsMissingAppDll`, `LauncherReportsMissingQtDll`, `LauncherSetsTheErrorModeForItsLoads`,
`LauncherImportsOnlyKernel32`, `AppDllHasNoDependentLoadFlags`, `AppDllEmbedsNoManifest`;
`StartupLoaderLimit.PlainPathOfAManifestDllLoadsUpTo250Units`, `StartupLauncherMode.OptionsBeforeDoubleDashSelectACommandLineMode`,
`StartupInstallDirectory.ExtendedDirectoryNormalisesThePlainSpelling`,
`StartupPlatformPlugin.OverrideDirectoryIsNormalisedAsQtResolvesIt`, `PinsFromQtQpaPlatformPluginPathFirst`,
`DebugQtUsesTheDSuffix`, `StartupLoadPlatformPlugin.ReportsWin32ErrorForAnUnloadableFile`,
`StartupHardeningBits.RoundTripEveryField`, `StartupHardeningDeathTest.ExtendedInstallDirectoryServesLaterLoads`,
`StartupDialogGuardDeathTest.RefusesDialogWindowsOnTheMainThread`, `StartupFatalHandlerDeathTest.QtFatalExitsWithStatus4`
and `OtherMessagesReachThePreviousHandler`, `StartupGpuDriverRule.OnlyANativeQApplicationMayReachTheDriver`,
`StartupShortLink.NameIsAStableHashOfTheDirectory`, `MountPointReparseDataRoundTrips`,
`CreatesAJunctionToTheInstallDirectoryAndKeepsIt`, `HoldStopsRemovalButNotAReparseDataWriter`,
`RootIsNormalisedAsWin32OpensIt`, `HandlesWhatItFindsAtItsPathWithoutFollowingIt`, `IsNamedByItsResolvedPath` and
`RefusesWhereALinkCannotHelp`, `StartupRestartMarker.CountsOnlyForTheSameImageFile`,
`StartupElevation.OnlyTheElevatedHalfOfASplitTokenRefusesTheShortLink`,
`StartupLauncherMode.RestartPassesStandardHandlesUnlessAFieldMeansSomethingElse`;
`FrostRuntimeDlls.CopiesOncePerDirectory` gained a shared library. The process tests' LOCALAPPDATA and the unit
tests' link root have spaces and non-ASCII characters ("ä 测"), as PLAN.md's M0 fixture for profile paths asks.
`LauncherSetsTheErrorModeForItsLoads` copies frost.exe beside a stand-in frost_app.dll
(`tests/app/launcher_probe_app.cpp`, KERNEL32.dll only) that reports the error mode during its own load; the parent
hands down `SEM_NOGPFAULTERRORBOX` alone, so the critical-error flags it sees are the launcher's. The stand-in loads
cleanly, so no hard error can arise even if the launcher regresses. The restart tests use the same stand-in, which
also reports the path it was restarted from, its image path and whether the marker is still set, and on request
copies stdin to stdout, writes to stderr and exits with a chosen status
(`RestartPassesStatusAndStandardStreamsThrough` checks 70 KB of binary stdin, both streams byte for byte, statuses 0, 7, 42, 256 and 0xC0000409, the parent's error
mode and the hardening bits through the restart). `HandlesWhatItFindsAtItsPathWithoutFollowingIt` plants an empty
directory, a directory with contents, a junction to another directory, WSL symlinks on a directory and on a file (a
name surrogate a standard user may create), an NT symbolic link where this account may create one (here it may not:
error 1314, so the WSL links stand in for it on the same code path), a file and an AF_UNIX reparse point; it checks
what each becomes and that what a replaced link pointed at is untouched. The GUI tests find the restarted frost's
window (the main window "FrostIDE", or the failure box "FrostIDE cannot start") on the test's private desktop with
EnumDesktopWindows, check that the launcher is its parent and still waiting, read the box's text, and close it with
WM_CLOSE.

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
- **The GPU driver's limit** (review of 4c5533d, every run on a hidden desktop; scripts and logs in its
  scratch folder `review1\`, `pg*_gui` and `pn_nvidia`): with Qt's native platform plugin (windows), the GUI and
  `--selftest=onscreen` run up to an image path of 259 UTF-16 units (a 249-unit directory plain, 245 through `\\?\`,
  whose prefix counts) and die from 260 units on with 0xC0000409 within about 0.13 s, before any window appears, in
  Release, Debug and ASan. The fault is a fail-fast (FAST_FAIL_INVALID_ARG) in nvldumdx.dll, NVIDIA's user-mode
  driver loader, inside its DllMain, which qwindows.dll reaches through d3d9.dll. A copy of python.exe that creates a
  hardware Direct3D 11 device, without frost or Qt, dies the same way from 260 units and works at 255, so the limit
  is the driver's: it depends only on the image path's length, not on LongPathsEnabled. `QT_OPENGL=desktop`,
  `software` and `angle` do not help. The single-image frost died earlier, with 0xC0000106.
  On the interactive desktop, with the user's approval at the PC (2026-11-01 14:20-14:23, Release launcher of 33a329a;
  `E:\FrostBuilds\perf\m0\longpaths-on\launcher-impl\onscreen\README.txt`): the GUI from a 210-unit image path showed
  its window and `--selftest=onscreen` passed on 'windows'; from 310 units both ended with 0xC0000409 after 0.1-0.3 s
  without a window; and the python.exe Direct3D 11 control, started through its `\\?\` form, created a hardware
  device at 246 and 251 units and died with 0xC0000409 at 260, 271 and 311. So the limit holds on the user's desktop
  too, for any process.
- **B1 on screen** (the same session, approved by the user;
  `E:\FrostBuilds\perf\m0\longpaths-on\launcher-impl\b1-onscreen\README.txt`): `s1_first_frame --mode warm --runs
  15`, rounds interleaved, under `night.py measure-begin/-end`: the single-image frost.exe (m0/foundation 2c499a2)
  p50 197.5 ms, the launcher (33a329a) p50 195.6 ms, n 24 valid each; -1.9 ms, within noise. B1's warm p50 budget
  (150 ms) is missed before and after, as recorded (ADR 0007, question A3). Both runs start from short paths, so
  no restart is involved.
- **The restart on the interactive desktop** (2026-11-01 at about 16:30, the Release build of b9dd60f, under the
  user's standing permission of about 14:35 for window tests while they are away;
  `E:\FrostBuilds\perf\m0\longpaths-on\launcher-impl\onscreen\README.txt`, with `restart_fg.py` and
  `restart_fg.json` (the ShellExecuteW runs) and `restart_fg2.json` (the CreateProcessW runs) beside it; the script
  of the CreateProcessW runs was not saved, so their method rests on the README). The Release launcher tree was
  copied into a directory with non-ASCII characters and spaces, so that frost.exe's image path was 310 units, and
  LOCALAPPDATA pointed at a scratch folder.
  - *Started with CreateProcessW*, as a terminal or a script starts it, alternating 210 and 310 units, twice each.
    Every run showed a visible Qt window within about 0.2 s of the start (`after_s` is 0.202 in all four, which
    looks like the polling interval), and it was the foreground window. From 310 units the window belonged to the
    restarted process (a pid other than the launcher's). One junction was created under the scratch LOCALAPPDATA,
    and the second long start reused it. The launcher exited 0 after WM_CLOSE. So on this path the restarted GUI
    comes to the foreground without AllowSetForegroundWindow.
  - *Started with ShellExecuteW*, the shell's API: one direct ShellExecuteW of the 310-unit frost.exe returned 5
    (SE_ERR_ACCESSDENIED), and no window and no junction appeared, so frost did not run at all. The run through a
    `.lnk` shortcut also returned 5, but it shows less: WScript.Shell refused the 310-unit target when the script
    created the shortcut (ArgumentException, `restart_fg.json`), so that shortcut did not name frost.exe. Inferred,
    not measured: that the path's length causes the 5 (no ShellExecuteW of a shorter path was run as a control),
    that Explorer and real shortcuts, which start programs through the shell, fail the same way (neither was
    tried), and that the limit starts at 260 units (MAX_PATH; only 310 was tried).
- **The short link** (Release, Debug and ASan builds of this change; probes and raw results in
  `E:\FrostBuilds\perf\m0\longpaths-on\launcher-impl\junction\`, every run on a hidden or private desktop):
  - Premise (`pj_premise.json`): the python.exe control in a 300-unit directory dies with 0xC0000409 through its
    315-unit `\\?\` path and creates its device (hr 0x0) through a 140-unit junction to the same directory; the
    Release frost of 33a329a passes `--selftest=onscreen` on 'windows' through a 139-unit junction. The driver reads
    the path the process was started through, not the directory's real one.
  - File-system facts (`pj_fs.json`): a standard user may set a mount point on an empty directory (on a non-empty
    one: error 145) and replace one in place, and may set WSL symlinks on files and directories, but not an NT
    symbolic link (error 1314, no privilege or Developer Mode here). While a junction is held open with
    `FILE_SHARE_READ` only, a second reader opens it, a writer, a deleter and RemoveDirectoryW get error 32, the
    parent folder's rename gets error 5, traversal works, and after closing the rename succeeds. RemoveDirectoryW
    on a junction leaves its target's files. GetEnvironmentVariableW takes 49 us; loading shell32 and calling
    SHGetKnownFolderPath takes 3.3 ms in a fresh process.
  - Cost (`pj_cost.json`, `pj_cost2.json`, `pj_outliers.json`; Release; warm start-to-exit wall time with
    `launch_fast`, under `night.py measure-begin/-end`, PC in use: Discord, a terminal, two Claude sessions, system
    CPU 7-22 %; before the first lock night.py waited for another FrostIDE worktree's ctest run to finish). The
    stand-in frost_app.dll
    (`launcher_probe_app.cpp`, no Qt) isolates the restart: from a 300-unit image path (restart through the
    existing link) p50 13.2-15.7 ms against 6.6-7.9 ms when started through the link's own path, so +6.4 to +7.8 ms
    (paired p50 over 60 interleaved rounds in each of two sessions, `pj_cost` and `pj_cost2`; `pj_outliers` has 100
    rotated, unpaired rounds of four variants); in `pj_cost2`, 15 starts in a row per block give the same +7.7 ms. Two
    direct starts back to back (13.7 ms) cost what one restart costs (13.6 ms), so the restart is one more launcher
    start and the link check is within noise; creating the link on the first start adds about 0.6 ms (p50 14.2 ms).
    The real `--selftest=onscreen` (Qt, qwindows, Direct3D) from 300 units: p50 198.7 and 207.5 ms against 190.2
    and 197.1 ms through the link, so +8.5 to +10.3 ms (paired p50 +9.0 and +9.7 ms, 30 rounds each). Every
    variant, the direct start through the link included, has starts of about 150 ms more in 11-15 % of 100 rounds
    (`pj_outliers.json`); they belong to process creation on this machine, not to the link (a probe that reopened
    the link after each start never met a sharing violation, `pj_share.json`), and a restart, being two starts,
    meets them about twice as often. The waiting launcher holds 552 KiB private, 4.0 MiB working set and 52
    handles, and exits 1.2 ms (p50, 30 runs) after the restarted process.
  - Tests: `frost_process_tests` (31 tests, among them the 13 on-screen cells of
    `SelfTestOnScreenFromEveryImagePathLength`) and `app_startup_tests` (32) pass in Debug, Release and the ASan tree's RelWithDebInfo, each binary run on a
    hidden desktop (`frost_process_tests-*.xml`, `app_startup_tests-*.xml` there), and under `verify.ps1 -Quick` and
    `verify.sh` (the commit's footer). The whole on-screen matrix takes 3.2 s in Debug.
- **Review fixes of 2f40f9f** (two reviews; their probes `junction-review1\pr_hold.json` and
  `junction\review2_probe\retarget_probe.py` in the session scratch): a FILE_WRITE_ATTRIBUTES opener of a junction
  held as ShortLink holds it gets a handle, FSCTL_SET_REPARSE_POINT through it retargets the junction and
  FSCTL_DELETE_REPARSE_POINT strips it, while GENERIC_WRITE and DELETE openers get error 32;
  `HoldStopsRemovalButNotAReparseDataWriter` now repeats that on every run. The API set
  `api-ms-win-security-base-l1-1-0.dll` resolves to `C:\WINDOWS\System32\KERNELBASE.dll` with
  `LOAD_LIBRARY_SEARCH_SYSTEM32`, and this machine's token is TokenElevationTypeLimited (`apiset_probe.py`). After
  the fixes, `frost_process_tests` (31 tests) and `app_startup_tests` (37) pass in Debug, Release and the ASan
  tree's RelWithDebInfo under `verify.ps1 -Full` on a hidden desktop (win debug 415/415, release 418/418, asan
  419/419; wsl debug 335/335, asan 339/339, tsan 338/338): `launcher-impl\junction\review-fixes\` (`verify_full.log`,
  the Release and ASan ctest logs).
- **Review fixes**: the review's spelling probe, re-run against the fixed Debug launcher (scratch
  `fixcheck\fc_spelling-debug.jsonl`): `\\.\`, `..`, `.`, trailing-dot, doubled-separator and forward-slash spellings
  of the image path all pass `--selftest` and `--version`, where 4c5533d exited 4 for `\\.\` and failed
  process-hardening for the others. The single-image baseline at 240 units:
  `launcher-impl\review-fixes\baseline_67838d1_debug_240.txt`. After the fixes, `frost_process_tests` (24 tests)
  and `app_startup_tests` (25) pass in Debug, Release and the ASan tree's RelWithDebInfo under `verify.ps1 -Full`
  (`review-fixes\verify_full.log`; the Release and ASan ctest logs beside it, with the spelling probe's script and
  results).

## Consequences
- frost's command-line modes and its offscreen self-test start in place from every measured directory length (22
  lengths from 200 to 400 units, plain and `\\?\`, in Release, Debug and ASan; `StartsFromEveryDirectoryLength`).
  With long paths ineffective, which `StartsWithLongPathsIneffective` simulates by removing longPathAware from a
  copy of frost.exe's manifest, 8 of those lengths are tested (200, 218, 225, 236, 249, 260, 300 and 400, plain
  starts only up to 249): frost starts and passes every check except long-path-manifest, which reports the edited
  manifest, so the self-test exits 1 there as expected.
- The GUI and the on-screen self-test start from long image paths too: from 260 units on through the short link
  (decision 8), tested on private desktops from image paths of 250 to 400 units in both forms
  (`SelfTestOnScreenFromEveryImagePathLength`) and with the GUI's own window at 300 units
  (`GuiStartFromALongPathShowsItsWindow`). Some starts are refused with a message instead, among them install
  directories over about 4,089 units and elevated GUI or on-screen starts from 260 units (decision 8, *Security*;
  the full list is "New refusals", below). The M0 criterion is met for the IDE itself, and
  matrix row I7 is `green`. The driver's limit was confirmed on the interactive desktop; the restart was run on
  hidden and private desktops only, because a run on the user's desktop needs their approval at the PC. It works
  because the driver reads the path the process was started through, which the link makes short; it would not help
  against a limit on the directory's real path, which no driver showed. Update (2026-11-01, about 16:30): the
  restart has since run on the interactive desktop too. From a 310-unit image path started with CreateProcessW,
  the restarted GUI showed its window in the foreground (Measurements, "The restart on the interactive desktop").
- Residual limit, the shell's (the same on-screen check), partly inferred. Measured: one direct ShellExecuteW of the
  310-unit frost.exe returned 5, and frost did not start (no window, no junction). Inferred, not measured: that the
  path's length causes the 5 (no shorter-path ShellExecuteW was run as a control), that the limit starts at 260
  units, and that Explorer and shortcuts, which start programs through the shell, fail the same way (neither was
  tried; Measurements). If that holds, a frost installed that deep cannot be started from Explorer, a shortcut or
  anything else that goes through ShellExecuteW, and since that happens before frost runs, no fix in frost reaches
  it. A terminal or script start (CreateProcessW) works, through the restart. Registrations that go through the
  shell (file associations and shortcuts, I10 in M2) would inherit the limit.
- A start from 260 units or more that may load a GPU driver costs about 7 ms more (one more launcher start;
  about 10 ms for the on-screen self-test, against the about 5 ms the user's option named) and creates
  `%LOCALAPPDATA%\FrostIDE\i\<hash>` on first use. Starts from
  shorter paths, and the command-line modes at any length, pay nothing. While frost runs, Task Manager shows two
  frost.exe processes; the waiting one holds about 0.5 MiB private memory. Ending the waiting one leaves frost
  running (unless the parent's job ends both); ending frost ends the waiting one with frost's status.
- After a restart, GetModuleFileNameW, Qt's application directory and every path frost derives from them name the
  link, not the install directory; only the self-test's `image-path` check and the GUI's start-up failure message
  name the real one. TODO(M1) in `main.cpp`: code that starts frost again, writes logs or crash dumps, or registers
  frost (file associations, I10) must take the install directory from `restartedFrom`; and the taskbar may group a
  restarted frost apart from a pinned shortcut to the long path until frost sets an explicit AppUserModelID.
- New refusals, each with a message and status 4 instead of the driver's silent crash: from 260 units, a GUI or
  on-screen start that runs elevated, or with LOCALAPPDATA unset, relative, on a network drive or of 222 units or
  more; an install directory on a network share, a mapped drive or a volume without a drive letter, or of more than
  about 4,089 units (from 260 units none of these could run the GUI before either); and something at the link's
  path that frost does not remove.
- The link is only as safe as the folder it lives in: the user's own programs can retarget it even while frost
  holds it (decision 8, *Security*), which is why elevated starts refuse it.
- Links of deleted or moved installs stay behind (TODO(M2), decision 8).
- Two images to deploy: frost_app.dll must sit beside frost.exe; without it, or without one of its DLLs, the
  launcher reports whether frost_app.dll (or the ASan runtime) is missing or loads but lacks a DLL it needs, and
  exits with 4 instead of the loader's 0xC0000135. It cannot name the missing import: the loader does not say.
- Each start costs about 0.1-1.0 ms more for the launcher (Measurements), on top of a B1 that already fails; on
  screen the launcher measured -1.9 ms against the single image, within noise.
- Late loads by bare name search the `\\?\` install directory and System32 only (`USER_DIRS | SYSTEM32`). A DLL
  beside frost.exe therefore loads from a long plain start (`LateLoadOfADllBesideTheExeWorksFromALongPlainPath`);
  under DEFAULT_DIRS it failed with 206 (spike fact 18).
- The launcher uses the release CRT in every configuration, so the Debug launcher has no CRT debug checks: it is a
  dozen loader calls, and the debug CRT's own assertion box would be out of `suppressCrashDialogs()`'s reach (spike
  fact 5).
- The ASan tree's Debug configuration is unusable (above); verification uses RelWithDebInfo.
- Not tested automatically: the launcher's own GUI-mode message box (its refusals are tested in a command-line
  mode, where they go to stderr), the console attach of a command-line restart without redirection (no test starts
  frost in a terminal), the elevated refusal (no test can elevate), STARTF_USEMONITOR from a real taskbar start
  (only its rule is tested), and anything on the interactive desktop, such as whether a restarted GUI started from
  Explorer or a shortcut comes to the foreground: its window is created by a process that the waiting launcher
  started, not by the one Explorer started. If it does not, the remedy is AllowSetForegroundWindow for the
  restarted process through a lazily loaded user32 in GUI restarts; that needs an on-screen run, which needs the
  user's approval at the PC. (Update, 2026-11-01 about 16:30: the on-screen run is done. A CreateProcessW start
  from 310 units, as a terminal or a script makes it, brought the restarted window to the foreground, so
  AllowSetForegroundWindow is not needed there. A direct ShellExecuteW of the 310-unit frost.exe returned 5 and
  never reached frost; that Explorer and shortcut starts from that length fail the same way is inferred, not
  measured (Measurements). Neither run is an automated test.) The GUI start (`frost` without
  arguments) is
  tested on a private desktop from 300 units, its window and its failure box both; the native platform plugin
  through `--selftest=onscreen` up to 400 units. The spike did not cover the native platform at all (its README
  lists it as not measured); spike fact 15 is the offscreen self-test, and only probe fact 27 (qtprobe, not frost)
  loaded the windows platform after a preload.
- The GUI keeps Windows Error Reporting and its default error mode after the launcher's loads; a broken plugin that
  frost_app.dll pins in a GUI start can still raise the loader's own box before frost's message.
- TODO(M1): log a failed non-platform preload in the GUI once frost::core has logging (`src/app/src/main.cpp`).
- PLAN.md's M0 status paragraph (section 7) and its "Next step" note still say that the launch over 260 characters is
  not met while LongPathsEnabled=0 and that one ASan test fails; both are left to the branch that edits PLAN.md.
  (Done 2026-11-01 in M0's exit record on `m0/foundation`: both now say that the criterion is met.)

## User decisions (2026-11-01, evening)
- **The restart's cost:** about 10 ms against the 5 ms the option named: "It's OK".
- **Elevated starts from 260+ paths:** "Keep refusal". Decision 8's refusal stands, and no administrator-owned link
  location is built.
- **No further work on long install paths:** "If this 260+ limit is a hard limit on windows for built in windows
  things we shouldn't be putting this much resources into circumventing it". The Windows shell cannot launch from
  such a path, so this support is frozen as built. The launcher and the restart stay (built, reviewed, and about
  2.3 s of a Windows test run), but no more work goes into install directories of 260+ units. A future failure
  there gets a clear message rather than a new workaround. Long paths of project files are ordinary file I/O and are
  not affected.

## R5 acceptance qualification (2026-11-13)

The current Windows runtime can refuse a restart through a short junction backed by a physically
long installation with `ERROR_ACCESS_DENIED` (5). The launcher reports that error and advises an
installation whose frost.exe path is shorter than 260 UTF-16 units, implementing the frozen-support
decision above. The physical-long GUI tests require either the original successful window/handler
behaviour or that exact parent-owned modal refusal, status 4, preserved install identity and released
junction; crashes, timeouts and other errors fail.

Positive native-plugin launch-spelling coverage at 260, 261, 300 and 400 units uses real directory
junctions to a short physical installation, in plain and extended forms. The missing-plugin GUI case
and a separate mandatory crash-handler case use the same fixture, retaining successful restart,
window/error ownership and handler cleanup assertions. These tests do not claim that physically
long installs work on this runtime, and add no product workaround or security exception.

## Alternatives considered
- **Strip RT_MANIFEST 2 from the deployed DLLs** (design B; probe 10; for the ASan runtime alone, branch
  `m0/e4-asan-runtime`): works, but modifies vendor binaries (their Authenticode signatures must go) and must be
  redone for every Qt update. Rejected by the user's decision.
- **Re-execute frost through a `\\?\` image path** (a launcher hop): a second process start costs 4-6 ms (fact 29),
  and the `\\?\` image path alone does not get Qt's plugins loaded (fact 16).
- **Start through the 8.3 short name** (E1 (d), and the second decision's "Use 8.3 short names"): depends on the
  volume; E: generates none (fact 18), nor do Dev Drive and ReFS, and a deep enough path stays 260 units long even
  in 8.3 form. Rejected by the user's second decision.
- **Accept the driver's limit as a known limitation** (the second decision's "Known limitation"): the M0 criterion
  would stay unmet for the IDE. Rejected by the user's second decision.
- **Other short aliases for the restart.** A symbolic link needs SeCreateSymbolicLinkPrivilege or Developer Mode
  (error 1314 here). A drive letter (`subst`, DefineDosDeviceW) is visible to every process of the logon session,
  can collide with the user's own letters and needs cleaning up. Hard links of frost.exe and its DLLs in a short
  folder would mean dozens of links per install, fail across volumes (LOCALAPPDATA on C:, the install on E:) and
  go stale on update; a copy of the install costs far more than 7 ms. A junction needs no privilege, is one
  object per install, points across local volumes, and is checked on every use.
- **A link the hold makes immutable, or a protected link for elevated starts.** No share mode stops an opener that
  asks for FILE_WRITE_ATTRIBUTES alone. A DACL on the link that denies the user write access (with an OWNER RIGHTS
  entry, so ownership does not restore it) would stop the user's programs while frost holds the link, but the
  user's FILE_DELETE_CHILD on the folder lets them replace it whenever no frost holds it, so every start would also
  have to verify the owner and DACL through security APIs the launcher does not import; an administrator-owned
  folder for elevated starts would need the same checks and a parent that non-elevated programs cannot rename.
  Elevated starts from long paths are rare, so they refuse instead (decision 8).
- **SHGetKnownFolderPath for LOCALAPPDATA**: 3.3 ms of shell32 in a KERNEL32-only launcher, with no security gain
  (the parent controls the environment it would read the profile path from anyway; decision 8).
- **Exit right after the restart instead of waiting**: the parent would see frost end at once, losing the exit
  status and the ends of the standard streams (scripts, the tests, `frost --selftest=onscreen`), and B20 would time
  a process that is already gone. Waiting costs 0.5 MiB and 1.2 ms at exit.
- **Change what the driver reads instead of the path** (rewriting the process's own image path in its PEB before
  Qt loads): undocumented, and other components read the same field. Rejected without a spike.
- **Delay-load Qt in frost.exe**: lld-link refuses, because frost imports data from all three Qt DLLs (fact 31).
- **QT_FATAL_WARNINGS=1 against Qt's box**: helps only when no plugin is found at all, and makes every warning fatal
  (fact 22). The dialog guard and fatal handler cover every case without global effects.
- **Trust LongPathsEnabled**: the switch does not move the loader's limit (fact 1), and frost cannot set it.
- **Use `\\?\` only beyond a length threshold**: equal in cost within noise (spike facts 21-22), and the threshold
  hangs on a hand-kept constant (the longest deployed DLL name).
- **Design A as first written**: `/DEPENDENTLOADFLAG:0xA00` on frost_app.dll breaks plain starts from 236 units, and
  its preload flags (`DLL_LOAD_DIR | DEFAULT_DIRS`) lose plugins whose dependencies sit beside frost.exe (spike facts
  7 and 16).
