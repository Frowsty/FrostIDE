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

struct CommandLine {
    Mode mode = Mode::Gui;
    // Paths given as positional arguments, in order, as UTF-8.
    std::vector<std::string> paths;
    // Self-test only: show the window on the real platform instead of offscreen.
    bool selfTestOnScreen = false;
};

// Parses UTF-8 arguments (argv[1..], without the program name). Every argument
// starting with '-' before "--" is an option; unknown options (including
// single-dash typos such as -version) are an InvalidArgument error, so they are
// reported instead of being opened as paths. Two different mode options (e.g.
// --version --selftest) are also an InvalidArgument error; repeating one, or combining
// --selftest with --selftest=onscreen, is allowed. Pure and thread-safe; O(total length
// of the arguments).
[[nodiscard]] Result<CommandLine> parseCommandLine(std::span<const std::string_view> args);

// Usage text printed by --help and after a parse error.
[[nodiscard]] std::string_view usageText() noexcept;

} // namespace frost::app
