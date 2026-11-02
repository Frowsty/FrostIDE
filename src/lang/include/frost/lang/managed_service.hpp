// SPDX-License-Identifier: Apache-2.0
//
// Trusted managed language server configuration for the supervised LSP document service.
#pragma once

#include "frost/lang/lua_service.hpp"

namespace frost::lang {

// The lifecycle, versioning and ownership contract is identical to the supervised Lua service.
using ManagedService = LuaService;

// Configure csharp-ls, a Roslyn-based LSP server launched through an absolute dotnet executable.
// The caller must validate the pinned server files and acquire workspace trust before start().
[[nodiscard]] LuaServiceOptions managedServiceOptions(std::string dotnetPath, std::string serverAssembly,
                                                      std::string workspaceRoot, bool trusted);

} // namespace frost::lang
