// SPDX-License-Identifier: Apache-2.0
//
// Process-wide start-up steps and checks that frost runs before Qt's GUI exists (PLAN.md
// section 3.5 and inventory item I7):
//   - hardenProcess() and the rest of frost/app/startup_core.hpp: the DLL and executable
//     search hardening, which the Windows launcher (frost.exe) applies before frost_app.dll
//     loads;
//   - suppressCrashDialogs(), guardCommandLineDialogs() and installCommandLineFatalHandler()
//     keep command-line modes from hanging on a modal box: CRT and Windows Error Reporting
//     dialogs, any dialog window on the main thread (Qt's unsuppressible Release "no Qt
//     platform plugin" box among them), and Qt's fatal errors, which end the process with
//     status 4 instead;
//   - preparePlatformPlugin() loads (pins) the Qt platform plugin through its \\?\ path before
//     QApplication exists, or reports why it cannot, so Qt's own load of a plugin frost pinned
//     cannot fail. When QT_PLUGIN_PATH, QT_QPA_PLATFORM_PLUGIN_PATH or qt.conf may point Qt
//     elsewhere and no candidate exists, nothing is pinned and Qt may still fail (the
//     command-line guards then end the process with status 4, FrostProcess.SelfTestNeverShowsQtsDialog);
//     preloadOtherPlugins() pins the other deployed plugins when their paths are long.
//
// Why the pin (measured on Windows 11 26300 with Qt 6.12.0, LongPathsEnabled=0 and =1; ADR
// 0012): the Windows loader refuses a plain path longer than kLoaderPlainPathLimitUnits for
// a DLL that embeds an isolation-aware manifest (RT_MANIFEST resource 2), as Qt's DLLs and
// plugins do, whatever LongPathsEnabled says; the \\?\ form loads at any length. Qt strips
// \\?\ from plugin paths and loads plugins through their plain path, but the loader returns
// a module that is already mapped from the same file, so a plugin pinned through \\?\ first
// satisfies Qt's later plain-path load (probe 7; StartupLoadPlatformPlugin.
// PinnedPluginSatisfiesQtsPlainPathLoad). The DLLs themselves are never modified.
//
// All of them are no-ops or trivially successful on Linux. Its dynamic loader does not
// search the current directory by itself, but an empty or relative element in
// LD_LIBRARY_PATH (a stray leading or trailing ':', or '.') makes it do so, and the same
// holds for PATH in execvp-style lookups.
// TODO(M2): the trust work (inventory I8) decides whether start-up or the launcher drops
// such elements from LD_LIBRARY_PATH and PATH.
#pragma once

#include "frost/app/startup_core.hpp"
#include "frost/core/error.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

/*
 * Makes crashes and aborts exit with a status instead of showing a dialog: no
 * abort() message box, no Windows Error Reporting UI, no critical-error boxes, and
 * debug CRT reports (warnings, errors, assertions; Qt's debug qFatal uses them) go to
 * stderr. For the command-line modes (--version, --help, --selftest and usage errors),
 * which tests and scripts run unattended; the GUI keeps the default behaviour until the
 * crash handler exists (M1). It reaches the CRT that frost_app.dll uses (the shared UCRT),
 * not the launcher's static one, which is why the launcher uses the release CRT in every
 * configuration. Main thread, before other threads start. The SetErrorMode flags are
 * inherited by child processes; a launcher that wants a child to report to Windows Error
 * Reporting passes CREATE_DEFAULT_ERROR_MODE. No-op on non-Windows platforms.
 */
void suppressCrashDialogs();

/*
 * Refuses every dialog window (window class "#32770", which MessageBoxW and Qt's Release "no
 * Qt platform plugin could be initialized" box use) that the calling thread tries to create:
 * a thread-local WH_CBT hook fails its HCBT_CREATEWND, so MessageBoxW returns 0 at once, and
 * writes "frost: refused to show a dialog in a command-line mode: <title>" to stderr. Every
 * other window is left alone. For the command-line modes only, on the main thread (where Qt
 * creates its platform integration and that box), installed for the rest of the process.
 * Returns whether the hook is installed; always true on non-Windows platforms.
 */
[[nodiscard]] bool guardCommandLineDialogs();

/*
 * Installs a Qt message handler for the command-line modes: a QtFatalMsg writes
 * "frost: Qt fatal error: <message>" to stderr, flushes stdout and stderr, restores the
 * console output code page `*consoleCodePageToRestore` when the pointer is set and the value
 * is not 0 (main's ConsoleCodePageRestorer::previous), and ends the process with status 4
 * (std::_Exit, no atexit handlers), where Qt would abort with 0xC0000409 or, in a Debug build,
 * show the CRT's Abort/Retry/Ignore box. Every other message goes to the handler that was
 * installed before, unchanged. Call it on the main thread before QApplication exists; a later
 * call replaces the code-page pointer only (nullptr clears it, so frost_main() can drop the
 * pointer before its frame ends). Not for the GUI, which keeps Qt's behaviour until M1's crash
 * handler.
 */
void installCommandLineFatalHandler(const unsigned* consoleCodePageToRestore);

/*
 * The longest plain path, in UTF-16 code units, from which the Windows loader loads a DLL that
 * embeds RT_MANIFEST resource 2 (all of Qt's DLLs and plugins, clang's ASan runtime): 250
 * loads and 251 fails with ERROR_FILENAME_EXCED_RANGE, with LongPathsEnabled=0 and =1 alike,
 * in legacy and hardened search modes (measured 2026-11-01 on Windows 11 26300; ADR 0012).
 * DLLs without that resource load at any plain length. frost loads every DLL of its own
 * through \\?\ and pins the platform plugin, so the limit no longer decides whether frost
 * starts; it only decides whether preloadOtherPlugins() has work to do. A lower real limit
 * would make Qt skip plugins between it and this value silently, so
 * StartupLoaderLimit.PlainPathOfAManifestDllLoadsUpTo250Units measures it on every test run
 * with the SDK's unmodified offscreen plugin (250 loads, 251 fails with 206).
 */
inline constexpr std::size_t kLoaderPlainPathLimitUnits = 250;

// Whether a plugin `longestRelativeUnits` long (relative to the install directory, e.g.
// "imageformats\qsvg.dll") lies beyond kLoaderPlainPathLimitUnits in a directory `dirUnits`
// long: dirUnits + 1 + longestRelativeUnits > the limit. Pure; O(1).
[[nodiscard]] bool pluginPathExceedsLoaderLimit(std::size_t dirUnits, std::size_t longestRelativeUnits) noexcept;

// The platform plugins Qt tries, in order, for a QT_QPA_PLATFORM value: its ';'-separated
// entries without their ':'-separated options, skipping empty ones, or "windows" (Qt's
// default on Windows) when none is left. frost's command line rejects Qt's -platform
// option, and frost passes Qt no user arguments, so the variable is the only way to
// choose another one. O(length of the value).
[[nodiscard]] std::vector<std::string> platformPluginNames(std::string_view qpaPlatform);

/*
 * The files Qt would load for `platforms` (from platformPluginNames()) from the plugin
 * directories `dirs` (absolute, without a \\?\ prefix, in Qt's search order): <dir>\q<p>.dll,
 * or q<p>d.dll when `debugQt` (Qt loads only plugins of its own build), ordered platform-major
 * and directory-minor, as Qt tries them. Pure; O(dirs x platforms).
 */
[[nodiscard]] std::vector<std::u16string>
platformPluginCandidates(std::span<const std::u16string> dirs, std::span<const std::string> platforms, bool debugQt);

// The report for a missing platform plugin: "Qt's platform plugin \"offscreen\" is missing:
// <dir>\platforms holds no qoffscreen.dll or qoffscreend.dll. Reinstall FrostIDE or ...".
// `dir` is the install directory; unpaired surrogates in it become U+FFFD. Pure.
[[nodiscard]] std::string missingPlatformPluginMessage(std::span<const std::string> platforms, std::u16string_view dir);

/*
 * Loads the first of `candidates` (from platformPluginCandidates()) that exists, through its
 * \\?\ path, with LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32: its imports
 * resolve from the launcher's \\?\ install directory and System32, never from a plain long
 * application directory, the current directory or PATH. A candidate that GetFileAttributesW
 * does not find is skipped. The module is never freed: the pin makes Qt's later plain-path load
 * of the same file return it. Returns the loaded candidate.
 * Errors: NotFound when no candidate exists; IoError "Qt's platform plugin \"<p>\" cannot be
 * loaded from <path>: <system text> (win32 error N)" for the first candidate that exists but
 * does not load (later candidates are not tried, because Qt would stop there too).
 * Preconditions: hardenProcess() ran; the dependencies of the plugin beside frost.exe are
 * reachable through the launcher's AddDllDirectory entry or already loaded. Main thread,
 * before QApplication exists. Unsupported on non-Windows platforms.
 */
[[nodiscard]] Result<std::u16string> loadPlatformPlugin(std::span<const std::u16string> candidates,
                                                        std::span<const std::string> platforms);

#ifdef _WIN32
/*
 * The plugin directory that a QT_QPA_PLATFORM_PLUGIN_PATH value names, in the form
 * preparePlatformPlugin() makes its first candidates from, or "" when `value` is empty or not
 * absolute (a relative value is Qt's to resolve, and frost pins nothing from it). A \\?\ or
 * //?/ prefix is stripped and '/' becomes '\'; an absolute value is a drive path ("X:\...") or
 * a UNC or \\.\ path. It is then normalised as Win32 and Qt resolve it ("." and "..",
 * repeated separators, trailing dots and spaces; GetFullPathNameW), because the pin goes
 * through \\?\, which takes all of them literally; a \\.\ prefix is dropped, and trailing
 * separators are removed except after a drive root ("C:\"). O(length); no file system access.
 */
[[nodiscard]] std::u16string platformPluginOverrideDirectory(std::u16string_view value);
#endif

// What preparePlatformPlugin() reports when Qt can start.
struct PlatformPluginReport {
    std::string detail; // For the self-test's plugin-paths check. Always set.
};

/*
 * Pins the platform plugin Qt is about to use (platformPluginNames() of QT_QPA_PLATFORM, so
 * call it after the platform is chosen) before QApplication exists. The candidates come first
 * from QT_QPA_PLATFORM_PLUGIN_PATH (platformPluginOverrideDirectory(), when it names an
 * absolute path), then from <install dir>\platforms, where the install directory is
 * frost.exe's, as the launcher loads from it (extendedDirectoryOf(): normalised, 8.3 short
 * components kept, since Qt derives its plugin path from the same form,
 * FrostProcess.RunsThroughShortNamesOfALongPath).
 * The plugin's build ("d" suffix or not) follows the Qt6Core that is loaded.
 * Details: "loaded Qt's platform plugin <path>"; when no candidate exists but QT_PLUGIN_PATH,
 * QT_QPA_PLATFORM_PLUGIN_PATH or <install dir>\qt.conf may point Qt elsewhere, "platform
 * plugin \"<p>\" not checked: ..." (Qt then reports a missing plugin itself, and the
 * command-line guards keep that from showing a box).
 * Errors: NotFound with missingPlatformPluginMessage(); IoError from loadPlatformPlugin(), or
 * when frost.exe's path cannot be determined.
 * Budget: B1. The cost is one or two GetFileAttributesW and the LoadLibraryExW that Qt would
 * make anyway; no directory listing. Main thread, after the last change to QT_QPA_PLATFORM and
 * before QApplication exists. On non-Windows platforms it loads nothing and reports "not
 * checked on this platform: Qt reports a missing plugin itself".
 */
[[nodiscard]] Result<PlatformPluginReport> preparePlatformPlugin();

// What preloadOtherPlugins() did.
struct PluginPreloadReport {
    bool ok = true;     // False when a plugin that needed the preload did not load.
    std::string detail; // For the self-test's other-plugins check. Always set.
};

/*
 * The other deployed plugins (DLLs in a first-level subdirectory of the install directory
 * other than platforms\: styles, imageformats, ...) are best-effort: Qt skips one it cannot
 * load. When one of their plain paths lies beyond kLoaderPlainPathLimitUnits
 * (pluginPathExceedsLoaderLimit(), over every listed DLL), each DLL whose import directory
 * names the Qt6Core this process loaded (Qt6Core.dll or Qt6Cored.dll) is loaded through its
 * \\?\ path with loadPlatformPlugin()'s flags, so Qt's later plain-path load finds it mapped,
 * and its dependencies beside frost.exe resolve in \\?\ form (qsvg needs Qt6Svg, for one).
 * The others (a plugin of Qt's other build, a DLL that is no Qt plugin) are skipped and
 * named in the detail, because Qt would refuse them and loading them would bring in a second
 * Qt or run foreign code. A file that cannot be read or loaded is reported (ok false), never
 * fatal: Qt would skip that plugin silently (FrostProcess.PreloadFailureOnlyWarns).
 * Otherwise nothing is loaded and Qt loads them as usual. Budget: B1. The cost on a normal
 * install is one listing of the install directory and one per plugin directory; today only
 * platforms\ is deployed, so this lists one directory and loads nothing. Beyond the limit
 * each DLL is also mapped once as a data file for its imports. Same threading rule as
 * preparePlatformPlugin(). Does nothing on non-Windows platforms.
 */
[[nodiscard]] PluginPreloadReport preloadOtherPlugins();

// Whether this process can use paths beyond MAX_PATH without the \\?\ prefix
// (RtlAreLongPathsEnabled: needs both LongPathsEnabled=1 and the longPathAware manifest
// entry). Always true on non-Windows platforms. Thread-safe; O(1).
[[nodiscard]] bool longPathsEffective() noexcept;

} // namespace frost::app
