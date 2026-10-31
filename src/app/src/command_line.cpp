// SPDX-License-Identifier: Apache-2.0
//
// Implementation of frost's command-line parser (see frost/app/command_line.hpp).

#include "frost/app/command_line.hpp"

#include <format>

namespace frost::app {

std::string_view usageText() noexcept {
    return "Usage: frost [options] [path...]\n"
           "\n"
           "Options:\n"
           "  --version            Print version information and exit.\n"
           "  --help               Print this help and exit.\n"
           "  --selftest           Run the built-in self-test offscreen and exit with its status.\n"
           "  --selftest=onscreen  Run the self-test with a visible window.\n"
           "  --                   Treat every following argument as a path (also one starting with '-').\n";
}

Result<CommandLine> parseCommandLine(std::span<const std::string_view> args) {
    CommandLine result;
    bool pathsOnly = false;
    // A second mode option (e.g. --version --selftest) is ambiguous, so it is an error.
    auto setMode = [&result](Mode mode, std::string_view option) -> Result<void> {
        if (result.mode != Mode::Gui && result.mode != mode) {
            return fail(ErrorCode::InvalidArgument, std::format("option {} conflicts with an earlier option", option));
        }
        result.mode = mode;
        return {};
    };

    for (const std::string_view arg : args) {
        // Anything starting with '-' is an option, so typos such as -version or -h are
        // reported; a path that starts with '-' must follow "--".
        if (pathsOnly || !arg.starts_with('-')) {
            result.paths.emplace_back(arg);
            continue;
        }
        Result<void> status;
        if (arg == "--") {
            pathsOnly = true;
        } else if (arg == "--version") {
            status = setMode(Mode::Version, arg);
        } else if (arg == "--help") {
            status = setMode(Mode::Help, arg);
        } else if (arg == "--selftest") {
            status = setMode(Mode::SelfTest, arg);
        } else if (arg == "--selftest=onscreen") {
            status = setMode(Mode::SelfTest, arg);
            result.selfTestOnScreen = true;
        } else {
            return fail(ErrorCode::InvalidArgument, std::format("unknown option {}", arg));
        }
        if (!status) {
            return std::unexpected(std::move(status.error()));
        }
    }
    return result;
}

} // namespace frost::app
