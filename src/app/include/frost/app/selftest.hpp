// SPDX-License-Identifier: Apache-2.0
//
// The built-in self-test behind `frost --selftest`. It verifies, inside the real program
// (on Windows the frost.exe launcher with frost_app.dll; ADR 0012), the properties that unit
// tests cannot see: the start-up hardening and the launcher's \\?\ install-directory search
// order, the DependentLoadFlags of both images, and the command-line dialog guard (all
// required); the embedded manifest and its effects (UTF-8 code page and per-monitor v2 DPI
// awareness are required; long-path awareness is required in the manifest, and its effect is
// reported for information, because frost starts either way); the form of the image path and
// whether the launcher restarted frost through its short link (information) and that
// frost_app.dll and Qt were loaded through \\?\ paths (required); that
// Qt's platform plugin was pinned (frost/app/startup.hpp's preparePlatformPlugin(), run by
// frost_main() before QApplication exists) and the other deployed plugins preloaded when their
// paths are long (information); that the Qt GUI stack can create, expose and close the main
// window; and that a DLL beside frost.exe loaded later by name resolves (information).
// Packaging smoke tests (PLAN.md section 2.2) run it from each extracted package.
#pragma once

#include "frost/app/startup.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

struct SelfTestCheck {
    std::string name;     // Stable identifier, e.g. "utf8-code-page".
    bool ok = false;      // Whether the property holds.
    bool required = true; // Required checks decide the exit status; others are informational.
    std::string detail;   // Human-readable explanation (always set).
};

struct SelfTestReport {
    std::vector<SelfTestCheck> checks;

    // True when every required check passed.
    [[nodiscard]] bool passed() const noexcept;
    // One line per check ("selftest: ok   name: detail") plus a summary line.
    [[nodiscard]] std::string format() const;
};

/*
 * Checks that need no QApplication: the start-up hardening `hardening` reports (decoded from
 * the launcher's bits), whether guardCommandLineDialogs() succeeded (`dialogGuard`), the
 * DependentLoadFlags of frost.exe and of the image holding this code (frost_app.dll), the
 * code page, the manifest contents, long paths, the image path's form and the form in which
 * frost_app.dll and Qt6Core were loaded. `restartedFrom` is the first launcher's image path
 * (UTF-8) when the launcher restarted frost through its short link (ADR 0012), else ""; the
 * image-path check then names it, with its form and length: "plain form, 61 UTF-16 units;
 * restarted through this short link from <path> (\\?\ form, 314 UTF-16 units)". On Linux only
 * a placeholder check.
 */
[[nodiscard]] std::vector<SelfTestCheck> runProcessChecks(const ProcessHardening& hardening, bool dialogGuard,
                                                          std::string_view restartedFrom);

/*
 * Checks that need a live QApplication (precondition) on the calling GUI thread: the
 * platform plugin in use, per-monitor v2 DPI awareness on Windows, and creating, showing
 * and closing a MainWindow. Blocks for up to 5 s while it pumps the event loop waiting
 * for the window to be exposed; on a native platform the window is visible on screen.
 */
[[nodiscard]] std::vector<SelfTestCheck> runGuiChecks();

/*
 * --selftest=render (ADR 0043): an EditorView on the real Qt platform with editor.renderer's default
 * ("auto"), shown for at most 10 s. "editor-renderer" passes when the rule's backend draws: raster
 * where the rule picks raster (Linux, remote sessions), else the QRhi viewport initialises without a
 * fallback and a grabbed frame shows text. The detail names the backend, the reason and the adapter.
 * Precondition: a QApplication on the platform under test. GUI thread.
 */
[[nodiscard]] std::vector<SelfTestCheck> runRenderChecks();

/*
 * Windows: the "late-load" check. Loads each DLL beside frost.exe that is not loaded yet by
 * its bare name, with the process default search order, as Qt loads libraries it needs
 * lazily, and frees it again; it must resolve through the launcher's \\?\ install directory
 * also when frost was started through a long plain path. Informational (a DLL may fail for
 * other reasons). Loading runs each DLL's DllMain; frost's own deployment has no such DLL, so
 * normally it lists one directory and loads nothing. Empty on Linux. Main thread.
 */
[[nodiscard]] std::vector<SelfTestCheck> runLateLoadChecks();

} // namespace frost::app
