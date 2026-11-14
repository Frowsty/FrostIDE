// SPDX-License-Identifier: Apache-2.0
// Pure .vcxproj/.filters replacements for one transaction-owned physical source rename.
#pragma once

#include "frost/core/error.hpp"
#include "frost/project/project_edit.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frost::project {

struct VcxprojRenameInput {
    std::string path;
    std::optional<std::string> contents; // nullopt is captured absence, never an unreadable file.
};
struct VcxprojRenameReplacement {
    std::string path;
    std::string before;
    std::string after;
};
struct VcxprojRenameRequest {
    std::string projectPath;
    std::string fromPath;
    std::string toPath;
    std::span<const VcxprojRenameInput> inputs;
};
struct VcxprojRenamePlan {
    std::vector<VcxprojRenameReplacement> replacements;
    std::vector<VcxprojRenameInput> dependencies;
};

// No I/O, helper invocation, source-file move or document mutation. inputs must contain unique
// absolute project and projectPath+".filters" snapshots (including explicit absence), plus imports
// when needed. Caller captures dirty buffers over disk and revalidates project/evaluation/trust
// identity through transaction admission. The same selected rename must include the physical
// source move in RefactoringPlan; convert replacements to disk/buffer participants and retain all
// dependencies, including an absent filters file. XML serialization may normalize formatting;
// the exact original bytes remain the inverse, never reserialized during Undo.
// Only explicit literal file Includes and literal Link display names are supported. Wildcards,
// lists/properties/item expressions, build-time item mutation and ambiguous/import-owned source
// membership return actionable Unsupported; malformed XML is Corrupt, conflicts AlreadyExists.
[[nodiscard]] Result<VcxprojRenamePlan> planVcxprojRename(const VcxprojRenameRequest& request);

// Worker-only practical MSBuild route: edit must be RenameFiles, commit=false,
// beforeFileMove=true. The existing helper evaluates every declared configuration before/after
// an IN-MEMORY construction edit and publishes exact before/import bytes. Definitions, imports
// and sources are never temporarily written or moved. Dirty definition/import overlays that
// differ from evaluated bytes refuse; unsaved source buffers remain the caller's participants.
// Capture project and project+'.filters' (including absence) in inputs. The result joins the
// caller's single RefactoringPlan, never the helper's independent commit/journal path.
[[nodiscard]] Result<VcxprojRenamePlan>
planVcxprojRenameWithHost(io::IProcessLauncher& launcher, const ProjectEditLaunch& launch, const ProjectEdit& edit,
                          std::span<const VcxprojRenameInput> inputs, std::stop_token stop = {});

} // namespace frost::project
