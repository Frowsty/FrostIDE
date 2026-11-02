// SPDX-License-Identifier: Apache-2.0
//
// Implementation of frost's command-line parser (see frost/app/command_line.hpp).

#include "frost/app/command_line.hpp"

#include <charconv>
#include <format>
#include <limits>
#include <optional>

namespace frost::app {

std::string_view usageText() noexcept {
    return "Usage: frost [options] [path...]\n"
           "\n"
           "Options:\n"
           "  --project <path>     Open a project folder or .frostproj first.\n"
           "  --line <number>      Open the file at a 1-based line (0 keeps its position).\n"
           "  --column <number>    Open the file at a 1-based column (0 keeps its position).\n"
           "  --column-utf16       Interpret --column as UTF-16 code units.\n"
           "  --version            Print version information and exit.\n"
           "  --help               Print this help and exit.\n"
           "  --selftest           Run the built-in self-test offscreen and exit with its status.\n"
           "  --selftest=onscreen  Run the self-test with a visible window.\n"
           "  --selftest=crash     Start the crash handler, then crash on purpose (writes a crash report).\n"
           "  --selftest=freeze    Block the user interface for 300 ms (writes a freeze report).\n"
           "  --selftest=render    Draw an editor with the default renderer on the real display and exit.\n"
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

    bool explicitLine = false;
    bool explicitColumn = false;
    auto number = [](std::string_view value) -> std::optional<int> {
        int result = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (value.empty() || error != std::errc{} || end != value.data() + value.size() || result < 0)
            return std::nullopt;
        return result;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        // Anything starting with '-' is an option, so typos such as -version or -h are
        // reported; a path that starts with '-' must follow "--".
        if (pathsOnly || !arg.starts_with('-')) {
            result.paths.emplace_back(arg);
            continue;
        }
        Result<void> status;
        if (arg == "--") {
            pathsOnly = true;
        } else if (arg == "--project" || arg == "--line" || arg == "--column") {
            if (++i >= args.size() || args[i].empty() || args[i].starts_with("--"))
                return fail(ErrorCode::InvalidArgument, std::format("option {} needs a value", arg));
            if (arg == "--project") {
                if (!result.project.empty())
                    return fail(ErrorCode::InvalidArgument, "--project may only be specified once");
                result.project = args[i];
            } else {
                auto value = number(args[i]);
                if (!value)
                    return fail(ErrorCode::InvalidArgument, std::format("{} requires a non-negative integer", arg));
                if (arg == "--line") {
                    if (explicitLine)
                        return fail(ErrorCode::InvalidArgument, "--line may only be specified once");
                    explicitLine = true;
                    result.line = *value;
                } else {
                    if (explicitColumn)
                        return fail(ErrorCode::InvalidArgument, "--column may only be specified once");
                    explicitColumn = true;
                    result.column = *value;
                }
            }
        } else if (arg == "--column-utf16") {
            if (result.columnUtf16)
                return fail(ErrorCode::InvalidArgument, "--column-utf16 may only be specified once");
            result.columnUtf16 = true;
        } else if (arg == "--version") {
            status = setMode(Mode::Version, arg);
        } else if (arg == "--help") {
            status = setMode(Mode::Help, arg);
        } else if (arg == "--selftest") {
            status = setMode(Mode::SelfTest, arg);
        } else if (arg == "--selftest=onscreen") {
            status = setMode(Mode::SelfTest, arg);
            result.selfTestOnScreen = true;
        } else if (arg == "--selftest=render") {
            status = setMode(Mode::SelfTest, arg);
            result.selfTestKind = SelfTestKind::Render;
        } else if (arg == "--selftest=crash" || arg == "--selftest=freeze") {
            // Deliberate faults: never combined with anything that could change what they test.
            if (args.size() != 1) {
                return fail(ErrorCode::InvalidArgument, std::format("option {} must be the only argument", arg));
            }
            status = setMode(Mode::SelfTest, arg);
            result.selfTestKind = arg == "--selftest=crash" ? SelfTestKind::Crash : SelfTestKind::Freeze;
        } else {
            return fail(ErrorCode::InvalidArgument, std::format("unknown option {}", arg));
        }
        if (!status) {
            return std::unexpected(std::move(status.error()));
        }
    }
    if (result.columnUtf16 && (!explicitColumn || result.column == 0))
        return fail(ErrorCode::InvalidArgument, "--column-utf16 requires a positive --column");
    if (result.mode != Mode::Gui && (!result.project.empty() || explicitLine || explicitColumn || result.columnUtf16))
        return fail(ErrorCode::InvalidArgument, "project and location options require GUI mode");
    if ((explicitLine || explicitColumn) && result.paths.size() != 1)
        return fail(ErrorCode::InvalidArgument, "--line and --column require exactly one file path");
    // FrostEngine passes a single argv item as path:line:column. Parse from the right so a
    // Windows drive colon and colons elsewhere in the path are preserved.
    if (!explicitLine && !explicitColumn && result.paths.size() == 1) {
        std::string& path = result.paths.front();
        const auto last = path.rfind(':');
        if (last != std::string::npos && last > 1) {
            auto column = number(std::string_view(path).substr(last + 1));
            const auto second = path.rfind(':', last - 1);
            if (column && second != std::string::npos && second > 0) {
                auto line = number(std::string_view(path).substr(second + 1, last - second - 1));
                if (line) {
                    result.line = *line;
                    result.column = *column;
                    path.resize(second);
                }
            }
        }
    }
    return result;
}

} // namespace frost::app
