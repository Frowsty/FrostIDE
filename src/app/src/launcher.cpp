// SPDX-License-Identifier: Apache-2.0
//
// frost.exe on Windows: the launcher that starts FrostIDE from any directory length (ADR 0012).
//
// The application lives in frost_app.dll beside it (src/app/src/main.cpp). The launcher is
// built with the static CRT (/MT in every configuration, so no debug CRT can show its own
// assertion box) and without sanitizer instrumentation, and it imports KERNEL32.dll only. So
// the loader maps nothing beside it before wmain runs: the 250-unit plain-path limit that the
// Windows loader applies to DLLs embedding RT_MANIFEST resource 2 (Qt's, the ASan runtime's)
// cannot kill the process before its first instruction, as it did for the old frost.exe with
// static Qt imports (0xC0000106 on plain starts from directories of 236-249 units in Release,
// 235-249 in Debug and 218-249 in ASan trees; ADR 0003). wmain then:
//   1. hardens the process (hardenProcess(), frost/app/startup_core.hpp) while only ntdll,
//      kernel32 and kernelbase are mapped;
//   2. sets the error mode for its loads (see wmain), so a broken DLL raises no system box;
//   3. adds the install directory in its normalised \\?\ form (extendedDirectoryOf()) as a
//      DLL directory and makes USER_DIRS | SYSTEM32 the default search order
//      (searchInstallDirectoryExtended());
//   4. in ASan trees (FROST_LAUNCHER_PRELOAD_ASAN_RUNTIME), loads the ASan runtime by its \\?\
//      path first: it must initialise before the UCRT and Qt, or the instrumented frost_app.dll
//      aborts with bad-free or bad-malloc_usable_size (measured 2026-11-01);
//   5. loads frost_app.dll by its \\?\ path with kLauncherLoadFlags, so the DLL and its static
//      imports (Qt, the VC runtime) load in \\?\ form at any length, and calls its frost_main();
//      its return value is the exit status.
// The image path itself stays as the parent gave it (plain on a normal start): a start
// through a \\?\ image path costs about 1.2-3.8 ms more (probe fact 28, spike fact 21 and the
// review of 4c5533d; ADR 0012, Measurements), so a short install never pays for it.
// If frost_app.dll, its imports or its entry point cannot be loaded, the launcher reports why
// (reportFailure) and exits with 4, the status frost uses for "cannot start in this
// environment". It has no other state and no globals (startup hygiene, PLAN.md 2.1).

#include "frost/app/startup_core.hpp"

#include <span>
#include <string>
#include <string_view>

#include <windows.h>

namespace {

// FormatMessageW's text for a Win32 error, without the trailing line break.
std::wstring win32Message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD units =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = units > 0 && buffer != nullptr ? std::wstring(buffer, units) : std::wstring(L"unknown error");
    LocalFree(buffer);
    while (!text.empty() &&
           (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    return text;
}

// UTF-8 of `text` (unpaired surrogates become U+FFFD, as WideCharToMultiByte does).
std::string toUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr,
                            nullptr);
    }
    return out;
}

// Whether `handle` is a usable standard handle (a console, pipe or file).
bool usable(HANDLE handle) {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

/*
 * Reports why frost cannot start, as frost_app.dll's reportStartupFailure() does for the GUI:
 *   - "frost: <message>" on stderr if the process has one (UTF-8 to a pipe or file, UTF-16
 *     through WriteConsoleW to a console), else on the parent's console (AttachConsole and
 *     CONOUT$), if there is one;
 *   - in GUI mode (`commandLineMode` false, see launcherCommandLineMode()), also a message
 *     box, because a launch from Explorer has neither. user32 is loaded only here, from
 *     System32, so the launcher's static imports stay KERNEL32.dll alone. Command-line modes
 *     never show a box: tests and scripts run them unattended, and wmain has set an error mode
 *     in which the loader raises no system box either.
 */
void reportFailure(const std::wstring& message, bool commandLineMode) {
    const std::wstring line = L"frost: " + message + L"\n";
    DWORD written = 0;
    if (HANDLE err = GetStdHandle(STD_ERROR_HANDLE); usable(err)) {
        if (GetFileType(err) == FILE_TYPE_CHAR) {
            WriteConsoleW(err, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        } else {
            const std::string bytes = toUtf8(line);
            WriteFile(err, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        }
    } else if (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE) {
        HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        if (console != INVALID_HANDLE_VALUE) {
            WriteConsoleW(console, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            CloseHandle(console);
        }
    }
    if (commandLineMode) {
        return;
    }
    HMODULE user32 = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using MessageBoxFn = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
    auto* box = user32 == nullptr
                    ? nullptr
                    : reinterpret_cast<MessageBoxFn>(reinterpret_cast<void*>(GetProcAddress(user32, "MessageBoxW")));
    if (box != nullptr) {
        box(nullptr, message.c_str(), L"FrostIDE cannot start", MB_OK | MB_ICONERROR);
    }
}

/*
 * "cannot load <path>: <reason> (win32 error N)", with the path in its plain form. When the
 * file itself exists, "cannot load <path> or a DLL it needs: ...": the loader reports a
 * missing (126), mismatched (127) or broken (193) static import of the DLL with the same
 * error as the DLL itself, and does not say which one it was.
 */
std::wstring loadFailure(const std::wstring& path, DWORD error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const bool fileExists = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return L"cannot load " + frost::app::withoutExtendedPrefix(path) + (fileExists ? L" or a DLL it needs" : L"") +
           L": " + win32Message(error) + L" (win32 error " + std::to_wstring(error) + L")";
}

} // namespace

// The static CRT's wide entry point (/ENTRY:wmainCRTStartup with /SUBSYSTEM:WINDOWS, see
// src/app/CMakeLists.txt). `argv` is used only to tell a GUI launch from a command-line mode;
// frost_app.dll parses the command line itself.
int wmain(int argc, wchar_t** argv) {
    frost::app::ProcessHardening hardening = frost::app::hardenProcess();

    const bool commandLineMode = frost::app::launcherCommandLineMode(
        std::span<const wchar_t* const>(argv + (argc > 0 ? 1 : 0), argc > 1 ? static_cast<std::size_t>(argc - 1) : 0));
    /*
     * Without SEM_FAILCRITICALERRORS in the process error mode, which a parent such as a
     * terminal passes on as its own, the loader raises a hard error for a mismatched import
     * (STATUS_ENTRYPOINT_NOT_FOUND, win32 error 127) or a broken image (STATUS_INVALID_IMAGE_FORMAT,
     * 193), and csrss shows a modal "Entry Point Not Found" or "Bad Image" box that holds the
     * process until someone clicks (measured 2026-11-01, review of 4c5533d). Command-line
     * modes keep the no-box mode for good, as frost_app.dll's suppressCrashDialogs() sets it
     * again later. The GUI has it only while the launcher loads, so the launcher's own box is
     * the only one, and frost_app.dll starts with the mode the parent gave (Windows Error
     * Reporting stays on until M1's crash handler). The mode is process-wide, not
     * SetThreadErrorMode, because the loader maps a DLL's imports on its worker threads too;
     * the launcher starts no thread of its own. FrostProcess.LauncherSetsTheErrorModeForItsLoads
     * reads the mode back inside a stand-in frost_app.dll.
     */
    constexpr UINT kCommandLineMode = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;
    constexpr UINT kGuiLoadMode = SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX;
    const UINT inheritedErrorMode = SetErrorMode(GetErrorMode() | (commandLineMode ? kCommandLineMode : kGuiLoadMode));

    const std::wstring self = frost::app::modulePath();
    // Always the \\?\ form, whatever the length: a length rule would hang on a hand-kept list of
    // the longest deployed file name and measured no faster (ADR 0012).
    const std::wstring dir = self.empty() ? std::wstring() : frost::app::extendedDirectoryOf(self);
    if (dir.empty()) {
        reportFailure(L"cannot determine the path of frost.exe (win32 error " + std::to_wstring(GetLastError()) + L")",
                      commandLineMode);
        return 4;
    }
    hardening.installDirectoryExtended = frost::app::searchInstallDirectoryExtended(dir);

#ifdef FROST_LAUNCHER_PRELOAD_ASAN_RUNTIME
    const std::wstring runtime = dir + L"\\"
                                       L"" FROST_LAUNCHER_PRELOAD_ASAN_RUNTIME;
    if (LoadLibraryExW(runtime.c_str(), nullptr, frost::app::kLauncherLoadFlags) == nullptr) {
        reportFailure(loadFailure(runtime, GetLastError()), commandLineMode);
        return 4;
    }
#endif

    const std::wstring app = dir + L"\\" + frost::app::kAppDllName;
    HMODULE module = LoadLibraryExW(app.c_str(), nullptr, frost::app::kLauncherLoadFlags);
    if (module == nullptr) {
        reportFailure(loadFailure(app, GetLastError()), commandLineMode);
        return 4;
    }
    auto* entry = reinterpret_cast<frost::app::AppEntryPoint>(
        reinterpret_cast<void*>(GetProcAddress(module, frost::app::kAppEntryPoint)));
    if (entry == nullptr) {
        reportFailure(frost::app::withoutExtendedPrefix(app) + L" has no entry point frost_main (win32 error " +
                          std::to_wstring(GetLastError()) + L")",
                      commandLineMode);
        return 4;
    }
    if (!commandLineMode) {
        SetErrorMode(inheritedErrorMode);
    }
    return entry(frost::app::encodeHardening(hardening));
}
