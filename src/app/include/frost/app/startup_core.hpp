// SPDX-License-Identifier: Apache-2.0
//
// The part of frost's start-up that both halves of the Windows executable run (ADR 0012):
// frost.exe, the launcher (src/app/src/launcher.cpp), and frost_app.dll, the application it
// loads. It holds the process hardening, the hand-over of its result across the DLL boundary,
// the launcher's DLL search order for the install directory, and the path helpers they need.
// The source (src/startup_core.cpp) is compiled twice: into frost.exe with the static CRT
// (/MT, every configuration) and into frost::app with the shared one (/MD), because one static
// library cannot serve both (lld-link's /failifmismatch on RuntimeLibrary). So it uses only the
// C++ standard library and Win32: no Qt, no frost::core, nothing that needs another library's
// objects. On Linux there is no launcher; hardenProcess() runs at the top of main() and the
// Windows-only declarations below do not exist.
#pragma once

#include <string>

namespace frost::app {

/*
 * What hardenProcess() and the launcher applied. Each field is true when its step is in
 * effect: the call succeeded (for safeSearchPathMode, or the safe mode was already
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
    // application directory, System32 and AddDllDirectory entries. The launcher narrows
    // it further (installDirectoryExtended).
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
    /*
     * Windows, set by the launcher only (searchInstallDirectoryExtended()): the install
     * directory is an AddDllDirectory entry in its \\?\ form, and the process default search
     * order is LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32 instead of
     * DEFAULT_DIRS. A later LoadLibrary by name of a DLL beside frost.exe then resolves in
     * \\?\ form, which the loader's 250-unit plain-path limit for DLLs embedding RT_MANIFEST
     * resource 2 does not apply to. Under DEFAULT_DIRS the plain application directory is
     * searched first, and its failure (ERROR_FILENAME_EXCED_RANGE) ends the search (measured
     * 2026-11-01 at 300 units; ADR 0012). Both orders exclude the current directory and PATH.
     * False when AddDllDirectory or SetDefaultDllDirectories failed; DEFAULT_DIRS then stays.
     */
    bool installDirectoryExtended = false;

    // True when the four hardenProcess() steps applied (always true on non-Windows
    // platforms). installDirectoryExtended is reported separately.
    [[nodiscard]] bool complete() const noexcept;
};

/*
 * Applies the first four steps above to the whole process. On Windows the launcher calls it
 * first in wmain(), before any DLL other than ntdll, kernel32 and kernelbase is mapped
 * (frost.exe imports only KERNEL32.dll; FrostProcess.LauncherImportsOnlyKernel32 pins that),
 * on the main thread, before any other thread starts: the calls change process-global state
 * that is not synchronised. On Linux main() calls it first. Idempotent.
 * SetDefaultDllDirectories and the permanent search-path mode cannot be undone for the
 * life of the process; the SetDllDirectoryW setting can be, which is harmless while
 * SetDefaultDllDirectories holds.
 * Child processes inherit NoDefaultCurrentDirectoryInExePath through the environment
 * (see the TODO(M2) on ProcessSpec::environment in frost/io/process.hpp); the DLL
 * settings are not inherited.
 * On non-Windows platforms it does nothing and returns a complete state.
 */
[[nodiscard]] ProcessHardening hardenProcess();

// The hardening state as the bit set the launcher passes to frost_main(), and back. Bits
// that no field uses are ignored by decodeHardening(). Pure; O(1).
[[nodiscard]] unsigned encodeHardening(const ProcessHardening& hardening) noexcept;
[[nodiscard]] ProcessHardening decodeHardening(unsigned bits) noexcept;

#ifdef _WIN32

// frost_app.dll's file name and entry point, the contract between the launcher and the DLL:
//   extern "C" int frost_main(unsigned hardeningBits)
// receives encodeHardening() of the launcher's state and returns the process exit status.
inline constexpr wchar_t kAppDllName[] = L"frost_app.dll";
inline constexpr char kAppEntryPoint[] = "frost_main";
using AppEntryPoint = int (*)(unsigned hardeningBits);

/*
 * LoadLibraryExW flags for the DLLs the launcher loads by their \\?\ path (frost_app.dll and,
 * in ASan trees, the ASan runtime): LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32
 * (0x900). Their static imports resolve from the DLL's own \\?\ directory and System32 only,
 * never from the plain application directory, the current directory or PATH. frost_app.dll
 * must not carry /DEPENDENTLOADFLAG: a value there replaces these flags for its imports, and
 * 0xA00 (APPLICATION_DIR | SYSTEM32) makes Qt resolve through the plain exe directory, which
 * fails from 236 units on a plain start (measured 2026-11-01; ADR 0012).
 */
inline constexpr unsigned long kLauncherLoadFlags = 0x900;

// The path of `module` (nullptr: this process's executable) as GetModuleFileNameW reports
// it: \\?\-prefixed or with 8.3 short components when it was loaded or started through such
// a path. "" on failure. `module` is an HMODULE. Thread-safe; O(path length).
[[nodiscard]] std::wstring modulePath(const void* module = nullptr);

// `path` without a \\?\ or \\?\UNC\ prefix: the form Qt and other Win32 callers use. Pure.
[[nodiscard]] std::wstring withoutExtendedPrefix(std::wstring path);

// The \\?\ (or \\?\UNC\ for \\server\share) form of the absolute, normalised path `path`,
// which Win32 accepts at any length. Pure.
[[nodiscard]] std::wstring extendedPath(const std::wstring& path);

/*
 * The launcher's step after hardenProcess(): AddDllDirectory(`extendedDirectory`), the
 * install directory in \\?\ form, then SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_USER_DIRS
 * | LOAD_LIBRARY_SEARCH_SYSTEM32). Returns ProcessHardening::installDirectoryExtended. Fails
 * closed: if AddDllDirectory fails, the default order is left at DEFAULT_DIRS and false comes
 * back; the self-test's process-hardening check reports it. Same threading rule as
 * hardenProcess(). Cannot be undone for the life of the process.
 */
[[nodiscard]] bool searchInstallDirectoryExtended(const std::wstring& extendedDirectory);

#endif

} // namespace frost::app
