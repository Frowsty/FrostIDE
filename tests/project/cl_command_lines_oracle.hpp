// SPDX-License-Identifier: Apache-2.0
//
// The GetClCommandLines oracle of SLN-5: this machine's MSBuild.exe, run by the test itself, builds
// the Visual C++ design-time target GetClCommandLines through a targets file the test writes
// (ForceImportAfterCppTargets) and writes each ClCommandLines item, IncludePath and
// ExternalIncludePath to a text file. Its answers are the expected values of the ProjectHost,
// clangd and corpus tests. Header-only; Windows test code only. Needs only public headers, so the
// lang tests can use it too.
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/cl_command_lines.hpp"
#include "frost/project/project_host.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {

// gtest's printers for failure messages (found by argument-dependent lookup).
inline void PrintTo(const ClCommandLine& line, std::ostream* out) {
    *out << "{commandLine: \"" << line.commandLine << "\", workingDirectory: \"" << line.workingDirectory
         << "\", configurationOptions: " << line.configurationOptions << ", files:";
    for (const std::string& file : line.files) {
        *out << " " << file;
    }
    *out << "}";
}

inline void PrintTo(const ClangdCompileCommand& command, std::ostream* out) {
    *out << "{file: " << command.file << ", directory: " << command.directory << ", arguments:";
    for (const std::string& argument : command.arguments) {
        *out << " " << argument;
    }
    *out << "}";
}

} // namespace frost::project

namespace frost::project::test {

// The oracle's targets file: one line per ClCommandLines item ("CMD|" files "|" working directory
// "|" ConfigurationOptions "|" command line; transforms keep a metadata value's ';' in one line),
// then a "#INCLUDEPATH" and an "#EXTERNALINCLUDEPATH" line, each followed by one directory per line
// (WriteLinesToFile splits a property on ';').
inline constexpr std::string_view kClCommandLinesOracleTargets = R"xml(<Project>
  <Target Name="FrostOracleClCommandLines" DependsOnTargets="GetClCommandLines">
    <WriteLinesToFile File="$(FrostOracleOut)" Overwrite="true"
        Lines="@(ClCommandLines->'CMD|%(Files)|%(WorkingDirectory)|%(ConfigurationOptions)|%(Identity)')" />
    <WriteLinesToFile File="$(FrostOracleOut)" Lines="#INCLUDEPATH;$(IncludePath)" />
    <WriteLinesToFile File="$(FrostOracleOut)" Lines="#EXTERNALINCLUDEPATH;$(ExternalIncludePath)" />
  </Target>
</Project>
)xml";

// `path` normalised, ".." and "." folded (std::filesystem's lexical rules).
inline std::string oracleNormalized(std::string_view path) {
    const std::string folded = std::filesystem::path(std::string(path)).lexically_normal().string();
    return io::normalizePath(folded).value_or(folded);
}

/*
 * GetClCommandLines of one project configuration from MSBuild.exe, with the global properties the
 * product passes (clCommandLinesGlobalProperties). Paths are normalised like the product's;
 * __temporary.cpp, the target's placeholder for project-wide options, is skipped like the
 * product's. nullopt (with a test failure) when MSBuild fails.
 */
inline std::optional<ProjectClCommandLines> runClCommandLinesOracle(const std::string& msbuild,
                                                                    const ProjectEvaluationRequest& request) {
    frost::test::ScratchDir scratch("oracle-gcl");
    auto& fs = io::realFileSystem();
    EXPECT_TRUE(fs.createDirectories(scratch.path()).has_value());
    const std::string targets = scratch.join("oracle.targets");
    const std::string out = scratch.join("lines.txt");
    EXPECT_TRUE(fs.writeFileAtomic(targets, kClCommandLinesOracleTargets).has_value());
    io::ProcessSpec spec;
    spec.executable = msbuild;
    spec.arguments = {request.projectPath,
                      "-nologo",
                      "-nodeReuse:false",
                      "-v:q",
                      "-t:FrostOracleClCommandLines",
                      "-p:ForceImportAfterCppTargets=" + targets,
                      "-p:FrostOracleOut=" + out};
    for (const auto& [name, value] : clCommandLinesGlobalProperties(request)) {
        spec.arguments.push_back("-p:" + name + "=" + value);
    }
    spec.workingDirectory = std::string(io::parentPath(request.projectPath));
    spec.environment = {{"MSBUILDDISABLENODEREUSE", "1"}, {"MSBUILDTERMINALLOGGER", "off"}};
    auto launched = io::realProcessLauncher().launch(spec);
    if (!launched) {
        ADD_FAILURE() << launched.error().describe();
        return std::nullopt;
    }
    auto ran = (*launched)->communicate({}, std::chrono::minutes(5));
    auto text = fs.readFile(out);
    if (!ran || ran->exitCode != 0 || !text) {
        ADD_FAILURE() << "the GetClCommandLines oracle failed for " << request.projectPath << ": "
                      << (ran ? ran->stdoutBytes + ran->stderrBytes : ran.error().describe());
        return std::nullopt;
    }
    ProjectClCommandLines lines{.projectPath = oracleNormalized(request.projectPath),
                                .configuration = request.configuration,
                                .platform = request.platform,
                                .commandLines = {},
                                .includePath = {},
                                .externalIncludePath = {}};
    std::vector<std::string>* section = nullptr;
    std::string_view all(*text);
    if (all.starts_with("\xEF\xBB\xBF")) {
        all.remove_prefix(3);
    }
    std::size_t start = 0;
    while (start < all.size()) {
        const std::size_t end = std::min(all.find('\n', start), all.size());
        std::string_view line = all.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && line.back() == '\r') { // A command line may end in a space; it stays.
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (line == "#INCLUDEPATH") {
            section = &lines.includePath;
            continue;
        }
        if (line == "#EXTERNALINCLUDEPATH") {
            section = &lines.externalIncludePath;
            continue;
        }
        if (line.starts_with("CMD|")) {
            section = nullptr;
            std::vector<std::string_view> fields;
            std::string_view rest = line.substr(4);
            for (int i = 0; i < 3; ++i) {
                const std::size_t bar = rest.find('|');
                fields.push_back(rest.substr(0, bar));
                rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
            }
            ClCommandLine command;
            command.commandLine = std::string(rest);
            command.workingDirectory = fields[1].empty() ? std::string() : oracleNormalized(fields[1]);
            command.configurationOptions = fields[2] == "true" || fields[2] == "True";
            std::string_view files = fields[0];
            while (!files.empty()) {
                const std::size_t semicolon = files.find(';');
                const std::string_view file = files.substr(0, semicolon);
                files = semicolon == std::string_view::npos ? std::string_view{} : files.substr(semicolon + 1);
                if (!file.empty() && io::fileName(file) != "__temporary.cpp") {
                    command.files.push_back(oracleNormalized(file));
                }
            }
            lines.commandLines.push_back(std::move(command));
            continue;
        }
        if (section != nullptr) {
            // MSBuild's CL task runs cl.exe in the project's directory with INCLUDE and
            // EXTERNAL_INCLUDE set: a relative directory is relative to it.
            std::string directory(line);
            if (!io::isAbsolutePath(directory)) {
                directory = io::joinPath(io::parentPath(lines.projectPath), directory).value_or(directory);
            }
            section->push_back(oracleNormalized(directory));
        }
    }
    return lines;
}

} // namespace frost::project::test
