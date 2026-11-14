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
//      kernel32 and kernelbase are mapped, and takes the restart marker kRestartedFromVariable
//      out of its environment (it counts below only if it names this very image file);
//   2. sets the error mode for its loads (see wmain), so a broken DLL raises no system box;
//   3. restarts itself through a short link when its image path is too long for the GPU driver
//      and the start may load one (restartThroughShortLink(), below), and returns that
//      process's exit status;
//   4. adds the install directory in its normalised \\?\ form (extendedDirectoryOf()) as a
//      DLL directory and makes USER_DIRS | SYSTEM32 the default search order
//      (searchInstallDirectoryExtended());
//   5. in ASan trees (FROST_LAUNCHER_PRELOAD_ASAN_RUNTIME), loads the ASan runtime by its \\?\
//      path first: it must initialise before the UCRT and Qt, or the instrumented frost_app.dll
//      aborts with bad-free or bad-malloc_usable_size (measured 2026-11-01);
//   6. loads frost_app.dll by its \\?\ path with kLauncherLoadFlags, so the DLL and its static
//      imports (Qt, the VC runtime) load in \\?\ form at any length, and calls its frost_main();
//      its return value is the exit status.
// The image path itself stays as the parent gave it (plain on a normal start): a start
// through a \\?\ image path costs about 1.2-3.8 ms more (probe fact 28, spike fact 21 and the
// review of 4c5533d; ADR 0012, Measurements), so a short install never pays for it.
//
/*
 * The restart (step 3; the user's decision of 2026-11-01, ADR 0012). NVIDIA's user-mode driver
 * ends any process whose image path is kGpuDriverImagePathLimitUnits (MAX_PATH) or longer as it
 * loads (0xC0000409 in nvldumdx.dll's DllMain), so the GUI and --selftest=onscreen died from
 * such paths. When GetModuleFileNameW's path reaches that length and
 * launcherMayReachGpuDriver() says the start may load a driver, the launcher gets a junction
 * to its install directory under %LOCALAPPDATA%\FrostIDE\i (prepareShortLink(): created,
 * verified or repaired without administrator rights, then held open so it cannot be removed or
 * renamed, and named by its resolved path; its reparse data stays changeable by the user's own
 * programs, see ShortLink) and starts <junction>\frost.exe with CreateProcessW: the same
 * command line, current directory, environment (plus kRestartedFromVariable), standard
 * handles, desktop and show state, the parent's error mode and priority class, in the same
 * jobs (measured: about 7 ms per start with a stand-in frost_app.dll, 9-10 ms for the
 * on-screen self-test; ADR 0012). An elevated launcher refuses instead (shortLinkAllowedFor()):
 * programs of the same user without elevation could retarget the link and so choose the code
 * an elevated frost loads. The launcher cannot use frost::io's
 * IProcessLauncher (it links neither frost::io nor frost::core: /MT, KERNEL32.dll only), so it
 * calls CreateProcessW itself, with the junction's absolute path as lpApplicationName (no
 * search). It waits for that process in every mode and exits with its status, so the parent's
 * process handle still means "frost runs" (B20, exit to process gone, measures it) and the exit
 * status and output reach the parent unchanged; while it waits it keeps the junction open. The
 * restarted launcher finds the marker, checks that it names its own image file (sameImageFile():
 * the link leads to the first launcher's directory), never restarts again, refuses with a
 * message if its own path is still too long, and hands the first image path to frost_main(),
 * which shows the real
 * install directory where frost names its own (the self-test's image-path check, the GUI's
 * failure message). A start that cannot be restarted (an elevated launcher, LOCALAPPDATA unset
 * or long, the install directory on a network share, something else at the junction's path) is
 * refused with a message, as a missing DLL is, rather than left to the driver's silent crash.
 * LOCALAPPDATA comes from the environment, not SHGetKnownFolderPath: shell32 would be the
 * launcher's first DLL beyond KERNEL32 (about 3 ms to load), the parent already controls the
 * environment as it controls the image path and the current directory, and tests can point it
 * at a scratch folder. prepareShortLink() checks that it is an absolute path on a local drive
 * and short enough, and normalises it; it does not check who may write there.
 */
//
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

// The value of environment variable `name` at any length; "" when it is unset or empty.
std::wstring environmentVariable(const wchar_t* name) {
    std::wstring value(64, L'\0');
    while (true) {
        const DWORD units = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (units < value.size()) {
            value.resize(units);
            return value;
        }
        value.resize(units);
    }
}

// The file name of `path` (after its last backslash).
std::wstring fileNameOf(const std::wstring& path) { return path.substr(path.find_last_of(L'\\') + 1); }

// While the launcher waits on the parent's console, Ctrl+C, Ctrl+Break and closing the console
// must not end it before the restarted frost: that process gets the same events and decides.
// A handler routine, not SetConsoleCtrlHandler(nullptr, TRUE), whose setting children inherit.
BOOL WINAPI keepWaitingOnConsoleEvents(DWORD) { return TRUE; }

/*
 * The STARTUPINFOW for the restarted frost: this launcher's own (GetStartupInfoW), so it gets
 * the parent's desktop, window title or the shortcut it was started from
 * (STARTF_TITLEISLINKNAME), show state, position and size, and the standard handles this
 * process holds now (made inheritable). lpReserved2, the C runtime's table of inherited file
 * descriptors, is dropped: the standard handles cover descriptors 0-2, the only ones frost uses.
 * When no standard handle is usable, or STARTF_USEHOTKEY or STARTF_USEMONITOR gives a handle
 * field another meaning (startupFlagsAllowStandardHandles()), the three handle fields pass as
 * the parent set them. `hasStandardHandles` says whether any standard handle is usable.
 */
STARTUPINFOW restartStartupInfo(bool& hasStandardHandles) {
    STARTUPINFOW startup{};
    GetStartupInfoW(&startup);
    startup.cb = sizeof startup;
    startup.lpReserved = nullptr;
    startup.cbReserved2 = 0;
    startup.lpReserved2 = nullptr;
    HANDLE handles[] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
                        GetStdHandle(STD_ERROR_HANDLE)};
    hasStandardHandles = false;
    for (HANDLE& handle : handles) {
        if (usable(handle)) {
            hasStandardHandles = true;
        } else {
            handle = nullptr;
        }
    }
    if (!hasStandardHandles || !frost::app::startupFlagsAllowStandardHandles(startup.dwFlags)) {
        return startup;
    }
    for (HANDLE handle : handles) {
        if (handle != nullptr) {
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
    }
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdInput = handles[0];
    startup.hStdOutput = handles[1];
    startup.hStdError = handles[2];
    return startup;
}

/*
 * Starts frost again through its short link and waits for it (see the header comment): returns
 * that process's exit status, or 4 after reportFailure() when the launcher runs elevated, there
 * is no usable link or the start fails. `self` is this launcher's image path
 * (kGpuDriverImagePathLimitUnits or longer),
 * `extendedDirectory` its \\?\ install directory, `parentErrorMode` the error mode the parent
 * passed down, which the restarted process inherits in turn, so it sets its own exactly as a
 * direct start would.
 * In a command-line mode without usable standard handles (a terminal start without
 * redirection), the launcher attaches to the parent's console first, so that frost_app.dll's
 * attachParentConsoleIfNeeded() in the restarted process finds it through this process. The
 * GUI does not: a waiting process attached to a console would keep that console window open
 * after its shell exits.
 */
int restartThroughShortLink(const std::wstring& self, const std::wstring& extendedDirectory, UINT parentErrorMode,
                            bool commandLineMode) {
    const std::wstring why = L"frost.exe's path is " + std::to_wstring(self.size()) +
                             L" UTF-16 units long, and graphics drivers end a program started from " +
                             std::to_wstring(frost::app::kGpuDriverImagePathLimitUnits) +
                             L" units (MAX_PATH) or more, so frost restarts through a short link, but ";
    const std::wstring advice = L". Install FrostIDE in a directory with a shorter path.";
    // Before the link is created or touched: an elevated frost must not run through it.
    const auto elevation = frost::app::processUacElevation();
    if (!elevation) {
        reportFailure(why + L"it cannot tell whether it runs elevated (win32 error " +
                          std::to_wstring(elevation.error()) + L")" + advice,
                      commandLineMode);
        return 4;
    }
    if (!frost::app::shortLinkAllowedFor(*elevation)) {
        reportFailure(why + L"it runs elevated (as administrator), and programs that run without elevation could "
                            L"change that link and so the code frost loads. Start frost without elevation, or "
                            L"install FrostIDE in a directory with a shorter path.",
                      commandLineMode);
        return 4;
    }
    const std::wstring root = environmentVariable(L"LOCALAPPDATA");
    if (root.empty()) {
        reportFailure(why + L"LOCALAPPDATA, the folder for that link, is not set" + advice, commandLineMode);
        return 4;
    }
    auto link = frost::app::prepareShortLink(root, extendedDirectory, fileNameOf(self));
    if (!link) {
        // An occupied link path says what to remove; installing elsewhere is the other way out.
        const bool occupied = link.error().kind == frost::app::ShortLinkError::Kind::Occupied;
        reportFailure(why + link.error().message +
                          (occupied ? L", or install FrostIDE in a directory with a shorter path." : advice),
                      commandLineMode);
        return 4;
    }
    if (SetEnvironmentVariableW(frost::app::kRestartedFromVariable, self.c_str()) == FALSE) {
        reportFailure(why + L"the restart marker cannot be set (win32 error " + std::to_wstring(GetLastError()) + L")" +
                          advice,
                      commandLineMode);
        return 4;
    }
    bool hasStandardHandles = false;
    STARTUPINFOW startup = restartStartupInfo(hasStandardHandles);
    if (commandLineMode && !hasStandardHandles && AttachConsole(ATTACH_PARENT_PROCESS) != FALSE) {
        SetConsoleCtrlHandler(keepWaitingOnConsoleEvents, TRUE);
    }
    // CreateProcessW may write to the command line, so it gets a copy.
    std::wstring commandLine = GetCommandLineW();
    PROCESS_INFORMATION process{};
    const UINT launcherErrorMode = SetErrorMode(parentErrorMode);
    // bInheritHandles: the standard handles, and every handle the parent let this process
    // inherit, as it meant frost to have them; the launcher's own handles are not inheritable.
    const BOOL started = CreateProcessW(link->imagePath().c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        GetPriorityClass(GetCurrentProcess()), nullptr, nullptr, &startup, &process);
    const DWORD error = GetLastError();
    SetErrorMode(launcherErrorMode);
    if (started == FALSE) {
        reportFailure(L"cannot restart frost through its short link " + link->imagePath() + L": " +
                          win32Message(error) + L" (win32 error " + std::to_wstring(error) +
                          L"). Install FrostIDE in a directory whose frost.exe path is shorter than 260 UTF-16 units.",
                      commandLineMode);
        return 4;
    }
    CloseHandle(process.hThread);
    DWORD status = 4;
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
        GetExitCodeProcess(process.hProcess, &status) == FALSE) {
        status = 4;
    }
    CloseHandle(process.hProcess);
    return static_cast<int>(status);
}

} // namespace

// The static CRT's wide entry point (/ENTRY:wmainCRTStartup with /SUBSYSTEM:WINDOWS, see
// src/app/CMakeLists.txt). `argv` is used only to tell a GUI launch from a command-line mode
// and whether the start may reach a GPU driver; frost_app.dll parses the command line itself.
int wmain(int argc, wchar_t** argv) {
    frost::app::ProcessHardening hardening = frost::app::hardenProcess();
    // The restart marker: read and removed before anything can start a process, so nothing
    // frost starts inherits it. It is checked once the image path is known.
    const std::wstring marker = environmentVariable(frost::app::kRestartedFromVariable);
    if (!marker.empty()) {
        SetEnvironmentVariableW(frost::app::kRestartedFromVariable, nullptr);
    }

    const std::span<const wchar_t* const> arguments(argv + (argc > 0 ? 1 : 0),
                                                    argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    const bool commandLineMode = frost::app::launcherCommandLineMode(arguments);
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
    /*
     * The first launcher sets the marker to its own image path and starts the same file through
     * the link, so a true marker names this process's image file. One that names another file (a
     * parent's stale or hand-set variable) is dropped: it neither stops the restart below nor
     * reaches frost_main(). Costs two opens only when the marker is set.
     */
    const std::wstring restartedFrom =
        !marker.empty() && frost::app::sameImageFile(marker, self) ? marker : std::wstring();
    // Before any DLL of frost's is loaded, so the first launcher costs one process start and
    // nothing else (ADR 0012, Measurements).
    if (self.size() >= frost::app::kGpuDriverImagePathLimitUnits &&
        frost::app::launcherMayReachGpuDriver(arguments, environmentVariable(L"QT_QPA_PLATFORM"))) {
        if (!restartedFrom.empty()) {
            // The loop guard: a restarted launcher never restarts again. A real restart cannot
            // get here (prepareShortLink() keeps the link's path short); a parent that sets the
            // marker to frost's own long path can.
            reportFailure(L"frost was restarted through the short link " + self + L" from " + restartedFrom +
                              L", but that path is still " + std::to_wstring(self.size()) +
                              L" UTF-16 units long, and graphics drivers end a program started from " +
                              std::to_wstring(frost::app::kGpuDriverImagePathLimitUnits) +
                              L" units (MAX_PATH) or more. Install FrostIDE in a directory with a shorter path.",
                          commandLineMode);
            return 4;
        }
        return restartThroughShortLink(self, dir, inheritedErrorMode, commandLineMode);
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
    return entry(frost::app::encodeHardening(hardening), restartedFrom.empty() ? nullptr : restartedFrom.c_str());
}
