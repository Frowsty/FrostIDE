// SPDX-License-Identifier: Apache-2.0
//
// Output parsers: what turns a build tool's output lines into diagnostics. BuildSession runs one
// per output stream (BuildSessionOptions::parser); the MSBuild driver keeps the canonical-format
// parser, and the CMake driver (ADR 0046) uses the compiler parser for builds and the CMake parser
// for configure runs.
//
/*
 * GNU format (parseGnuDiagnosticLine), as GCC and Clang print it on every OS:
 *
 *     file:line:col: severity: message [flag]
 *     file:line: severity: message
 *     tool: severity: message                           (clang, ld.lld, collect2, ...)
 *     In file included from file:line[:col][:,]
 *                      from file:line[:col][:,]
 *
 *   - file may be a Windows drive path ("C:/src/a.c", "C:\src\a.c"); its drive colon is skipped.
 *     The tool form needs an origin without spaces or path separators.
 *   - severity: error, fatal error (both Error), warning, note and remark (both Message).
 *   - A trailing "[-Wflag]" or "[-Werror,-Wflag]" becomes the code ("-Wflag") and leaves the text.
 *   - The include-chain lines become Message diagnostics at the including file and line, with the
 *     text "In file included from here", so the error list shows where the header came from.
 *   - ANSI colour escapes are removed first (a user's CMAKE_COLOR_DIAGNOSTICS=ON).
 *
 * CMake format (CMakeOutputParser), as `cmake` prints messages during configure and generate:
 *
 *     CMake Error at CMakeLists.txt:12 (message):       Error (also "CMake Error in <file>:")
 *       the message, indented by two spaces,
 *       possibly over several lines
 *     <blank line>
 *     Call Stack (most recent call first):              ignored, with its indented frames
 *       sub/CMakeLists.txt:3 (include)
 *
 *   "CMake Warning", "CMake Warning (dev)" and "CMake Deprecation Warning" give Warnings; the
 *   one-line form "CMake Error: text" gives an Error without a file. A message's lines are joined
 *   with single spaces. A block ends at the next line that is neither blank nor indented, or at the
 *   end of the stream. The file is as CMake printed it: relative to the top-level source directory
 *   or absolute (the CMake service resolves it). Only lines starting with "CMake " at column 0 open
 *   a block, so the project's own message() output is never mistaken for one.
 *
 * Thread safety: a parser instance is used by one stream at a time (the session serializes).
 */
#pragma once

#include "frost/build/diagnostic.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace frost::build {
// SLN-10 event records emitted by FrostBuildLogger (also binary-log replay).
[[nodiscard]] std::optional<Diagnostic> parseStructuredMsBuildLine(std::string_view line);

// Receives each diagnostic a parser recognised.
using DiagnosticSink = std::function<void(Diagnostic diagnostic)>;

// Turns the lines of one output stream into diagnostics; may keep state across lines.
class IOutputParser {
public:
    virtual ~IOutputParser() = default;
    // One line (without its terminator). Calls `sink` for each diagnostic it completes.
    virtual void feed(std::string_view line, const DiagnosticSink& sink) = 0;
    // The end of the stream: completes a diagnostic still being read. The default does nothing.
    virtual void finish(const DiagnosticSink& sink);

protected:
    IOutputParser() = default;
    IOutputParser(const IOutputParser&) = default;
    IOutputParser& operator=(const IOutputParser&) = default;
};

// Makes one parser per stream for a BuildSession.
using OutputParserFactory = std::function<std::unique_ptr<IOutputParser>()>;

// Parses one line in the GNU format (header comment); nullopt for every other line. Pure; O(line).
[[nodiscard]] std::optional<Diagnostic> parseGnuDiagnosticLine(std::string_view line);

// MSBuild's canonical format (parseDiagnosticLine: MSBuild, cl, link, clang-cl) first, then GNU.
[[nodiscard]] std::optional<Diagnostic> parseCompilerDiagnosticLine(std::string_view line);

// parseDiagnosticLine per line: the MSBuild driver's parser.
[[nodiscard]] std::unique_ptr<IOutputParser> makeStructuredMsBuildOutputParser();
[[nodiscard]] std::unique_ptr<IOutputParser> makeMsBuildOutputParser();
// parseCompilerDiagnosticLine per line: `cmake --build` with any compiler.
[[nodiscard]] std::unique_ptr<IOutputParser> makeCompilerOutputParser();
// CMake's message blocks (header comment): configure runs.
[[nodiscard]] std::unique_ptr<IOutputParser> makeCMakeOutputParser();

} // namespace frost::build
