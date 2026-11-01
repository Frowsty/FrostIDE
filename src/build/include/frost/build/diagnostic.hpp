// SPDX-License-Identifier: Apache-2.0
//
// Build diagnostics: the Diagnostic record that the error list shows, the parser that reads one
// from a line of MSBuild console output, and DiagnosticSet, which drops duplicates and counts.
//
/*
 * The parser reads MSBuild's canonical message format, which MSBuild's console logger prints for
 * every error and warning and which the tools it runs print themselves:
 *
 *     origin: [subcategory] category [code]: text [project]
 *
 *   - origin: a file with an optional location, "file(line)", "file(line-line2)",
 *     "file(line,col)", "file(line,col-col2)" or "file(line,col,line2,col2)"; or a tool name
 *     ("LINK", "MSBUILD", "cl", "EXEC"); or absent ("error MSB1009: ..."). It may contain spaces
 *     and non-ASCII characters, but no ':' except a drive letter's ("C:\..."), because the first
 *     other ':' ends it.
 *   - subcategory: at most two words, such as "fatal" or "command line".
 *   - category: error, warning, message or note (any case); note is clang's, and both message and
 *     note map to Severity::Message.
 *   - code: one token with at least one digit (C2065, LNK2019, MSB3073, RC2135, D9002), or absent,
 *     as with clang-cl ("file(line,col): error: text"). A clang warning's trailing flag
 *     ("[-Wunused-variable]") becomes the code of a code-less diagnostic and leaves the text.
 *   - project: MSBuild appends " [<absolute project path>]" to the diagnostics it logs.
 *
 * Covered producers: cl.exe (C####, D####), link.exe (LNK####, "LINK : fatal error LNK1104"),
 * MSBuild itself (MSB####), rc.exe (RC####) and clang-cl with the ClangCL toolset. Leading
 * whitespace and MSBuild's multi-processor prefix ("12>") are skipped. Lines in other formats,
 * such as clang's GNU format ("file:line:col: error: text"), are not diagnostics here.
 * TODO(M4): GNU-format diagnostics for the CMake driver, and localized category words (I12).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace frost::build {

enum class Severity : std::uint8_t {
    Error,
    Warning,
    Message, // MSBuild's "message" category and clang's "note".
};

// "error", "warning" or "message".
[[nodiscard]] std::string_view toString(Severity severity) noexcept;

// One error, warning or message from a build, as the error list shows it.
struct Diagnostic {
    Severity severity = Severity::Error;
    std::string code{};    // "C2065", "LNK2019", "MSB3073", a clang flag ("-Wunused-variable"), or empty.
    std::string message{}; // The text after the code, without the trailing project.
    // The origin without its location, as printed: a path (absolute for cl, which MSBuild runs with
    // /FC), an object file ("main.obj"), a tool name ("LINK", "MSBUILD"), or empty. Only an
    // absolute path with line > 0 is a place to jump to.
    std::string file{};
    int line = 0;          // 1-based; 0 when the origin has no location.
    int column = 0;        // 1-based; 0 when the location has no column.
    std::string project{}; // The project file MSBuild appended in brackets, or empty.

    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

// Parses one line of build output (without its line terminator; a trailing CR is ignored) in the
// canonical format above. Returns nullopt for every other line. Pure; O(line length).
[[nodiscard]] std::optional<Diagnostic> parseDiagnosticLine(std::string_view line);

/*
 * The diagnostics of one build, in arrival order, without duplicates: a diagnostic equal in every
 * field to one already added is dropped (a header's warning seen again from another source file of
 * the same project, a message logged twice). Not thread-safe; the build session serializes calls.
 * Memory is O(total size of the unique diagnostics).
 */
class DiagnosticSet {
public:
    // Adds `diagnostic` unless it is a duplicate; returns whether it was added. O(log n).
    bool add(Diagnostic diagnostic);

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept { return m_items; }
    [[nodiscard]] std::size_t errorCount() const noexcept { return m_errors; }
    [[nodiscard]] std::size_t warningCount() const noexcept { return m_warnings; }
    [[nodiscard]] std::size_t messageCount() const noexcept { return m_messages; }

private:
    using Key = std::tuple<Severity, std::string, std::string, std::string, int, int, std::string>;
    std::vector<Diagnostic> m_items;
    std::set<Key> m_keys;
    std::size_t m_errors = 0;
    std::size_t m_warnings = 0;
    std::size_t m_messages = 0;
};

} // namespace frost::build
