// SPDX-License-Identifier: Apache-2.0
//
// SLN-7: edits of a solution's project configuration mappings (what each project builds as in
// each solution configuration), the writes behind the Configuration Manager. .sln and .slnx.
/*
 * The edits change only what they must, so `git diff` shows the intended lines alone:
 *
 * .sln (applySlnConfigurationEdits). In GlobalSection(ProjectConfigurationPlatforms), the
 * "{GUID}.<solution configuration>.ActiveCfg = <configuration>|<platform>" line gets the new
 * value; "Build.0" and "Deploy.0" lines are added right after it (with its indentation) or
 * removed to match `build` and `deploy`, and given the same value when kept. A project that has no
 * line for that solution configuration gets its lines after its last line in the section; a
 * mapping of nullopt removes the project from the solution configuration (all three lines). Line
 * ends follow the file's first line end; a UTF-8 BOM is kept. UTF-16 files are refused.
 *
 * .slnx (applySlnxConfigurationEdits). Each <Project> carries rules (<BuildType>, <Platform>,
 * <Build>, <Deploy> with Solution="pattern" Project="value"); later rules win. For each property
 * whose current mapping (from `model`) differs from the edit, the last rule of that kind whose
 * Solution names exactly the edited configuration gets the new value in place when no later rule
 * of that kind also matches it; otherwise a rule with Solution="<configuration>|<platform>" is
 * appended as the project's last child (a self-closing <Project/> is opened for it). Removing a
 * project from a configuration has no .slnx form and is refused (Unsupported).
 *
 * writeSolutionConfigurationEdits applies them to the file on disk atomically, then reloads the
 * solution and checks that every edited mapping reads back exactly; when one does not, the
 * original bytes are written back and the call fails (Corrupt), so a write never leaves a mapping
 * other than the one asked for.
 *
 * Errors: NotFound (an unknown project or solution configuration), Unsupported (UTF-16 .sln,
 * removal in .slnx), Corrupt (no ProjectConfigurationPlatforms section / unparsable .slnx, or the
 * read-back check), InvalidArgument (empty configuration or platform), and the file system's.
 * Pure apart from writeSolutionConfigurationEdits; thread-safe.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/project/solution_model.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

// What one project builds as in one solution configuration after the edit.
struct ConfigurationMappingEdit {
    ProjectId project;                                  // In the model the edit is made against.
    std::size_t solutionConfiguration = 0;              // Index into model.configurations().
    std::optional<ProjectConfigurationMapping> mapping; // nullopt: not part of it (.sln only).

    friend bool operator==(const ConfigurationMappingEdit&, const ConfigurationMappingEdit&) = default;
};

// The edited .sln text. `model` is the loaded solution the edits name projects of.
[[nodiscard]] Result<std::string> applySlnConfigurationEdits(std::string_view content, const SolutionModel& model,
                                                             std::span<const ConfigurationMappingEdit> edits);

// The edited .slnx text.
[[nodiscard]] Result<std::string> applySlnxConfigurationEdits(std::string_view content, const SolutionModel& model,
                                                              std::span<const ConfigurationMappingEdit> edits);

// Reads model.path(), applies the edits by format, writes atomically and verifies (see above).
[[nodiscard]] Result<void> writeSolutionConfigurationEdits(io::IFileSystem& fs, const SolutionModel& model,
                                                           std::span<const ConfigurationMappingEdit> edits);

} // namespace frost::project
