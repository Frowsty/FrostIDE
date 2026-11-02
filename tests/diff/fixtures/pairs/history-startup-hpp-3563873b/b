// SPDX-License-Identifier: Apache-2.0
//
// Process-wide start-up hardening and checks that main() runs before Qt exists
// (PLAN.md section 3.5 and inventory item I7):
//   - hardenProcess() stops DLL searches from looking in the current directory, where an
//     opened project could plant a binary, and stops CreateProcessW and cmd.exe from
//     resolving bare executable names there (SearchPathW still does; see ProcessHardening);
//   - suppressCrashDialogs() keeps command-line modes from hanging on modal
//     CRT or Windows Error Reporting dialogs;
//   - checkPluginPaths() reports, before Qt aborts with a less helpful message
//     (or, in a Debug build, a CRT Abort/Retry/Ignore box), that the Qt platform plugin
//     frost will use is missing or sits on a path too long for Qt to load it.
// All of them are no-ops or trivially successful on Linux. Its dynamic loader does not
// search the current directory by itself, but an empty or relative element in
// LD_LIBRARY_PATH (a stray leading or trailing ':', or '.') makes it do so, and the same
// holds for PATH in execvp-style lookups.
// TODO(M2): the trust work (inventory I8) decides whether start-up or the launcher drops
// such elements from LD_LIBRARY_PATH and PATH.
#pragma once

#include "frost/core/error.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

/*
 * What hardenProcess() applied. Each field is true when its step is in effect after the
 * call: the call succeeded (for safeSearchPathMode, or the safe mode was already
 * permanent), or, for exeSearchSkipsCurrentDirectory, NeedCurrentDirectoryForExePathW
 * confirms that the variable takes effect.
 *
 * Trade-off of the DLL rules, as in Chromium and Firefox: PATH is no longer searched for
 * DLLs, and LoadLibraryExW(bare name, LOAD_WITH_ALTERED_SEARCH_PATH) fails with
 * ERROR_INVALID_PARAMETER. Third-party code loaded into frost (IMEs, shell extensions)
 * that relies on either breaks. A DLL loaded by absolute path with
 * LOAD_WITH_ALTERED_SEARCH_PATH, as COM loads in-process servers and shell extensions,
 * still finds its dependencies in its own directory.
 */
struct ProcessHardening {
    // SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS): LoadLibrary by name, and
    // the dependencies of a DLL loaded by absolute path, are searched only in the
    // application directory, System32 and AddDllDirectory entries.
    bool defaultDllDirectories = false;
    // SetDllDirectoryW(L""): removes the current directory from the legacy search orders,
    // including the one LOAD_WITH_ALTERED_SEARCH_PATH uses. While defaultDllDirectories is
    // in effect it is redundant (measured on Windows 11 26300); it is the fallback if
    // SetDefaultDllDirectories fails, and unlike it, any later SetDllDirectoryW call undoes it.
    bool dllDirectoryCleared = false;
    // SetSearchPathMode(safe | permanent): SearchPathW looks in the current directory after
    // the system directories but still before PATH, so a planted tool.exe still wins over
    // the one on PATH. Tool resolution must therefore never use SearchPathW: frost-lint's
    // exe-search rule bans it, and tools resolve to absolute paths (PLAN.md 3.5).
    bool safeSearchPathMode = false;
    // NoDefaultCurrentDirectoryInExePath=1: CreateProcessW with a NULL application name,
    // and cmd.exe in child processes (which inherit the variable), resolve bare executable
    // names without the current directory. It does not affect SearchPathW.
    bool exeSearchSkipsCurrentDirectory = false;

    // True when every step applied (always true on non-Windows platforms).
    [[nodiscard]] bool complete() const noexcept;
};

/*
 * Applies the hardening above to the whole process. Call it first in main(), on
 * the main thread, before any other thread starts or library loads a DLL: the
 * calls change process-global state that is not synchronised. Idempotent.
 * SetDefaultDllDirectories and the permanent search-path mode cannot be undone for the
 * life of the process; the SetDllDirectoryW setting can be, which is harmless while
 * SetDefaultDllDirectories holds.
 * The DLLs frost links directly are already loaded when main() runs; their search
 * is restricted at link time instead (/DEPENDENTLOADFLAG, see src/app/CMakeLists.txt),
 * which is why they must stay next to frost.exe.
 * Child processes inherit NoDefaultCurrentDirectoryInExePath through the environment
 * (see the TODO(M2) on ProcessSpec::environment in frost/io/process.hpp); the DLL
 * settings are not inherited.
 * On non-Windows platforms it does nothing and returns a complete state.
 */
[[nodiscard]] ProcessHardening hardenProcess();

/*
 * Makes crashes and aborts exit with a status instead of showing a dialog: no
 * abort() message box, no Windows Error Reporting UI, no critical-error boxes, and
 * debug CRT reports (warnings, errors, assertions; Qt's debug qFatal uses them) go to
 * stderr. For the command-line modes (--version, --help, --selftest and usage errors),
 * which tests and scripts run unattended; the GUI keeps the default behaviour until the
 * crash handler exists (M1). Same threading rule as hardenProcess(). The SetErrorMode
 * flags are inherited by child processes; a launcher that wants a child to report to
 * Windows Error Reporting passes CREATE_DEFAULT_ERROR_MODE. No-op on non-Windows platforms.
 */
void suppressCrashDialogs();

/*
 * The longest plugin path, in UTF-16 code units, that Qt can load while long paths are not
 * in effect. Measured on Windows 11 26300 with LongPathsEnabled=0 and Qt 6.12.0 (Debug) by
 * pointing QT_QPA_PLATFORM_PLUGIN_PATH at padded directories: a 250-unit plugin path loads
 * and 251 does not, whatever the lengths of the directory and the file name. That is below
 * MAX_PATH (259 units plus NUL). The limit is the Windows loader's, not Qt's: LoadLibraryW
 * alone fails with ERROR_FILENAME_EXCED_RANGE beyond 250 units for a DLL that embeds an
 * isolation-aware manifest (RT_MANIFEST resource 2), as Qt's DLLs and plugins do, while a
 * DLL without one (System32\version.dll) still loads at 300 units. Whether
 * LongPathsEnabled=1 lifts the limit has not been measured. While LongPathsEnabled=0,
 * FrostProcess.PlatformPluginPathBoundary checks that Qt loads a 250-unit plugin and that
 * frost refuses 251; measure again when Windows or Qt is upgraded.
 */
// TODO(M1): on a plain launch the same limit kills frost before main() (0xC0000106, no message) once one
// of frost.exe's static imports beside it passes it: directories of 236-249 units in Release, 235-249 in
// Debug (ADR 0003, Consequences). This check never runs there. Question E4, option (b), in docs/progress.md.
inline constexpr std::size_t kQtPluginPathLimitUnits = 250;

// The platform plugins Qt tries, in order, for a QT_QPA_PLATFORM value: its ';'-separated
// entries without their ':'-separated options, skipping empty ones, or "windows" (Qt's
// default on Windows) when none is left. frost's command line rejects Qt's -platform
// option, and main() passes Qt no user arguments, so the variable is the only way to
// choose another one. O(length of the value).
[[nodiscard]] std::vector<std::string> platformPluginNames(std::string_view qpaPlatform);

// What checkPluginPaths() reports when Qt can start.
struct PluginPathReport {
    // What was checked, for the self-test's report. Always set.
    std::string detail;
    // Non-empty when Qt will skip other plugins (not the platform plugin it needs) because
    // their paths are too long; Qt starts without them.
    std::string warning;
};

/*
 * Pure part of checkPluginPaths(). `pluginPaths` are the DLLs one directory below the
 * executable's directory `dir` (absolute, without a \\?\ prefix); `platforms`
 * comes from platformPluginNames(). A plugin for platform p is a DLL named q<p>.dll or
 * q<p>d.dll (Qt's debug builds) directly inside a directory named "platforms", ASCII
 * case-insensitive. These are the names of Qt's own plugins; Qt itself matches plugins on
 * their metadata, so a renamed plugin is not recognised here.
 * Errors:
 *   - NotFound when `requirePresence` and no plugin exists for any of `platforms`. Callers
 *     pass false when QT_PLUGIN_PATH, QT_QPA_PLATFORM_PLUGIN_PATH or a qt.conf may point Qt
 *     elsewhere;
 *   - Unsupported when `longPathsEffective` is false and every plugin for `platforms` is
 *     longer than kQtPluginPathLimitUnits; the message names the shortest one.
 * Other DLLs beyond the limit only produce PluginPathReport::warning. Lengths count UTF-16
 * code units, the unit Win32 paths are measured in. O(total length of the paths).
 */
[[nodiscard]] Result<PluginPathReport> evaluatePluginPaths(std::span<const std::u16string> pluginPaths,
                                                           std::span<const std::string> platforms,
                                                           std::u16string_view dir, bool requirePresence,
                                                           bool longPathsEffective);

/*
 * Checks that Qt will be able to load the platform plugin it is about to use
 * (platformPluginNames() of QT_QPA_PLATFORM, so call it after main() has chosen the
 * platform). Lists the executable's directory and each of its N subdirectories for DLLs:
 * 1 + N small directory listings on the start-up path (Budget: B1), then applies
 * evaluatePluginPaths(). Presence is not required when QT_PLUGIN_PATH,
 * QT_QPA_PLATFORM_PLUGIN_PATH or a qt.conf next to the executable may point Qt elsewhere.
 * Without this check Qt aborts with "no Qt platform plugin could be initialized".
 * The directory is measured as GetModuleFileNameW reports it, 8.3 short components
 * included, because Qt derives its plugin paths from the same form: frost started through
 * the short name of a long directory does start (FrostProcess.RunsThroughShortNamesOfALongPath).
 * Errors: as evaluatePluginPaths(), and IoError if the executable's path cannot be
 * determined. Always succeeds on non-Windows platforms, where Qt reports a missing plugin
 * itself.
 * Thread-safe, but it answers for the environment at the time of the call, so call it on
 * the main thread after the last change to QT_QPA_PLATFORM and before QApplication exists.
 */
[[nodiscard]] Result<PluginPathReport> checkPluginPaths();

// Whether this process can use paths beyond MAX_PATH without the \\?\ prefix
// (RtlAreLongPathsEnabled). Always true on non-Windows platforms. Thread-safe; O(1).
[[nodiscard]] bool longPathsEffective() noexcept;

} // namespace frost::app
