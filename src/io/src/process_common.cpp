// SPDX-License-Identifier: Apache-2.0
//
// Platform-independent parts of the process seam (see frost/io/process.hpp):
// spec validation, Windows command-line quoting and runToCompletion.

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include <algorithm>
#include <format>

namespace frost::io {
namespace {

Result<void> invalid(std::string message) { return fail(ErrorCode::InvalidArgument, std::move(message)); }

bool hasNul(std::string_view text) noexcept { return text.find('\0') != std::string_view::npos; }

// ASCII lower-casing for extension checks; non-ASCII bytes pass through.
char asciiLower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Whether `path` names a batch file as CreateProcessW would see it. Win32 path
// normalisation strips trailing dots and spaces, so "x.bat. " also runs x.bat.
bool isBatchFile(std::string_view path) noexcept {
    while (!path.empty() && (path.back() == '.' || path.back() == ' ')) {
        path.remove_suffix(1);
    }
    if (path.size() < 4) {
        return false;
    }
    std::string extension(path.substr(path.size() - 4));
    std::ranges::transform(extension, extension.begin(), asciiLower);
    return extension == ".bat" || extension == ".cmd";
}

} // namespace

Result<void> validateProcessSpec(const ProcessSpec& spec, bool windowsRules) {
    const PathStyle style = windowsRules ? PathStyle::Windows : PathStyle::Posix;
    if (hasNul(spec.executable)) {
        return invalid("executable path contains a NUL character");
    }
    if (!isAbsolutePath(spec.executable, style)) {
        return invalid(std::format("executable must be an absolute path, got '{}' (FrostIDE never searches PATH "
                                   "or the current directory; PLAN.md 3.5)",
                                   spec.executable));
    }
    if (windowsRules && isBatchFile(spec.executable)) {
        return fail(ErrorCode::Unsupported,
                    std::format("'{}' is a batch file; cmd.exe re-parses batch command lines, so they are not "
                                "launched directly",
                                spec.executable));
    }
    if (!spec.workingDirectory.empty() && !isAbsolutePath(spec.workingDirectory, style)) {
        return invalid(std::format("working directory must be an absolute path, got '{}'", spec.workingDirectory));
    }
    for (const std::string& argument : spec.arguments) {
        if (hasNul(argument)) {
            return invalid("argument contains a NUL character");
        }
    }
    for (const auto& [name, value] : spec.environment) {
        if (name.empty() || name.find('=') != std::string::npos || hasNul(name)) {
            return invalid(std::format("invalid environment variable name '{}'", name));
        }
        if (value && hasNul(*value)) {
            return invalid(std::format("environment variable '{}' contains a NUL character", name));
        }
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
