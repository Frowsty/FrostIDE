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

#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
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

// LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32, set by /DEPENDENTLOADFLAG.
constexpr DWORD kDependentLoadFlagsWanted = 0xA00;

// The DependentLoadFlags of this executable's load configuration directory: the search
// flags the loader uses for its static imports. 0 if the image has no such field.
DWORD dependentLoadFlags() {
    const auto* base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
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

#endif

} // namespace

std::vector<SelfTestCheck> runProcessChecks(const ProcessHardening& hardening) {
    std::vector<SelfTestCheck> checks;
#ifdef _WIN32
    std::string missing;
    for (const auto& [applied, what] :
         {std::pair{hardening.defaultDllDirectories, "SetDefaultDllDirectories"},
          std::pair{hardening.dllDirectoryCleared, "SetDllDirectoryW"},
          std::pair{hardening.safeSearchPathMode, "SetSearchPathMode"},
          std::pair{hardening.exeSearchSkipsCurrentDirectory, "NoDefaultCurrentDirectoryInExePath"}}) {
        if (!applied) {
            missing += missing.empty() ? what : std::string(", ") + what;
        }
    }
    // Only what the hardening guarantees: SearchPathW still searches the current directory
    // (see ProcessHardening::safeSearchPathMode).
    checks.push_back({"process-hardening", hardening.complete(), true,
                      hardening.complete() ? "DLL searches skip the current directory, and so do CreateProcessW "
                                             "and cmd.exe for bare executable names"
                                           : "not applied: " + missing});
    const bool dependentFlags = dependentLoadFlags() == kDependentLoadFlagsWanted;
    checks.push_back({"dependent-load-flags", dependentFlags, true,
                      std::format("load configuration DependentLoadFlags = {:#x}{}", dependentLoadFlags(),
                                  dependentFlags ? "" : " (expected 0xa00: application directory and System32)")});

    const UINT acp = GetACP();
    checks.push_back({"utf8-code-page", acp == CP_UTF8, true, std::format("process ANSI code page is {}", acp)});

    const std::string_view manifest = embeddedManifest();
    const bool declaresLongPaths = manifest.find("<longPathAware") != std::string_view::npos &&
                                   manifest.find(">true</longPathAware>") != std::string_view::npos;
    checks.push_back({"long-path-manifest", declaresLongPaths, true,
                      declaresLongPaths ? "embedded manifest declares longPathAware"
                                        : "embedded manifest is missing longPathAware=true"});

    // The registry switch needs administrator rights, so this check informs rather than fails.
    const bool effective = longPathsEffective();
    checks.push_back({"long-paths-effective", effective, false,
                      effective ? "paths beyond MAX_PATH work without the \\\\?\\ prefix"
                                : "disabled system-wide (HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem "
                                  "LongPathsEnabled=0); FrostIDE uses \\\\?\\ paths internally"});
#else
    static_cast<void>(hardening);
    checks.push_back({"platform", true, false, "no process-level manifest checks on this platform"});
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
