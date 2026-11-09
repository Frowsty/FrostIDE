// SPDX-License-Identifier: Apache-2.0
//
// GetClCommandLines to clangd (frost/project/cl_command_lines.hpp): the MSVC command-line
// splitter and the translation into compilation-database entries (SLN-5).

#include "frost/project/cl_command_lines.hpp"

#include "frost/io/path.hpp"

#include "paths.hpp"
#include "text_util.hpp"

#include <array>
#include <unordered_set>

namespace frost::project {

std::vector<std::string> splitMsvcCommandLine(std::string_view commandLine) {
    std::vector<std::string> out;
    std::string current;
    bool inArgument = false;
    bool quoted = false;
    std::size_t i = 0;
    while (i < commandLine.size()) {
        const char c = commandLine[i];
        if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (inArgument) {
                out.push_back(std::move(current));
                current.clear();
                inArgument = false;
            }
            ++i;
            continue;
        }
        inArgument = true;
        if (c == '\\') {
            std::size_t backslashes = 0;
            while (i < commandLine.size() && commandLine[i] == '\\') {
                ++backslashes;
                ++i;
            }
            if (i < commandLine.size() && commandLine[i] == '"') {
                current.append(backslashes / 2, '\\');
                if (backslashes % 2 == 1) {
                    current += '"';
                    ++i;
                }
                // An even count leaves the quote to the next iteration, which toggles quoting.
            } else {
                current.append(backslashes, '\\');
            }
            continue;
        }
        if (c == '"') {
            if (quoted && i + 1 < commandLine.size() && commandLine[i + 1] == '"') {
                current += '"';
                i += 2;
                continue;
            }
            quoted = !quoted;
            ++i;
            continue;
        }
        current += c;
        ++i;
    }
    if (inArgument) {
        out.push_back(std::move(current));
    }
    return out;
}

bool isDroppedClSwitch(std::string_view argument) noexcept {
    if (argument.size() < 2 || (argument.front() != '/' && argument.front() != '-')) {
        return false;
    }
    const std::string_view name = argument.substr(1);
    // Output files. Case matters: /FI is a forced include and /FU a forced using, both kept.
    static constexpr std::array<std::string_view, 10> kOutputs{"Fo", "Fd", "Fp", "Fa", "FA",
                                                               "Fe", "Fm", "FR", "Fr", "Fi"};
    for (std::string_view output : kOutputs) {
        if (name.starts_with(output)) {
            return true;
        }
    }
    return name.starts_with('Y') || name.starts_with("MP") || name.starts_with("errorReport");
}

std::string clangTargetForPlatform(std::string_view platform) {
    if (detail::iequals(platform, "x64")) {
        return "x86_64-pc-windows-msvc";
    }
    if (detail::iequals(platform, "Win32") || detail::iequals(platform, "x86")) {
        return "i686-pc-windows-msvc";
    }
    if (detail::iequals(platform, "ARM64")) {
        return "aarch64-pc-windows-msvc";
    }
    if (detail::iequals(platform, "ARM")) {
        return "armv7-pc-windows-msvc";
    }
    return {};
}

std::vector<std::string> translatedClArguments(std::string_view commandLine) {
    std::vector<std::string> out;
    for (std::string& argument : splitMsvcCommandLine(commandLine)) {
        if (!isDroppedClSwitch(argument)) {
            out.push_back(std::move(argument));
        }
    }
    return out;
}

std::vector<ClangdCompileCommand> clangdCommands(const ProjectClCommandLines& lines, std::string_view compiler) {
    std::vector<ClangdCompileCommand> out;
    std::vector<std::string> system;
    std::unordered_set<std::string> systemSeen;
    for (const std::vector<std::string>* list : {&lines.includePath, &lines.externalIncludePath}) {
        for (const std::string& directory : *list) {
            if (!directory.empty() && systemSeen.insert(detail::pathKey(directory)).second) {
                system.push_back("/imsvc" + directory);
            }
        }
    }
    const std::string target = clangTargetForPlatform(lines.platform);
    const std::string projectDirectory(io::parentPath(lines.projectPath));
    std::unordered_set<std::string> filesSeen;
    for (const ClCommandLine& line : lines.commandLines) {
        std::vector<std::string> base{std::string(compiler)};
        if (!target.empty()) {
            base.push_back("--target=" + target);
        }
        for (std::string& argument : translatedClArguments(line.commandLine)) {
            base.push_back(std::move(argument));
        }
        base.insert(base.end(), system.begin(), system.end());
        for (const std::string& file : line.files) {
            if (file.empty() || !filesSeen.insert(detail::pathKey(file)).second) {
                continue;
            }
            ClangdCompileCommand command{.directory =
                                             line.workingDirectory.empty() ? projectDirectory : line.workingDirectory,
                                         .file = file,
                                         .arguments = base};
            command.arguments.push_back(file);
            out.push_back(std::move(command));
        }
    }
    return out;
}

} // namespace frost::project
