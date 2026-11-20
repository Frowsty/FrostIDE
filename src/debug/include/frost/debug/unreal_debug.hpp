// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/debug/lldb_dap.hpp"
namespace frost::debug {
// Pure native launch adapter, for DebugController::startTarget. Project trust is mandatory;
// native backends perform validateDebugTargetTrust before starting. No remote transport implied.
[[nodiscard]] Result<DebugTarget> unrealEditorDebugTarget(std::string editor, std::string projectFile,
    std::vector<std::string> arguments = {});
} // namespace frost::debug
