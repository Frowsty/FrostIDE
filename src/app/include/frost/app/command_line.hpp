// SPDX-License-Identifier: Apache-2.0
//
// Command-line parsing for frost (the IDE executable). Parsing happens before
// Qt is initialised so that --version and --help never pay for GUI start-up, and
// --selftest runs its process and plugin checks before QApplication exists.
#pragma once

#include "frost/core/error.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

// What the process should do after parsing.
enum class Mode {
    // Normal start: open the main window. Positional paths that name files open in editor tabs.
    // TODO(M2): open folders and solutions too (`frost <path>`, PLAN.md 2.2 shell integration).
    Gui,
    Version,  // Print version information and exit.
    Help,     // Print usage and exit.
    SelfTest, // Run the built-in self-test and exit with its status.
};

// Which self-test Mode::SelfTest runs.
enum class SelfTestKind {
    Checks, // --selftest, --selftest=onscreen: the process, plugin and GUI checks.
    Crash,  // --selftest=crash: start the crash handler, then crash on purpose (ADR 0022).
    Freeze, // --selftest=freeze: block the GUI thread for 300 ms and write a freeze report.
    Render, // --selftest=render: draw an editor on the real platform with editor.renderer's default.
};

struct CommandLine {
    Mode mode = Mode::Gui;
    // Paths given as positional arguments, in order, as UTF-8.
    std::vector<std::string> paths;
    // Explicit project root or .frostproj, before document paths; empty means restore as usual.
    std::string project;
    // Optional 1-based location on the single positional file; zero leaves position unchanged.
    int line = 0;
    int column = 0;
    // Self-test only: show the window on the real platform instead of offscreen.
    bool selfTestOnScreen = false;
    // Self-test only: which one.
    SelfTestKind selfTestKind = SelfTestKind::Checks;
};

// Parses UTF-8 arguments (argv[1..], without the program name). Every argument
// starting with '-' before "--" is an option; unknown options (including
// single-dash typos such as -version) are an InvalidArgument error, so they are
// reported instead of being opened as paths. Two different mode options (e.g.
// --version --selftest) are also an InvalidArgument error; repeating one, or combining
// --selftest with --selftest=onscreen, is allowed. --selftest=crash and --selftest=freeze must
// be the only argument (anything else with them is an InvalidArgument error). Pure and
// thread-safe; O(total length of the arguments).
[[nodiscard]] Result<CommandLine> parseCommandLine(std::span<const std::string_view> args);

// Usage text printed by --help and after a parse error.
[[nodiscard]] std::string_view usageText() noexcept;

} // namespace frost::app
