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
//   2. adds the install directory in its \\?\ form as a DLL directory and makes USER_DIRS |
//      SYSTEM32 the default search order (searchInstallDirectoryExtended());
//   3. in ASan trees (FROST_LAUNCHER_PRELOAD_ASAN_RUNTIME), loads the ASan runtime by its \\?\
//      path first: it must initialise before the UCRT and Qt, or the instrumented frost_app.dll
//      aborts with bad-free or bad-malloc_usable_size (measured 2026-11-01);
//   4. loads frost_app.dll by its \\?\ path with kLauncherLoadFlags, so the DLL and its static
//      imports (Qt, the VC runtime) load in \\?\ form at any length, and calls its frost_main();
//      its return value is the exit status.
// The image path itself stays as the parent gave it (plain on a normal start), so a short
// install pays nothing for the \\?\ form of the image (about +1.4 ms per start, ADR 0012).
// If frost_app.dll, its imports or its entry point cannot be loaded, the launcher reports why
// (reportFailure) and exits with 4, the status frost uses for "cannot start in this
// environment". It has no other state and no globals (startup hygiene, PLAN.md 2.1).

#include "frost/app/startup_core.hpp"

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
 *   - in GUI mode (`commandLineMode` false: no argument starts with '-'), also a message box,
 *     because a launch from Explorer has neither. user32 is loaded only here, from System32,
 *     so the launcher's static imports stay KERNEL32.dll alone. Command-line modes never show
 *     a box: tests and scripts run them unattended.
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

// "cannot load <path>: <reason> (win32 error N)", with the path in its plain form.
std::wstring loadFailure(const std::wstring& path, DWORD error) {
    return L"cannot load " + frost::app::withoutExtendedPrefix(path) + L": " + win32Message(error) + L" (win32 error " +
           std::to_wstring(error) + L")";
}

} // namespace

// The static CRT's wide entry point (/ENTRY:wmainCRTStartup with /SUBSYSTEM:WINDOWS, see
// src/app/CMakeLists.txt). `argv` is used only to tell a GUI launch from a command-line mode;
// frost_app.dll parses the command line itself.
int wmain(int argc, wchar_t** argv) {
    frost::app::ProcessHardening hardening = frost::app::hardenProcess();

    bool commandLineMode = false;
    for (int i = 1; i < argc; ++i) {
        commandLineMode = commandLineMode || argv[i][0] == L'-';
    }

    const std::wstring self = frost::app::modulePath();
    const std::size_t slash = self.find_last_of(L'\\');
    if (self.empty() || slash == std::wstring::npos) {
        reportFailure(L"cannot determine the path of frost.exe (win32 error " + std::to_wstring(GetLastError()) + L")",
                      commandLineMode);
        return 4;
    }
    // Always the \\?\ form, whatever the length: a length rule would hang on a hand-kept list of
    // the longest deployed file name and measured no faster (ADR 0012).
    const std::wstring dir = frost::app::extendedPath(frost::app::withoutExtendedPrefix(self.substr(0, slash)));
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
    return entry(frost::app::encodeHardening(hardening));
}
