// SPDX-License-Identifier: Apache-2.0
//
// Platform-independent parts of the process seam (see frost/io/process.hpp):
// spec validation, Windows command-line quoting and runToCompletion.

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"

#include "internal.hpp"

#include <format>

namespace frost::io {
namespace {

Result<void> invalid(std::string message) { return fail(ErrorCode::InvalidArgument, std::move(message)); }

bool hasNul(std::string_view text) noexcept { return text.find('\0') != std::string_view::npos; }

// "\\.\..." is absolute to Win32 but refused by normalizePath with its own, more precise message.
bool isDeviceNamespace(std::string_view path, bool windowsRules) noexcept {
    return windowsRules && path.size() >= 4 && (path[0] == '\\' || path[0] == '/') &&
           (path[1] == '\\' || path[1] == '/') && path[2] == '.' && (path[3] == '\\' || path[3] == '/');
}

} // namespace

Result<void> validateProcessSpec(const ProcessSpec& spec, bool windowsRules) {
    const PathStyle style = windowsRules ? PathStyle::Windows : PathStyle::Posix;
    if (hasNul(spec.executable)) {
        return invalid("executable path contains a NUL character");
    }
    if (!isAbsolutePath(spec.executable, style) && !isDeviceNamespace(spec.executable, windowsRules)) {
        return invalid(std::format("executable must be an absolute path, got '{}' (FrostIDE never searches PATH "
                                   "or the current directory; PLAN.md 3.5)",
                                   spec.executable));
    }
    const auto batchFile = [&spec] {
        return fail(ErrorCode::Unsupported,
                    std::format("'{}' is a batch file; cmd.exe re-parses batch command lines, so they are not "
                                "launched directly",
                                spec.executable));
    };
    // Checked on the given spelling first, so "run.cmd." is reported as a batch file rather
    // than as a name with a trailing dot...
    if (windowsRules && detail::isBatchFileName(spec.executable)) {
        return batchFile();
    }
    // ...and on the normalised one, which is what the launchers start: "dir\t.bat\" and
    // "dir\t.bat\x\.." both normalise to "dir\t.bat".
    auto executable = normalizePath(spec.executable, style);
    if (!executable) {
        return std::unexpected(std::move(executable.error()).withContext("executable"));
    }
    if (windowsRules && detail::isBatchFileName(*executable)) {
        return batchFile();
    }
    if (!spec.workingDirectory.empty()) {
        if (!isAbsolutePath(spec.workingDirectory, style) && !isDeviceNamespace(spec.workingDirectory, windowsRules)) {
            return invalid(std::format("working directory must be an absolute path, got '{}'", spec.workingDirectory));
        }
        if (auto directory = normalizePath(spec.workingDirectory, style); !directory) {
            return std::unexpected(std::move(directory.error()).withContext("working directory"));
        }
        // The Windows launcher converts it to UTF-16; normalizePath does not check the encoding.
        if (windowsRules && !isValidUtf8(spec.workingDirectory)) {
            return invalid("working directory is not valid UTF-8");
        }
    }
    for (const std::string& argument : spec.arguments) {
        if (hasNul(argument)) {
            return invalid("argument contains a NUL character");
        }
    }
    if (windowsRules) {
        // The command line must be buildable and fit CreateProcessW's limit.
        auto commandLine = buildWindowsCommandLine(*executable, spec.arguments);
        if (!commandLine) {
            return std::unexpected(std::move(commandLine.error()));
        }
        auto units = utf8ToUtf16(*commandLine);
        if (!units) {
            return std::unexpected(std::move(units.error()).withContext("command line"));
        }
        if (units->size() >= detail::kMaxWindowsCommandLineUnits) {
            return invalid(std::format("command line for '{}' is {} UTF-16 units; Windows allows {}", spec.executable,
                                       units->size(), detail::kMaxWindowsCommandLineUnits - 1));
        }
    }
    for (const auto& [name, value] : spec.environment) {
        if (name.empty() || name.find('=') != std::string::npos || hasNul(name)) {
            return invalid(std::format("invalid environment variable name '{}'", name));
        }
        if (value && hasNul(*value)) {
            return invalid(std::format("environment variable '{}' contains a NUL character", name));
        }
        // The Windows environment block is UTF-16, so both must convert.
        if (windowsRules && !isValidUtf8(name)) {
            return invalid("environment variable name is not valid UTF-8");
        }
        if (windowsRules && value && !isValidUtf8(*value)) {
            return invalid(std::format("environment variable '{}' is not valid UTF-8", name));
        }
    }
    // STARTUPINFOW::lpDesktop is a NUL-terminated UTF-16 string; POSIX ignores the field.
    if (windowsRules && (hasNul(spec.desktop) || !isValidUtf8(spec.desktop))) {
        return invalid("desktop name contains a NUL character or is not valid UTF-8");
    }
    if (spec.stdinMode == Stdio::MergeIntoStdout || spec.stdoutMode == Stdio::MergeIntoStdout) {
        return invalid("Stdio::MergeIntoStdout applies to stderr only");
    }
    return {};
}

void appendWindowsArgument(std::string& commandLine, std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        commandLine += argument; // Backslashes not followed by a quote are literal, so no escaping is needed.
        return;
    }
    commandLine += '"';
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // 2n+1 backslashes and a quote parse as n backslashes and a literal quote.
            commandLine.append(backslashes * 2 + 1, '\\');
        } else {
            commandLine.append(backslashes, '\\');
        }
        backslashes = 0;
        commandLine += c;
    }
    // Backslashes before the closing quote are doubled so the quote stays a delimiter.
    commandLine.append(backslashes * 2, '\\');
    commandLine += '"';
}

Result<std::string> buildWindowsCommandLine(std::string_view executable, const std::vector<std::string>& arguments) {
    if (executable.find('"') != std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, std::format("executable path '{}' contains a quote", executable));
    }
    // argv[0] is parsed without escapes: everything between the quotes is taken literally.
    std::string commandLine = "\"";
    commandLine += executable;
    commandLine += '"';
    for (const std::string& argument : arguments) {
        commandLine += ' ';
        appendWindowsArgument(commandLine, argument);
    }
    return commandLine;
}

// The defaults for IProcess implementations that cannot see their container (process.hpp).
Result<std::vector<ContainedProcess>> IProcess::containedProcesses() {
    return fail(ErrorCode::Unsupported, "this process cannot list its container");
}

Result<void> IProcess::terminateContained(ProcessId pid) {
    return fail(ErrorCode::Unsupported, std::format("this process cannot end process {} of its container", pid));
}

Result<void> IProcess::releaseContainer() {
    return fail(ErrorCode::Unsupported, "this process cannot release its container");
}

void IProcess::abandonOutput() noexcept {}

Result<ProcessResult> runToCompletion(IProcessLauncher& launcher, const ProcessSpec& spec, std::string_view stdinData,
                                      IClock::Duration timeout) {
    auto process = launcher.launch(spec);
    if (!process) {
        return std::unexpected(std::move(process.error()).withContext("while running '" + spec.executable + "'"));
    }
    auto result = (*process)->communicate(stdinData, timeout);
    if (!result) {
        return std::unexpected(std::move(result.error()).withContext("while running '" + spec.executable + "'"));
    }
    return result;
}

} // namespace frost::io
