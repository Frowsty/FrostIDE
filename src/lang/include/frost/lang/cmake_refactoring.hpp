// SPDX-License-Identifier: Apache-2.0
// CMK-9 strict configured target rename plan and source inspections; no CMake execution.
#pragma once
#include "frost/lang/cmake_language.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/lang/diagnostics.hpp"
namespace frost::lang {
struct CMakeRenameCapture {
    std::shared_ptr<const project::CMakeProject> model;
    WorkspaceEdit edit;
    std::vector<std::pair<std::string,std::string>> inputs;
    std::vector<std::pair<std::string,std::string>> provenance;
};
// Full File API source input closure must be captured. Unsupported dynamic/indirect target
// spelling refuses the whole plan. No source build semantics or target counts are invented.
[[nodiscard]] Result<CMakeRenameCapture> planCMakeTargetRename(const CMakeLanguageInput& input,
                                                              LspPosition position,std::string newName);
// Syntax facts only; configured-target diagnostics explicitly refer to last configured graph.
[[nodiscard]] std::vector<LiveDiagnostic> inspectCMakeSource(std::string_view path,const text::Snapshot& source);
} // namespace frost::lang
