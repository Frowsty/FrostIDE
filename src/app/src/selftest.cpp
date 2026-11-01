// SPDX-License-Identifier: Apache-2.0
//
// Implementation of `frost --selftest` (see frost/app/selftest.hpp). Platform
// specifics are confined to the #ifdef _WIN32 blocks.

#include "frost/app/selftest.hpp"

#include "frost/app/main_window.hpp"
#include "frost/app/startup.hpp"

#include <QApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <cwchar>

#include <windows.h>
#endif

namespace frost::app {
namespace {

// Pumps events until `widget` has a native window that the platform reports as
// exposed, or `timeout` passes. Returns whether it was exposed.
bool waitForExposed(QWidget& widget, std::chrono::milliseconds timeout) {
    const QDeadlineTimer deadline(timeout);
    while (true) {
        const QWindow* handle = widget.windowHandle();
        if (handle != nullptr && handle->isExposed()) {
            return true;
        }
        if (deadline.hasExpired()) {
            return false;
        }
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

} // namespace

bool SelfTestReport::passed() const noexcept {
    for (const auto& check : checks) {
        if (check.required && !check.ok) {
            return false;
        }
    }
    return true;
}

std::string SelfTestReport::format() const {
    std::string out;
    int failed = 0;
    for (const auto& check : checks) {
        const std::string_view status = check.ok ? "ok  " : (check.required ? "FAIL" : "warn");
        out += std::format("selftest: {} {}: {}\n", status, check.name, check.detail);
        if (check.required && !check.ok) {
            ++failed;
        }
    }
    out += failed == 0 ? std::string("selftest: PASSED\n")
                       : std::format("selftest: FAILED ({} required checks)\n", failed);
    return out;
}

namespace {

#ifdef _WIN32

// Returns the text of this executable's embedded RT_MANIFEST resource 1, or "" if absent.
// The process manifest is frost.exe's; frost_app.dll carries none (/MANIFEST:NO).
std::string_view embeddedManifest() {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(self, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
    if (resource == nullptr) {
        return {};
    }
    HGLOBAL loaded = LoadResource(self, resource);
    const DWORD size = SizeofResource(self, resource);
    if (loaded == nullptr || size == 0) {
        return {};
    }
    return {static_cast<const char*>(LockResource(loaded)), size};
}

// The image that holds this code: frost_app.dll in frost, the test executable in unit tests.
HMODULE thisImage() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisImage), &module);
    return module;
}

// frost.exe's /DEPENDENTLOADFLAG: LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32.
// Its only import is KERNEL32.dll, so the value is defence in depth.
constexpr DWORD kExeDependentLoadFlags = 0xA00;
// frost_app.dll has none (no /DEPENDENTLOADFLAG, src/app/CMakeLists.txt), so its imports
// follow the launcher's kLauncherLoadFlags (0x900). A value there would replace them, and
// 0xA00 breaks plain starts from 236 units (ADR 0012).
constexpr DWORD kAppDependentLoadFlags = 0;

// The DependentLoadFlags of `module`'s load configuration directory: the search flags the
// loader uses for its static imports. 0 if the image has no such field.
DWORD dependentLoadFlags(HMODULE module) {
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG) {
        return 0;
    }
    const IMAGE_DATA_DIRECTORY& entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    if (entry.VirtualAddress == 0) {
        return 0;
    }
    const auto* config = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY*>(base + entry.VirtualAddress);
    // The directory's own Size says which fields the image has; DependentLoadFlags is a WORD.
    if (config->Size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, DependentLoadFlags) + sizeof(WORD)) {
        return 0;
    }
    return config->DependentLoadFlags;
}

// UTF-8 of a Win32 string (unpaired surrogates become U+FFFD).
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

// The file name of `path` (after its last backslash).
std::wstring_view fileName(std::wstring_view path) {
    const std::size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

bool isExtended(std::wstring_view path) { return path.starts_with(L"\\\\?\\"); }

// "\\?\ form" or "plain form", as the self-test reports a path.
std::string_view formOf(std::wstring_view path) { return isExtended(path) ? "\\\\?\\ form" : "plain form"; }

// The Qt6Core DLL this process loaded (release or debug build), or nullptr.
HMODULE qtCore() {
    HMODULE module = GetModuleHandleW(L"Qt6Core.dll");
    return module != nullptr ? module : GetModuleHandleW(L"Qt6Cored.dll");
}

#endif

} // namespace

std::vector<SelfTestCheck> runProcessChecks(const ProcessHardening& hardening, bool dialogGuard,
                                            [[maybe_unused]] std::string_view restartedFrom) {
    std::vector<SelfTestCheck> checks;
#ifdef _WIN32
    std::string missing;
    for (const auto& [applied, what] :
         {std::pair{hardening.defaultDllDirectories, "SetDefaultDllDirectories"},
          std::pair{hardening.dllDirectoryCleared, "SetDllDirectoryW"},
          std::pair{hardening.safeSearchPathMode, "SetSearchPathMode"},
          std::pair{hardening.exeSearchSkipsCurrentDirectory, "NoDefaultCurrentDirectoryInExePath"},
          std::pair{hardening.installDirectoryExtended,
                    "AddDllDirectory of the \\\\?\\ install directory with USER_DIRS | SYSTEM32 as the default"}}) {
        if (!applied) {
            missing += missing.empty() ? what : std::string(", ") + what;
        }
    }
    // Only what the hardening guarantees: SearchPathW still searches the current directory
    // (see ProcessHardening::safeSearchPathMode).
    const bool hardened = hardening.complete() && hardening.installDirectoryExtended;
    checks.push_back({"process-hardening", hardened, true,
                      hardened ? "DLL searches skip the current directory, and so do CreateProcessW and cmd.exe for "
                                 "bare executable names; DLLs load from the \\\\?\\ install directory and System32"
                               : "not applied: " + missing});

    const DWORD exeFlags = dependentLoadFlags(GetModuleHandleW(nullptr));
    const DWORD appFlags = dependentLoadFlags(thisImage());
    const bool flagsOk = exeFlags == kExeDependentLoadFlags && appFlags == kAppDependentLoadFlags;
    checks.push_back({"dependent-load-flags", flagsOk, true,
                      std::format("DependentLoadFlags frost.exe {:#x}, frost_app.dll {:#x}{}", exeFlags, appFlags,
                                  flagsOk ? ""
                                          : " (expected 0xa00 for frost.exe and 0 for frost_app.dll, whose imports "
                                            "follow the launcher's DLL_LOAD_DIR | SYSTEM32)")});

    const UINT acp = GetACP();
    checks.push_back({"utf8-code-page", acp == CP_UTF8, true, std::format("process ANSI code page is {}", acp)});

    const std::string_view manifest = embeddedManifest();
    const bool declaresLongPaths = manifest.find("<longPathAware") != std::string_view::npos &&
                                   manifest.find(">true</longPathAware>") != std::string_view::npos;
    checks.push_back({"long-path-manifest", declaresLongPaths, true,
                      declaresLongPaths ? "embedded manifest declares longPathAware"
                                        : "embedded manifest is missing longPathAware=true"});

    // The registry switch needs administrator rights, and frost does not depend on it.
    const bool effective = longPathsEffective();
    checks.push_back({"long-paths-effective", effective, false,
                      effective ? "paths beyond MAX_PATH work without the \\\\?\\ prefix"
                                : "not effective for this process (needs LongPathsEnabled=1 and longPathAware); "
                                  "FrostIDE starts either way"});

    // The form the parent started frost.exe through, as the loader recorded it, and the path the
    // first launcher had when it restarted frost through this short link.
    const std::wstring image = modulePath();
    std::string imageDetail = std::format("{}, {} UTF-16 units", formOf(image), image.size());
    if (!restartedFrom.empty()) {
        const int size = static_cast<int>(restartedFrom.size());
        const int units = MultiByteToWideChar(CP_UTF8, 0, restartedFrom.data(), size, nullptr, 0);
        std::wstring first(static_cast<std::size_t>(std::max(units, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, restartedFrom.data(), size, first.data(), static_cast<int>(first.size()));
        imageDetail += std::format("; restarted through this short link from {} ({}, {} UTF-16 units)", restartedFrom,
                                   formOf(first), first.size());
    }
    checks.push_back({"image-path", true, false, std::move(imageDetail)});

    // frost_app.dll and its static imports must have come in through \\?\ paths, or a long
    // plain start would have died in the loader; this shows that the mechanism was used.
    const std::wstring app = modulePath(thisImage());
    const HMODULE qt = qtCore();
    const std::wstring qtPath = qt != nullptr ? modulePath(qt) : std::wstring();
    const bool loadedExtended = isExtended(app) && isExtended(qtPath);
    checks.push_back(
        {"load-form", loadedExtended, true,
         loadedExtended
             ? std::format("{} and {} loaded through \\\\?\\ paths", toUtf8(fileName(app)), toUtf8(fileName(qtPath)))
             : std::format("{} loaded in {} and {} in {}; the launcher loads both through \\\\?\\ paths (ADR 0012)",
                           toUtf8(fileName(app)), formOf(app), qtPath.empty() ? "Qt6Core" : toUtf8(fileName(qtPath)),
                           formOf(qtPath))});

    checks.push_back(
        {"dialog-guard", dialogGuard, true,
         dialogGuard ? "dialogs are refused on the main thread" : "the dialog hook could not be installed"});
#else
    static_cast<void>(hardening);
    static_cast<void>(dialogGuard);
    checks.push_back({"platform", true, false, "no process-level manifest checks on this platform"});
#endif
    return checks;
}

std::vector<SelfTestCheck> runLateLoadChecks() {
    std::vector<SelfTestCheck> checks;
#ifdef _WIN32
    const std::wstring exe = withoutExtendedPrefix(modulePath());
    const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    std::vector<std::wstring> pending;
    std::size_t total = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((extendedPath(dir) + L"\\*.dll").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring_view name = data.cFileName;
            // "*.dll" also matches longer extensions through their 8.3 short names.
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || name.size() <= 4 ||
                _wcsicmp(name.substr(name.size() - 4).data(), L".dll") != 0) {
                continue;
            }
            ++total;
            if (GetModuleHandleW(data.cFileName) == nullptr) {
                pending.emplace_back(name);
            }
        } while (FindNextFileW(find, &data) != FALSE);
        FindClose(find);
    }
    if (pending.empty()) {
        checks.push_back(
            {"late-load", true, false, std::format("all {} DLLs beside frost.exe were loaded at start-up", total)});
        return checks;
    }
    bool ok = true;
    std::string detail;
    for (const std::wstring& name : pending) {
        // By bare name, with the process default search order, as Qt loads a library lazily.
        HMODULE module = LoadLibraryW(name.c_str());
        std::string result;
        if (module != nullptr) {
            result = std::format("{} ({})", toUtf8(name), formOf(modulePath(module)));
            FreeLibrary(module);
        } else {
            const DWORD error = GetLastError();
            ok = false;
            result = std::format("{} failed (win32 error {})", toUtf8(name), error);
        }
        detail += (detail.empty() ? "" : ", ") + result;
    }
    checks.push_back({"late-load", ok, false, "loaded by name: " + detail});
#endif
    return checks;
}

std::vector<SelfTestCheck> runGuiChecks() {
    std::vector<SelfTestCheck> checks;
    const QString platform = QGuiApplication::platformName();
    checks.push_back({"qt-platform", !platform.isEmpty(), true,
                      std::format("Qt {} on platform plugin '{}'", qVersion(), platform.toStdString())});

#ifdef _WIN32
    // The manifest sets per-monitor v2 awareness before any code runs, so it holds
    // even on the offscreen platform, which does not touch DPI settings itself.
    const bool pmv2 = AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                                   DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
    checks.push_back({"dpi-awareness", pmv2, true, pmv2 ? "per-monitor v2" : "thread is not per-monitor v2 DPI aware"});
#endif

    MainWindow window;
    window.show();
    // show() sets isVisible() synchronously; exposure proves the platform plugin
    // created a native window and delivered its expose event.
    const bool exposed = waitForExposed(window, std::chrono::seconds(5));
    const bool contents = window.windowTitle() == QStringLiteral("FrostIDE") && window.centralWidget() != nullptr;
    window.close();
    QApplication::processEvents();
    const bool closed = !window.isVisible();
    std::string detail;
    if (!exposed) {
        detail = "main window was not exposed within 5 s";
    } else if (!contents) {
        detail = "main window is missing its title or central widget";
    } else if (!closed) {
        detail = "main window did not close";
    } else {
        detail = "main window exposed and closed";
    }
    checks.push_back({"main-window", exposed && contents && closed, true, std::move(detail)});
    return checks;
}

} // namespace frost::app
