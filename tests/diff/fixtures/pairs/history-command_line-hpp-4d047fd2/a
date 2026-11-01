// SPDX-License-Identifier: Apache-2.0
//
// Command-line parsing for frost (the IDE executable). Parsing happens before
// Qt is initialised so that --version and --selftest never pay for GUI start-up.
#pragma once

#include "frost/core/error.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

// What the process should do after parsing.
enum class Mode {
    Gui,      // Normal start: open the main window (and any paths given).
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

// Parses UTF-8 arguments (argv[1..], without the program name). Unknown options
// are an InvalidArgument error so typos are reported instead of ignored.
[[nodiscard]] Result<CommandLine> parseCommandLine(std::span<const std::string_view> args);

// Usage text printed by --help and after a parse error.
[[nodiscard]] std::string_view usageText() noexcept;

} // namespace frost::app
