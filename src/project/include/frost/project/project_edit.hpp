// SPDX-License-Identifier: Apache-2.0
//
// Safe solution and project edits (SLN-6, ADR 0082): the requests FrostIDE sends the ProjectHost
// helper to add, remove and rename files of a .vcxproj and its .vcxproj.filters (through the MSBuild
// construction API) and to add or remove a project of a .sln or .slnx (through
// Microsoft.VisualStudio.SolutionPersistence), the answer lines it reads back, the edits a rename,
// move or delete on disk implies for the open solution, and a synchronous runner. FrostIDE never
// edits these files as text.
/*
 * The edit protocol (tools/project-host-spike/VsProbe.cs, ProjectFileEdits.cs, SolutionEdits.cs).
 * One helper process per edit, started through IProcessLauncher with the absolute directory of the
 * Visual Studio instance's MSBuild.exe as its only argument:
 *
 *   -> projectEditRequest(edit)                        one JSON line of strings
 *   <- {"Ready":true,"TransactionId":id,"JournalDigest":<64 hex>}
 *        the helper has validated the edit with MSBuild's evaluation (file edits) or by comparing
 *        SolutionPersistence's result with the original (solution edits), planned the new bytes
 *        and made its recovery journal (<root>/.frost-project-edit-journal) durable
 *   -> projectEditDecision(id, accept)                 nothing is written unless accept is true
 *   <- {"Error":null} or {"Error":"<reason>"}         the definitions were written, or why not
 *
 * A refusal before Ready answers {"Error":...} at once and writes nothing. Paths in a request are
 * absolute; lists are '|'-separated (Windows forbids '|' in names). The app's ProjectEditProcess
 * speaks the same protocol asynchronously on the GUI thread; runProjectEdit below is the blocking
 * form for tests and tools.
 *
 * Thread safety: every function is pure or uses only its arguments; runProjectEdit blocks its
 * caller and may run on any thread.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace frost::project {

class SolutionModel;

enum class ProjectEditKind : unsigned char {
    AddFiles,      // Adds files to a .vcxproj (and, when it exists, its .vcxproj.filters).
    RemoveFiles,   // Removes file items from both.
    RenameFiles,   // Changes the Include of file items in both (the files were renamed on disk).
    AddProject,    // Adds a .vcxproj to a .sln or .slnx, at the solution root.
    RemoveProject, // Removes a project from a .sln or .slnx (and the other projects' dependencies on it).
    SetProperties, // Configuration-scoped property pages via MSBuild construction API.
};

// One edit. Paths are absolute.
struct ProjectEdit {
    ProjectEditKind kind = ProjectEditKind::AddFiles;
    std::string root;       // The trusted root: definitions must lie inside it; the journal is written there.
    std::string definition; // The .vcxproj (file edits) or the .sln/.slnx (project edits).
    // AddFiles/RemoveFiles: the files; RenameFiles: their old paths.
    std::vector<std::string> files = {};
    std::vector<std::string> renamedTo = {}; // RenameFiles: the new paths, pairwise with `files`.
    bool earlyProperties = false; // Property group before imports (toolchain/dependency import hooks).
    std::vector<std::pair<std::string, std::string>> propertyUpdates = {}; // Prop.Name or ItemType.Metadata.
    std::string filter = {};                 // AddFiles: a filter the .filters file defines; "" for none.
    std::string project = {};                // AddProject/RemoveProject: the project file.
    // File edits: the project configuration the solution's active configuration maps to; the
    // helper evaluates every configuration the project declares in any case.
    std::string configuration = {};
    std::string platform = {};
    std::string transactionId = {}; // A GUID, "8-4-4-4-12" hexadecimal; names the journal and its record.
    bool commit = true;             // False: plan and check only, nothing is written.
};

/*
 * The request line (no trailing newline). Errors: InvalidArgument for a relative path, an empty or
 * mismatched list, a path containing '|' or a control character, a definition of the wrong type for
 * the kind (.vcxproj for file edits, .sln/.slnx for project edits) or a transaction id that is not
 * a GUID.
 */
[[nodiscard]] Result<std::string> projectEditRequest(const ProjectEdit& edit);

// The decision line answering a Ready line (no trailing newline).
[[nodiscard]] std::string projectEditDecision(std::string_view transactionId, bool accept);

// One answer line of the helper.
struct ProjectEditAnswer {
    bool ready = false;               // A Ready line (the journal is durable; a decision is awaited).
    std::string transactionId;        // Ready: the transaction it names.
    std::string journalDigest;        // Ready: SHA-256 of the journal, 64 upper-case hex digits.
    std::optional<std::string> error; // The final line: nullopt on success, else the helper's reason.
};

// Parses one line. Errors: Corrupt (not a JSON object, a Ready line without a valid id or digest).
[[nodiscard]] Result<ProjectEditAnswer> parseProjectEditAnswer(std::string_view line);

// The file edits of one project that a rename, move or delete on disk implies.
struct ProjectPathChange {
    std::string project;                // The .vcxproj.
    std::vector<std::string> files;     // The items' current (old) paths, in document order.
    std::vector<std::string> renamedTo; // Their new paths; empty for a delete.
};

/*
 * For every loaded C++ project of `solution` that lists `oldPath` (or, for a directory, files under
 * it) as an item written without a wildcard, the RemoveFiles (`newPath` empty) or RenameFiles edit
 * that keeps the project in step with the disk. Items from wildcards need no edit and are skipped.
 * Paths are compared as the file system does (case-insensitive on Windows). Pure; O(items).
 */
[[nodiscard]] std::vector<ProjectPathChange> projectEditsForPathChange(const SolutionModel& solution,
                                                                       std::string_view oldPath,
                                                                       std::string_view newPath, bool directory);

// How runProjectEdit starts the helper.
struct ProjectEditLaunch {
    std::string hostPath;    // Absolute ProjectHostVsProbe.exe.
    std::string msbuildPath; // Absolute MSBuild.exe of the Visual Studio instance (its directory is passed).
    IClock::Duration timeout = std::chrono::minutes(3); // The whole edit; the helper is ended after it.
};

/*
 * Runs one edit to its end: starts the helper, sends the request, calls `admit(digest)` on Ready and
 * sends its decision, then waits for the final answer and the helper's exit. Blocking.
 * Errors: InvalidArgument (projectEditRequest's, paths not absolute), the launcher's, Cancelled
 * (`admit` returned false; nothing was written), IoError (the helper's reason, or it ended without
 * an answer), Corrupt (an unreadable answer), Timeout (the helper was ended).
 */
[[nodiscard]] Result<void> runProjectEdit(io::IProcessLauncher& launcher, const ProjectEditLaunch& launch,
                                          const ProjectEdit& edit,
                                          const std::function<bool(std::string_view digest)>& admit);

} // namespace frost::project
