// SPDX-License-Identifier: Apache-2.0
// C1 resource admission uses physical workspace identity before any editor/file operation.
#pragma once
#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
namespace frost::app {
// GUI thread; returns an existing regular physical file within root. Never admits credentials/keychains.
// Resolves junctions/symlinks on both sides; errors propagate without probing file contents.
[[nodiscard]] Result<std::string> agentWorkspaceResource(io::IFileSystem& fs, std::string_view root, std::string_view resource);
}
