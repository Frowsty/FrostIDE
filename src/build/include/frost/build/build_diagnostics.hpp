// SPDX-License-Identifier: Apache-2.0
//
// Build diagnostics as the error list and the editor's marks use them: each Diagnostic resolved
// to an absolute file with a decision whether a click can jump there (BuildDiagnostic), the index
// that groups and counts them and answers "what was reported for this document", and the reader
// of MSBuild's per-project output lines behind the build service's progress (ADR 0035).
//
/*
 * Resolution (DiagnosticResolver::resolve). Diagnostic::file is the origin as MSBuild printed it:
 *   - a tool name ("LINK", "MSBUILD", "cl", "EXEC", "lld-link.exe"): no path, never jumpable. A
 *     tool name is an origin without a path separator whose extension is empty or ".exe";
 *   - an absolute path: normalised (io::normalizePath);
 *   - a relative path: resolved against the directory of the reporting project
 *     (Diagnostic::project, which MSBuild appends), else against the solution's directory;
 *   - empty: no path.
 * A diagnostic is jumpable when it has a path, line > 0, the path is not a build product (an
 * object file, library, executable, PDB or other binary: see isBuildProduct) and the file system
 * reports a regular file there. Each path is checked once per resolver (one build).
 *
 * Document identity. Paths are compared by documentKey: normalised, with ASCII case ignored on
 * Windows, so "c:\SRC\a.cpp" and "C:\src\a.cpp" name the same document there.
 * TODO(M2): non-ASCII case folding and final-path resolution (symlinks, 8.3 names).
 */
#pragma once

#include "frost/build/diagnostic.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::build {

// A diagnostic as the error list shows it.
struct BuildDiagnostic {
    Diagnostic diagnostic{}; // As parsed from the build output.
    std::string path{};      // Absolute and normalised; "" for tool origins and diagnostics without a file.
    bool jumpable = false;   // A real file with line > 0: the error list and the editor can go there.

    friend bool operator==(const BuildDiagnostic&, const BuildDiagnostic&) = default;
};

struct DiagnosticCounts {
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t messages = 0;

    friend bool operator==(const DiagnosticCounts&, const DiagnosticCounts&) = default;
};

// Whether an origin names a tool rather than a file (see the header comment). Pure.
[[nodiscard]] bool isToolOrigin(std::string_view origin) noexcept;

// Whether `path` names a build product by its extension (ASCII case ignored): .obj .o .lib .a .exe
// .dll .so .dylib .pdb .res .exp .ilk .idb .pch .ipch .tlog. Pure.
[[nodiscard]] bool isBuildProduct(std::string_view path) noexcept;

// The key documents are compared by (header comment); "" when `path` is not absolute.
[[nodiscard]] std::string documentKey(std::string_view path);

/*
 * Resolves the diagnostics of one build (header comment). `solutionDirectory` (absolute; may be
 * empty when there is no solution) is the fallback base for relative paths. The file system must
 * outlive the resolver. Not thread-safe: the build session serializes its diagnostics.
 */
class DiagnosticResolver {
public:
    DiagnosticResolver(io::IFileSystem& fs, std::string solutionDirectory);

    // O(path length) plus one status() call per new path.
    [[nodiscard]] BuildDiagnostic resolve(const Diagnostic& diagnostic);

private:
    io::IFileSystem& m_fs;
    std::string m_solutionDirectory;
    std::unordered_map<std::string, bool> m_isFile; // documentKey -> a regular file exists there.
};

// The diagnostics reported for one file, or for one tool origin.
struct DiagnosticFileGroup {
    // BuildDiagnostic::path; for diagnostics without one, the origin as printed ("LINK") or "".
    std::string file{};
    bool isPath = false; // `file` is a path (the group of a document), not a tool origin.
    std::vector<BuildDiagnostic> diagnostics{};
    DiagnosticCounts counts{};
};

/*
 * The diagnostics of one build in arrival order, with counts per severity, groups per file and
 * a per-document query. Not thread-safe (the build service guards it). Memory is O(diagnostics).
 */
class DiagnosticIndex {
public:
    // Appends `diagnostic` (the session has dropped duplicates already). O(1) amortised.
    void add(BuildDiagnostic diagnostic);
    void clear() noexcept;

    [[nodiscard]] const std::vector<BuildDiagnostic>& all() const noexcept { return m_items; }
    [[nodiscard]] const DiagnosticCounts& counts() const noexcept { return m_counts; }
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }

    // One group per document (by documentKey) and per tool origin, in order of first arrival,
    // each with its diagnostics in arrival order. O(diagnostics).
    [[nodiscard]] std::vector<DiagnosticFileGroup> byFile() const;

    // Every diagnostic whose path names the document at `path` (absolute), jumpable or not, in
    // arrival order; empty for unknown or relative paths. O(result).
    [[nodiscard]] std::vector<BuildDiagnostic> forDocument(std::string_view path) const;

private:
    std::vector<BuildDiagnostic> m_items;
    DiagnosticCounts m_counts;
    std::unordered_map<std::string, std::vector<std::size_t>> m_byDocument; // documentKey -> indices.
};

// What MSBuild prints at minimal verbosity when a project produced an output:
// "  Lib.vcxproj -> C:\out\Lib.lib".
struct ProjectOutputLine {
    std::string projectFile{}; // "Lib.vcxproj"
    std::string output{};      // "C:\out\Lib.lib"

    friend bool operator==(const ProjectOutputLine&, const ProjectOutputLine&) = default;
};

// Parses such a line (leading whitespace and a "12>" node prefix are skipped; the left side must
// be a file name ending in "proj", without a path or ':'); nullopt for every other line. Pure.
[[nodiscard]] std::optional<ProjectOutputLine> parseProjectOutputLine(std::string_view line);

} // namespace frost::build
