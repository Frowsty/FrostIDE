// SPDX-License-Identifier: Apache-2.0
//
// The built-in self-test behind `frost --selftest`. It verifies, inside the
// real executable, the properties that unit tests cannot see: the embedded
// manifest's effects (UTF-8 code page, long-path awareness, per-monitor v2
// DPI awareness) and that the Qt GUI stack can create and show the main window.
// Packaging smoke tests (PLAN.md section 2.2) run it from each extracted package.
#pragma once

#include <string>
#include <vector>

namespace frost::app {

struct SelfTestCheck {
    std::string name;   // Stable identifier, e.g. "utf8-code-page".
    bool ok = false;    // Whether the property holds.
    bool required = true; // Required checks decide the exit status; others are informational.
    std::string detail; // Human-readable explanation (always set).
};

struct SelfTestReport {
    std::vector<SelfTestCheck> checks;

    // True when every required check passed.
    [[nodiscard]] bool passed() const noexcept;
    // One line per check ("selftest: ok   name: detail") plus a summary line.
    [[nodiscard]] std::string format() const;
};

// Checks that need no QApplication (code page, manifest contents, long paths).
[[nodiscard]] std::vector<SelfTestCheck> runProcessChecks();

// Checks that need a live QApplication on the calling (GUI) thread: DPI
// awareness and creating, showing and closing the main window.
[[nodiscard]] std::vector<SelfTestCheck> runGuiChecks();

} // namespace frost::app
