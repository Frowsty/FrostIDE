// SPDX-License-Identifier: Apache-2.0
//
// The built-in self-test behind `frost --selftest`. It verifies, inside the
// real executable, the properties that unit tests cannot see: the start-up
// hardening and the linker's DependentLoadFlags (both required), the embedded
// manifest and its effects (UTF-8 code page and per-monitor v2 DPI awareness are
// required; long-path awareness is required in the manifest, and its effect is
// reported for information because it also needs the system LongPathsEnabled
// policy), that Qt can load the platform plugin it will use (frost/app/startup.hpp's
// checkPluginPaths(), run by main() before QApplication exists), and that the Qt GUI
// stack can create, expose and close the main window.
// Packaging smoke tests (PLAN.md section 2.2) run it from each extracted package.
#pragma once

#include "frost/app/startup.hpp"

#include <string>
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

// Checks that need no QApplication: the start-up hardening `hardening` reports (from
// hardenProcess()), the executable's DependentLoadFlags, the code page, the manifest
// contents and long paths.
[[nodiscard]] std::vector<SelfTestCheck> runProcessChecks(const ProcessHardening& hardening);

// Checks that need a live QApplication on the calling (GUI) thread: DPI
// awareness and creating, showing and closing the main window.
[[nodiscard]] std::vector<SelfTestCheck> runGuiChecks();

} // namespace frost::app
