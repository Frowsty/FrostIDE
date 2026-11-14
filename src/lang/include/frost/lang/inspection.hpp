// SPDX-License-Identifier: Apache-2.0
//
// C++ inspection framework (PLAN.md section 6.4, epic CPP-10): inspection profiles, rule severities,
// scopes (File, Directory, Target, Project), real clang-tidy discovery and execution through
// frost::io::IProcessLauncher, and SARIF 2.1.0 export with schema validation.

#ifndef FROST_LANG_INSPECTION_HPP
#define FROST_LANG_INSPECTION_HPP

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/sched/resource_governor.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

using ClangdSearchOptions = ClangdSearch;
using CompileCommandEntry = CompileCommand;

// --- Locating clang-tidy -------------------------------------------------------------------------

struct ClangTidyLocation {
    std::string path;
    ClangdOrigin origin = ClangdOrigin::Path;
    std::string version;

    bool operator==(const ClangTidyLocation&) const = default;
};

// Queries `<clangTidyPath> --version` via `launcher` and extracts the version string.
[[nodiscard]] Result<std::string> queryClangTidyVersion(io::IProcessLauncher& launcher, std::string_view clangTidyPath);

// Locates the real clang-tidy binary (`clang-tidy.exe` on Windows, `clang-tidy` on POSIX) using
// the same search order as `findClangd` (explicit setting, bundled/pinned SDK, PATH). Without a
// launcher, version is empty; it is never inferred from the pinned clangd version.
[[nodiscard]] Result<ClangTidyLocation> findClangTidy(io::IFileSystem& fs,
                                                      const ClangdSearchOptions& options = defaultClangdSearch());

[[nodiscard]] Result<ClangTidyLocation> findClangTidy(const io::IFileSystem& fs,
                                                      const ClangdSearchOptions& options = defaultClangdSearch());

// Locates clang-tidy and requires a successful real `--version` process query.
[[nodiscard]] Result<ClangTidyLocation> findClangTidy(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                      const ClangdSearchOptions& options = defaultClangdSearch());

// --- Profiles, Severities, and Scopes ------------------------------------------------------------

enum class InspectionSeverity : std::uint8_t {
    Error,
    Warning,
    WeakWarning,
    Info,
    Disabled,
};

[[nodiscard]] std::string_view toString(InspectionSeverity severity) noexcept;
[[nodiscard]] std::string_view toSarifLevel(InspectionSeverity severity) noexcept;
[[nodiscard]] InspectionSeverity fromDiagnosticSeverity(DiagnosticSeverity severity) noexcept;

enum class InspectionScopeKind : std::uint8_t {
    File,
    Directory,
    Target,
    Project,
};

struct InspectionScope {
    InspectionScopeKind kind = InspectionScopeKind::File;
    std::vector<std::string> paths;
    std::string targetName;

    bool operator==(const InspectionScope&) const = default;
};

struct InspectionRuleConfig {
    std::string checkPattern;
    InspectionSeverity severity = InspectionSeverity::Warning;
    bool enabled = true;

    bool operator==(const InspectionRuleConfig&) const = default;
};

struct InspectionProfile {
    std::string name = "Default";
    std::vector<InspectionRuleConfig> rules;
    bool respectProjectClangTidy = true;

    [[nodiscard]] std::string toChecksArgument() const;
    [[nodiscard]] InspectionSeverity
    effectiveSeverity(std::string_view ruleId,
                      DiagnosticSeverity toolSeverity = DiagnosticSeverity::Warning) const noexcept;

    bool operator==(const InspectionProfile&) const = default;
};

// Loads a compilation database from either a `compile_commands.json` file path or its directory.
[[nodiscard]] Result<std::vector<CompileCommandEntry>> loadCompileCommands(io::IFileSystem& fs,
                                                                           std::string_view compileCommandsPathOrDir);

// Resolves which translation units from `compile_commands.json` belong to `scope`.
[[nodiscard]] std::vector<std::string> resolveInspectionScopeFiles(std::span<const CompileCommandEntry> commands,
                                                                   const InspectionScope& scope);

[[nodiscard]] Result<std::vector<std::string>> resolveInspectionScopeFiles(io::IFileSystem& fs,
                                                                           std::string_view compileCommandsPathOrDir,
                                                                           const InspectionScope& scope);

// --- Real clang-tidy Runner ----------------------------------------------------------------------

struct InspectionFinding {
    std::string file;
    std::uint32_t line = 1;      // 1-based
    std::uint32_t column = 1;    // 1-based
    std::uint32_t endLine = 1;   // 1-based
    std::uint32_t endColumn = 1; // 1-based
    std::string ruleId;
    std::string message;
    InspectionSeverity severity = InspectionSeverity::Warning;
    DiagnosticSeverity toolSeverity = DiagnosticSeverity::Warning;
    std::optional<TextEdit> fixIt;

    bool operator==(const InspectionFinding&) const = default;
};

struct InspectionRequest {
    std::string clangTidyPath;
    std::string compileCommandsDir;
    InspectionScope scope;
    InspectionProfile profile;
    sched::ResourceGovernor* governor = nullptr;
    std::vector<std::string> extraArgs;
    std::chrono::milliseconds timeout{120'000};
    std::stop_token stopToken{};
    // Optional fresh caller-owned output path. The tool exports replacements only, never -fix.
    // Caller captures source images before execution and decodes the actual YAML separately.
    std::string exportFixesPath;
};

struct InspectionReport {
    std::string profileName;
    std::string toolPath;
    std::string toolVersion;
    std::string commandLine;
    std::vector<std::string> arguments;
    std::vector<std::string> inspectedFiles;
    std::vector<InspectionFinding> findings;
    int exitCode = 0;
    bool executionSuccessful = true;
    std::string rawStdout;
    std::string rawStderr;

    bool operator==(const InspectionReport&) const = default;
};

// Parses clang-tidy diagnostic output (`<file>:<line>:<col>: <severity>: <message> [<rule-id>]`)
// and applies `profile` filtering and severity mapping.
[[nodiscard]] std::vector<InspectionFinding> parseClangTidyOutput(std::string_view output,
                                                                  const InspectionProfile& profile);

// Runs the real clang-tidy binary via `launcher` and returns the structured inspection report.
[[nodiscard]] Result<InspectionReport> runClangTidyInspection(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                              const InspectionRequest& request);

// --- SARIF 2.1.0 Export and Schema Validation ----------------------------------------------------

// Exports `report` as a standards-compliant SARIF 2.1.0 JSON document.
[[nodiscard]] Json exportSarifJson(const InspectionReport& report);

// Checks this exporter's structural invariants. Acceptance also runs the official OASIS schema
// through tools/validate_sarif.py; this helper is not a general JSON Schema validator.
[[nodiscard]] Result<void> validateSarifSchema(const Json& sarif);

// Checks the exporter's invariants and atomically writes the SARIF report to `outputPath` on `fs`.
[[nodiscard]] Result<void> writeSarifReport(io::IFileSystem& fs, std::string_view outputPath,
                                            const InspectionReport& report);

} // namespace frost::lang

#endif // FROST_LANG_INSPECTION_HPP
