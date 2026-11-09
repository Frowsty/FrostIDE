// SPDX-License-Identifier: Apache-2.0
//
// The oracle of SLN-9's acceptance line "the error list equals the diagnostics MSBuild printed":
// runs this machine's MSBuild.exe itself (through IProcessLauncher) with MSBuild's own file
// loggers, one with errors only and one with warnings only, and reads back what MSBuild logged with
// a small parser of its own (not frost::build's), so a test can compare FrostIDE's error list with
// MSBuild's record of the same build. Header-only; Windows test code only.
#pragma once

#include "frost/build/build_diagnostics.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace frost::test {

// One logged error or warning: what the error list must show for it.
struct LoggedDiagnostic {
    std::string severity; // "error" or "warning" ("fatal error" is an error).
    std::string code;     // "C2065", "MSB8065"; "" without one.
    std::string file;     // The origin without its location, lower-cased, '\' separators.
    int line = 0;
    int column = 0;
    std::string message; // Without the trailing " [project]".

    friend auto operator<=>(const LoggedDiagnostic&, const LoggedDiagnostic&) = default;
};

inline std::string oracleFileKey(std::string_view file) {
    std::string key(file);
    for (char& c : key) {
        c = c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return key;
}

/*
 * One line of MSBuild's file logger in the canonical format,
 *     <origin>[(<line>[,<col>...])]: [fatal ]error|warning [<code>]: <text> [<project>]
 * or nullopt for any other line. Independent of frost::build::parseDiagnosticLine on purpose.
 */
inline std::optional<LoggedDiagnostic> parseLoggedDiagnostic(std::string_view text) {
    static const std::regex pattern(
        R"(^\s*(?:\d+>)?\s*(.*?)(?:\((\d+)(?:[,-](\d+))?[^)]*\))?\s*:\s*(fatal error|error|warning)\s+([A-Za-z]+\d+)?\s*:\s*(.*?)(?:\s+\[[^\]]+\])?\s*$)");
    std::string line(text);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::smatch match;
    if (!std::regex_match(line, match, pattern)) {
        return std::nullopt;
    }
    LoggedDiagnostic d;
    d.file = oracleFileKey(match[1].str());
    d.line = match[2].matched ? std::stoi(match[2].str()) : 0;
    d.column = match[3].matched ? std::stoi(match[3].str()) : 0;
    d.severity = match[4].str() == "warning" ? "warning" : "error";
    d.code = match[5].matched ? match[5].str() : "";
    d.message = match[6].str();
    return d;
}

// The error list's rows as LoggedDiagnostic values (errors and warnings; messages are not logged
// by the errors-only and warnings-only loggers).
inline std::set<LoggedDiagnostic> errorListRows(std::span<const build::BuildDiagnostic> rows) {
    std::set<LoggedDiagnostic> result;
    for (const build::BuildDiagnostic& row : rows) {
        const build::Diagnostic& d = row.diagnostic;
        if (d.severity == build::Severity::Message) {
            continue;
        }
        result.insert({.severity = d.severity == build::Severity::Warning ? "warning" : "error",
                       .code = d.code,
                       .file = oracleFileKey(d.file),
                       .line = d.line,
                       .column = d.column,
                       .message = d.message});
    }
    return result;
}

inline std::string describe(const std::set<LoggedDiagnostic>& diagnostics) {
    std::string text;
    for (const LoggedDiagnostic& d : diagnostics) {
        text += std::format("\n  {}({},{}): {} {}: {}", d.file, d.line, d.column, d.severity, d.code, d.message);
    }
    return text.empty() ? std::string(" (none)") : text;
}

struct OracleBuild {
    int exitCode = -1;
    std::set<LoggedDiagnostic> diagnostics; // From the two file loggers, duplicates folded.
    std::size_t loggedLines = 0;            // Diagnostic lines in the logs, duplicates included.
};

/*
 * Runs `msbuild` with `arguments` (the build: file, -t, -p...) plus -nologo -nodeReuse:false
 * -noconlog and the two file loggers writing into `scratchDir` (absolute, existing), in
 * `workingDirectory`, with English output (as FrostIDE asks for), and reads the logs back. The
 * process tree is contained and ends with the build.
 */
inline std::optional<OracleBuild> runMsBuildOracle(const std::string& msbuild, std::vector<std::string> arguments,
                                                   const std::string& workingDirectory, const std::string& scratchDir) {
    const std::string errors = scratchDir + "\\oracle-errors.log";
    const std::string warnings = scratchDir + "\\oracle-warnings.log";
    io::ProcessSpec spec;
    spec.executable = msbuild;
    spec.arguments = std::move(arguments);
    for (const char* extra : {"-nologo", "-nodeReuse:false", "-noconlog"}) {
        spec.arguments.emplace_back(extra);
    }
    spec.arguments.push_back("-flp1:logfile=" + errors + ";errorsonly");
    spec.arguments.push_back("-flp2:logfile=" + warnings + ";warningsonly");
    spec.workingDirectory = workingDirectory;
    spec.environment = {{"DOTNET_CLI_UI_LANGUAGE", "en-US"},
                        {"VSLANG", "1033"},
                        {"MSBUILDTERMINALLOGGER", "off"},
                        {"MSBUILDDISABLENODEREUSE", "1"}};
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    auto ran = io::runToCompletion(io::realProcessLauncher(), spec, {}, std::chrono::minutes(10));
    if (!ran) {
        ADD_FAILURE() << "the MSBuild oracle did not run: " << ran.error().describe();
        return std::nullopt;
    }
    OracleBuild result;
    result.exitCode = ran->exitCode;
    for (const std::string& log : {errors, warnings}) {
        auto content = io::realFileSystem().readFile(log);
        if (!content) {
            ADD_FAILURE() << "the MSBuild oracle wrote no " << log << ": " << content.error().describe();
            return std::nullopt;
        }
        if (content->starts_with("\xEF\xBB\xBF")) {
            content->erase(0, 3); // The loggers write a UTF-8 BOM.
        }
        std::istringstream lines(*content);
        std::string line;
        while (std::getline(lines, line)) {
            if (auto d = parseLoggedDiagnostic(line)) {
                ++result.loggedLines;
                result.diagnostics.insert(std::move(*d));
            }
        }
    }
    return result;
}

} // namespace frost::test
