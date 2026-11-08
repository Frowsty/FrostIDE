// SPDX-License-Identifier: Apache-2.0
//
// The real clangd as a test oracle (CMK-4, CPP-3): `clangd --check=<file>` with a
// --compile-commands-dir, parsed into the compile command clangd took from the database (after its
// own adjustments), the cc1 arguments its driver derived from it (the flags the parse really uses)
// and the diagnostics it reported. Header-only; the process goes through the real IProcessLauncher.

#pragma once

#include "frost/core/error.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace frost::test {

struct ClangdCheck {
    // "from CDB" (the database lists the file), "inferred" (clangd interpolated another entry's
    // command) or "fallback" (no database command at all).
    std::string origin;
    std::string command;             // The command line after "is: " (the directory in brackets first).
    std::string cc1;                 // After "internal (cc1) args are: ".
    std::vector<std::string> errors; // "Line N: message" of every E[...] [code] diagnostic line.
    std::string log;                 // Everything clangd wrote.
};

// The text after `marker` up to the end of its line in `log`, or "" when absent.
inline std::string clangdLogField(std::string_view log, std::string_view marker) {
    const std::size_t at = log.find(marker);
    if (at == std::string_view::npos)
        return {};
    const std::size_t begin = at + marker.size();
    std::size_t end = log.find('\n', begin);
    if (end == std::string_view::npos)
        end = log.size();
    std::string_view field = log.substr(begin, end - begin);
    if (!field.empty() && field.back() == '\r')
        field.remove_suffix(1);
    return std::string(field);
}

/* clangd's printed command without the switches that only name outputs (/Fo, /Fd, /FS; -o <file>):
 * the CMake synthesis leaves them out because they change no parse (cmake_compile_database.hpp). Tokens
 * are split as clangd prints them (double-quoted with backslash escapes when they need it). */
inline std::string withoutOutputSwitches(std::string_view command) {
    std::vector<std::string> tokens;
    std::string token;
    bool quoted = false;
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (quoted && c == '\\' && i + 1 < command.size()) {
            token += c;
            token += command[++i];
            continue;
        }
        if (c == '"')
            quoted = !quoted;
        if (c == ' ' && !quoted) {
            if (!token.empty())
                tokens.push_back(std::move(token));
            token.clear();
            continue;
        }
        token += c;
    }
    if (!token.empty())
        tokens.push_back(std::move(token));
    std::string out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        std::string_view bare = tokens[i];
        if (bare.starts_with('"'))
            bare.remove_prefix(1);
        if (bare.starts_with("/Fo") || bare.starts_with("/Fd") || bare == "/FS" || bare.starts_with("-Fo") ||
            bare.starts_with("-Fd"))
            continue;
        if (bare == "-o") {
            ++i;
            continue;
        }
        out += (out.empty() ? "" : " ") + tokens[i];
    }
    return out;
}

/*
 * Runs clangd --check on `file` with `compileCommandsDir` (none when empty) and config off, so
 * only the database decides; `--check-lines=1-1` keeps the per-token feature checks short. Errors:
 * the launch's, Timeout.
 */
inline Result<ClangdCheck> runClangdCheck(const std::string& clangd, const std::string& file,
                                          const std::string& compileCommandsDir,
                                          std::chrono::milliseconds timeout = std::chrono::minutes(5)) {
    io::ProcessSpec spec;
    spec.executable = clangd;
    spec.arguments = {"--check=" + file, "--check-lines=1-1", "--log=info", "--enable-config=false"};
    if (!compileCommandsDir.empty())
        spec.arguments.push_back("--compile-commands-dir=" + compileCommandsDir);
    spec.workingDirectory = std::string(io::parentPath(file));
    auto process = io::realProcessLauncher().launch(spec);
    if (!process)
        return std::unexpected(std::move(process.error()));
    auto result = (*process)->communicate({}, timeout);
    if (!result)
        return std::unexpected(std::move(result.error()));
    ClangdCheck check;
    check.log = result->stdoutBytes + result->stderrBytes;
    if (auto command = clangdLogField(check.log, "Compile command from CDB is: "); !command.empty()) {
        check.origin = "from CDB";
        check.command = std::move(command);
    } else if (check.log.find("Compile command inferred from") != std::string::npos) {
        check.origin = "inferred";
        check.command = clangdLogField(clangdLogField(check.log, "Compile command inferred from "), " is: ");
    } else if (auto fallback = clangdLogField(check.log, "Generic fallback command is: "); !fallback.empty()) {
        check.origin = "fallback";
        check.command = std::move(fallback);
    }
    check.cc1 = clangdLogField(check.log, "internal (cc1) args are: ");
    std::string_view rest = check.log;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        // E[08:50:29.993] [pp_hash_error] Line 3: "compiled for alpha"
        if (!line.starts_with("E[") || line.find("] [") == std::string_view::npos)
            continue;
        const std::size_t text = line.find("] ", line.find("] [") + 3);
        if (text != std::string_view::npos)
            check.errors.emplace_back(line.substr(text + 2));
    }
    return check;
}

} // namespace frost::test
