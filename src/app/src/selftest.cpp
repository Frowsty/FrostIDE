// SPDX-License-Identifier: Apache-2.0
//
// Implementation of `frost --selftest` (see frost/app/selftest.hpp). Platform
// specifics are confined to the #ifdef _WIN32 blocks.

#include "frost/app/selftest.hpp"

#include "frost/app/main_window.hpp"

#include <QApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#include <chrono>
#include <format>
#include <string>
#include <string_view>

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

// RtlAreLongPathsEnabled reports whether this process can use paths beyond
// MAX_PATH without the \\?\ prefix: it needs both the manifest entry and the
// system-wide LongPathsEnabled registry value. It is exported by ntdll on every
// supported Windows 11 build but not declared in the SDK headers.
bool longPathsEffective() {
    using Fn = BOOLEAN(NTAPI*)();
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }
    auto* fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlAreLongPathsEnabled")));
    return fn != nullptr && fn() != FALSE;
}

#endif

} // namespace

std::vector<SelfTestCheck> runProcessChecks() {
    std::vector<SelfTestCheck> checks;
#ifdef _WIN32
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
