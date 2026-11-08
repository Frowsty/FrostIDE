// SPDX-License-Identifier: Apache-2.0
//
// ProjectPropertiesDialog and Solution Management dialogs: Project Properties (.vcxproj C++ and
// .csproj .NET), Solution Properties, Configuration Manager, Run/Debug Configurations, Project
// References, NuGet, ASM/IL, Unit Tests and Local History. Views without a real tool behind them
// are read-only or disabled and name their owning epic (ADR 0078 truth rule, FND-3).

#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/storage/local_history.hpp"

#include <QString>
#include <QWidget>

#include <cstddef>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

// Key format in VcxprojPropertyMap:
//   - PropertyGroup element:          "Prop.<ElementName>" (e.g. "Prop.OutDir", "Prop.ConfigurationType")
//   - ItemDefinitionGroup metadata:   "<ItemGroup>.<ElementName>" (e.g. "ClCompile.PreprocessorDefinitions",
//                                     "Link.AdditionalDependencies", "Link.SubSystem", "Lib.OutputFile",
//                                     "PreBuildEvent.Command", "PostBuildEvent.Command")
using ProjectPropertyMap = std::map<std::string, std::string, std::less<>>;

// Reads the effective property and item-definition values for `configPlatform` (e.g. "Debug|x64")
// from `.vcxproj` XML content. Unconditioned groups are folded first, followed by groups matching
// `configPlatform`.
[[nodiscard]] ProjectPropertyMap parseVcxprojPropertiesForConfig(std::string_view xmlContent,
                                                                 std::string_view configPlatform);

// Applies modified keys from `updates` into `.vcxproj` XML content for `configPlatform` (or all
// existing configurations in the file when `configPlatform` is empty / "All Configurations").
[[nodiscard]] std::string applyVcxprojPropertiesForConfig(std::string_view xmlContent, std::string_view configPlatform,
                                                          const ProjectPropertyMap& updates);

// Reads `<PropertyGroup>` properties from `.csproj` XML content (`Prop.<ElementName>`).
[[nodiscard]] ProjectPropertyMap parseCsprojProperties(std::string_view xmlContent);

// Applies modified `Prop.<ElementName>` properties into `.csproj` XML content.
[[nodiscard]] std::string applyCsprojProperties(std::string_view xmlContent, const ProjectPropertyMap& updates);

// Parses `<PackageReference Include="..." Version="..." />` items (or `Update=` items, or a
// `<Version>` child element) from `.csproj` XML content, in document order. `version` is empty
// when the project file does not set one (for example under central package management). Pure;
// malformed XML yields the items read before the error. FrostIDE has no NuGet client yet (LTR-7),
// so these entries are only ever shown read-only.
struct NuGetPackageEntry {
    std::string id;
    std::string version;
};
[[nodiscard]] std::vector<NuGetPackageEntry> parseCsprojNuGetPackages(std::string_view xmlContent);

/*
 * Solution and project-membership edits. Until SLN-6 ("Safe solution and project edits") every
 * function below refuses: it reads and writes nothing and returns ErrorCode::Unsupported with a
 * message that names the operation and SLN-6, for the caller to show as a notification. The
 * single-source add/remove that ProjectHost supports goes through Workbench::removeProjectFile.
 * Thread safety: none needed (no state, no I/O).
 */
[[nodiscard]] Result<void> addExistingProjectToSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                            std::string_view projectPath);
[[nodiscard]] Result<void> removeProjectFromSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                         std::string_view projectPath);

[[nodiscard]] Result<std::string> addSolutionFolderToSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                                  std::string_view parentFolderId,
                                                                  std::string_view folderName);
[[nodiscard]] Result<void> renameSolutionFolderInSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                              std::string_view folderId, std::string_view newName);
[[nodiscard]] Result<void> removeSolutionFolderFromSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                                std::string_view folderId);
[[nodiscard]] Result<void> moveProjectToSolutionFolderInSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                                     std::string_view projectPath,
                                                                     std::string_view targetFolderId);

[[nodiscard]] Result<void> addFilterToVcxprojFilters(io::IFileSystem& fs, std::string_view vcxprojPath,
                                                     std::string_view filterPath);
[[nodiscard]] Result<void> renameFilterInVcxprojFilters(io::IFileSystem& fs, std::string_view vcxprojPath,
                                                        std::string_view oldFilterPath, std::string_view newFilterPath);
[[nodiscard]] Result<void> removeFilterFromVcxprojFilters(io::IFileSystem& fs, std::string_view vcxprojPath,
                                                          std::string_view filterPath);
[[nodiscard]] Result<void> moveFilesToVcxprojFilter(io::IFileSystem& fs, std::string_view vcxprojPath,
                                                    std::span<const std::string> filePaths,
                                                    std::string_view targetFilterPath);
[[nodiscard]] Result<std::size_t> addExistingFilesToVcxproj(io::IFileSystem& fs, std::string_view vcxprojPath,
                                                            std::span<const std::string> paths,
                                                            std::string_view targetFilterPath = {});
[[nodiscard]] Result<std::size_t> removeFilesFromVcxproj(io::IFileSystem& fs, std::string_view vcxprojPath,
                                                         std::span<const std::string> filePaths);

// Modal dialogs with custom frameless Rider chrome (`ui::installDialogChrome`).
void showProjectPropertiesDialog(QWidget* parent, io::IFileSystem& fs, const std::string& projectPath,
                                 const std::string& activeConfigPlatform, std::function<void()> onSaved);

// Startup project (applied through `onSetStartup` on OK) and a read-only view of each project's
// dependencies as loaded in `solution`. Editing project references is refused until SLN-6, so the
// dialog never writes a project file; `fs` and `onDependenciesChanged` are unused until then.
void showSolutionPropertiesDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                  std::optional<project::ProjectId> currentStartup,
                                  std::function<void(project::ProjectId)> onSetStartup,
                                  std::function<void()> onDependenciesChanged);

// Read-only Configuration Manager: the solution's project configuration mappings exactly as
// loaded ("not mapped" where the solution has none), with a note that editing arrives with SLN-6.
// The only action is choosing the active solution configuration, applied through
// `onActiveConfigurationChanged`; nothing is written, so `fs` and `onSolutionFileModified` are
// unused until SLN-6/SLN-7.
void showConfigurationManagerDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                    int activeConfigurationIndex, std::function<void(int)> onActiveConfigurationChanged,
                                    std::function<void()> onSolutionFileModified);

struct RunningProcessInfo {
    std::uint32_t pid = 0;
    std::string name;
    std::string executablePath;
    std::string windowTitle;
    bool isDotNet = false;
};

// The processes Attach to Process can offer, excluding this one, sorted with windowed processes
// first, then by name. Windows: a Toolhelp snapshot plus visible top-level window titles. Linux:
// /proc through readProcProcesses (no window titles). Never fails: an unreadable source yields
// fewer entries. GUI thread; O(processes).
[[nodiscard]] std::vector<RunningProcessInfo> enumerateRunningProcesses();

/*
 * The Linux process walk behind enumerateRunningProcesses, over any /proc-like `procRoot` so tests
 * can use a synthetic tree (it is plain file reading and builds on every platform). For each
 * numeric directory except 0 and `selfPid`: `readLink(<dir>/exe)` gives the executable ("" when
 * unreadable, as for other users' processes), with a trailing " (deleted)" removed; entries with
 * no executable and an empty `cmdline` (kernel threads, exited processes, zombies) are skipped.
 * The name is the executable's file name; without one, `comm`, or argv[0]'s file name when comm
 * is empty or is argv[0]'s first 15 bytes (the kernel's comm limit). isDotNet follows the dotnet
 * host's name or an apphost's <file name without .exe>.runtimeconfig.json beside it. Unsorted
 * (directory order). Never fails; O(processes).
 */
using ReadProcLink = std::function<std::string(const std::string& linkPath)>;
[[nodiscard]] std::vector<RunningProcessInfo> readProcProcesses(const std::string& procRoot, std::uint32_t selfPid,
                                                                const ReadProcLink& readLink);

void showAttachToProcessDialog(QWidget* parent,
                               std::function<void(const RunningProcessInfo& proc, bool attachAsDotNet)> onAttach);

// Read-only list of the projects `projectPath` references as loaded in `solution`. Adding or
// removing references requires SLN-6, so nothing is written and `fs` and `onSaved` are unused.
void showAddProjectReferenceDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                   const std::string& projectPath, std::function<void()> onSaved);

// Modal wrapper around createNuGetToolWindowWidget for one project; `onSaved` is unused (read-only).
void showNuGetPackageManagerDialog(QWidget* parent, io::IFileSystem& fs, const std::string& csprojPath,
                                   std::function<void()> onSaved);

/*
 * NuGet tool window, read-only: the PackageReference items parsed from the selected C# project
 * file (parseCsprojNuGetPackages), with a note that search, install and update require the NuGet
 * client (LTR-7). It shows no package that is not in the project file and writes nothing; the
 * file is read on the GUI thread when the project or filter changes. `fs` must outlive the
 * widget; `onSaved` is unused until LTR-7.
 */
[[nodiscard]] QWidget* createNuGetToolWindowWidget(QWidget* parent, io::IFileSystem& fs,
                                                   std::function<const project::SolutionModel*()> solutionProvider,
                                                   std::string initialCsprojPath, std::function<void()> onSaved);

/*
 * ASM / IL tool window, disabled: it shows why the view is unavailable and starts no process.
 * Disassembly needs the file's real compile command from the compile database (CPP-3) run through
 * IProcessLauncher; IL needs the .NET toolchain (LTR-3). `activeFileAndLineProvider` is only used
 * to name the active file in the message.
 */
[[nodiscard]] QWidget*
createAsmViewerToolWindowWidget(QWidget* parent,
                                std::function<std::pair<std::string, int>()> activeFileAndLineProvider);

// A test or entry point found by scanning source text (not by running a test binary). It says
// where a test is declared, never how it ran.
struct DiscoveredTestEntry {
    std::string solutionName;
    std::string projectName;
    std::string projectPath;
    std::string suiteName;
    std::string testName;
    std::string filePath;
    int line = 1; // 1-based source line
    bool isDotNet = false;
    bool isEntryPoint = false;
};

[[nodiscard]] std::vector<DiscoveredTestEntry> discoverTestsAndEntryPointsInFile(std::string_view filePath,
                                                                                 std::string_view content,
                                                                                 std::string_view projectName = {},
                                                                                 std::string_view projectPath = {},
                                                                                 std::string_view solutionName = {});

[[nodiscard]] std::vector<DiscoveredTestEntry>
discoverWorkspaceUnitTests(io::IFileSystem& fs, const project::SolutionModel* solution, std::string_view workspaceRoot);

/*
 * Unit Tests tool window, discovery only: the tree lists the tests discoverWorkspaceUnitTests
 * finds in source text, each marked "Discovered, not run", and double-click navigates to the
 * declaration. Run All, Run Selected, Run Failed, Repeat Until Failure, Debug Selected and Stop
 * are disabled with "Requires the test runner (RUN-7)": the window starts no process and shows no
 * outcome, duration or count of passed/failed tests. `onDebugTest` is unused until RUN-10. `fs`
 * must outlive the widget; discovery runs on the GUI thread on Refresh and on filter changes.
 */
[[nodiscard]] QWidget*
createUnitTestsToolWindowWidget(QWidget* parent, io::IFileSystem& fs,
                                std::function<const project::SolutionModel*()> solutionProvider,
                                std::function<std::string()> workspaceRootProvider,
                                std::function<void(const std::string& path, int line)> onNavigateToSource,
                                std::function<void(const DiscoveredTestEntry& test)> onDebugTest = {});

void showLocalHistoryDialog(QWidget* parent, storage::LocalHistory& history, const std::string& filePath,
                            const std::string& currentContent,
                            std::function<void(const std::string& restoredContent)> onRestore);

} // namespace frost::app
