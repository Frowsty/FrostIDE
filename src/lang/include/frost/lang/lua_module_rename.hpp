// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/io/file_system.hpp"
#include "frost/lang/lua_refactoring.hpp"

#include <optional>
namespace frost::lang {
struct LuaModuleSource {
    std::string path;
    text::Snapshot snapshot;
};
struct LuaModuleProbe {
    std::string path;
    std::optional<std::string> bytes;
    std::string physicalPath;
};
struct LuaModuleFileEdits {
    std::string path;
    text::Snapshot original;
    std::vector<LuaSourceEdit> edits;
};
struct LuaModuleRenameRequest {
    std::string workspaceRoot, fromPath, toPath;
    std::vector<std::string> runtimePaths; // Actual selected runtime.path, relative to workspace root.
    std::vector<LuaModuleSource> sources;  // Complete bounded caller-captured workspace, dirty overlays included.
};
struct LuaModuleRenameProposal {
    std::vector<LuaModuleFileEdits> files;
    std::vector<LuaModuleProbe> probes; // All positive and negative module lookup observations.
};
// Read-only actual AST require-call classification + runtime-path/physical-file resolution.
// Refuses dynamic requires, require aliases/shadowing, environment/package changes, ambiguous
// resolution, nonliteral paths and source+move overlap. Caller retains complete directory/source
// inventory/config/provider/trust receipts, adds the file move and applies one REF1 transaction.
[[nodiscard]] Result<LuaModuleRenameProposal> planLuaModuleRename(io::IFileSystem&, const LuaModuleRenameRequest&,
                                                                  std::stop_token = {});
} // namespace frost::lang
