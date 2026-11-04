// SPDX-License-Identifier: Apache-2.0
//
// ProjectPropertiesDialog and Solution Management dialogs: Visual Studio / JetBrains Rider-style
// Project Properties (.vcxproj C++ & .csproj .NET), Solution Properties, Solution Configuration
// Manager, Run/Debug Configurations, Project References, NuGet Package Manager, and Local History.

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

// Parses `<PackageReference Include="..." Version="..." />` items from `.csproj` XML content.
struct NuGetPackageEntry {
    std::string id;
    std::string version;
};
[[nodiscard]] std::vector<NuGetPackageEntry> parseCsprojNuGetPackages(std::string_view xmlContent);
[[nodiscard]] std::string upsertCsprojNuGetPackage(std::string_view xmlContent, std::string_view packageId,
                                                   std::string_view version);
[[nodiscard]] std::string removeCsprojNuGetPackage(std::string_view xmlContent, std::string_view packageId);

// Adds or removes a `<ProjectReference Include="..." />` in `.vcxproj` or `.csproj` XML content.
[[nodiscard]] std::string addProjectReferenceXml(std::string_view xmlContent, std::string_view relativeRefPath,
                                                 std::string_view referencedGuid = {});
[[nodiscard]] std::string removeProjectReferenceXml(std::string_view xmlContent, std::string_view relativeRefPath);

// Adds or removes a project entry in a `.sln` or `.slnx` solution file on disk.
[[nodiscard]] Result<void> addExistingProjectToSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                            std::string_view projectPath);
[[nodiscard]] Result<void> removeProjectFromSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                         std::string_view projectPath);

// C++ (.vcxproj / .vcxproj.filters) filter management and batch file/folder operations.
[[nodiscard]] Result<void> addFilterToVcxprojFilters(io::IFileSystem& fs, std::string_view vcxprojPath,
                                                     std::string_view filterPath);
[[nodiscard]] Result<void> renameFilterInVcxprojFilters(io::IFileSystem& fs, std::string_view vcxprojPath,
                                                        std::string_view oldFilterPath,
                                                        std::string_view newFilterPath);
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

void showSolutionPropertiesDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                  std::optional<project::ProjectId> currentStartup,
                                  std::function<void(project::ProjectId)> onSetStartup,
                                  std::function<void()> onDependenciesChanged);

void showConfigurationManagerDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                    int activeConfigurationIndex, std::function<void(int)> onActiveConfigurationChanged,
                                    std::function<void()> onSolutionFileModified);

void showEditRunConfigurationsDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel* solution,
                                     const std::string& preferredProjectPath, const std::string& activeConfigPlatform,
                                     bool externalConsole, std::function<void(bool)> onExternalConsoleChanged,
                                     std::function<void()> onSaved);

void showAddProjectReferenceDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                   const std::string& projectPath, std::function<void()> onSaved);

void showNuGetPackageManagerDialog(QWidget* parent, io::IFileSystem& fs, const std::string& csprojPath,
                                   std::function<void()> onSaved);

void showLocalHistoryDialog(QWidget* parent, storage::LocalHistory& history, const std::string& filePath,
                            const std::string& currentContent,
                            std::function<void(const std::string& restoredContent)> onRestore);

} // namespace frost::app
