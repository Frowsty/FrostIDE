// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/lang/inspection.hpp"
#include <map>
namespace frost::lang {
struct InspectionInputImage {
    std::string path, diskBytes, contents, physicalPath;
    bool buffer = false;
    bool editable = true;
};
struct InspectionScopeCapture {
    std::string root, physicalRoot, databasePath;
    std::vector<InspectionInputImage> images;
    std::map<std::string, std::vector<std::string>> directories;
    std::vector<std::string> translationUnits;
    std::vector<std::string> absentInputs;
};
struct InspectionFixFile { std::string path, before, after; bool buffer = false; };
struct InspectionFixGroup { std::string rule, message, refusal; std::size_t replacements = 0; };
struct InspectionScopeFixProposal {
    std::vector<InspectionFixFile> files;
    std::vector<InspectionFixGroup> groups;
    std::size_t withoutFix = 0;
    [[nodiscard]] bool ready() const;
};
// Capture is completed BEFORE any tool request. Actual project source/header/config images and
// open-buffer contents are retained. Metadata/output trees are excluded from the source walk;
// explicit project-local compiler include inputs are captured read-only, including generated files.
// Relative compiler working directories and unexpanded response/PCH/module inputs refuse.
// Linked trees, unreadable inputs, budget overflow and missing translation units explicitly refuse.
[[nodiscard]] Result<InspectionScopeCapture> captureInspectionScope(io::IFileSystem& fs,
    const InspectionRequest& request, std::string projectRoot,
    std::span<const std::pair<std::string, std::string>> openBuffers);
[[nodiscard]] Result<void> validateInspectionCapture(io::IFileSystem& fs, const InspectionScopeCapture& capture);
// Decodes ONLY the frost-clang LLVM YAML DTO. Notes with replacements are alternative fix groups;
// ambiguous alternatives, uncaptured targets and overlaps block the complete proposal, not a subset.
[[nodiscard]] Result<InspectionScopeFixProposal> planInspectionScopeFixes(const Json& decoded,
    const InspectionScopeCapture& capture, const InspectionProfile& profile);
// Caller supplies an existing private, unique scratch directory outside the project. No -fix is
// ever sent. Captured inputs are VFS-mapped, original compilation database is preserved, and actual
// clang-tidy -export-fixes YAML is decoded by the pinned helper. Processes are joined before return.
[[nodiscard]] Result<InspectionScopeFixProposal> runInspectionScopeFixes(io::IProcessLauncher& launcher,
    io::IFileSystem& fs, InspectionRequest request, const InspectionScopeCapture& capture,
    std::string helper, std::string scratchDirectory);
}
