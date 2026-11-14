// SPDX-License-Identifier: Apache-2.0
// Standalone lua-debug stdio DAP; never an engine endpoint or a bundled-runtime fallback.
#pragma once
#include "frost/debug/debug_session.hpp"
#include "frost/io/process.hpp"

#include <string>
#include <utility>
#include <vector>
namespace frost::debug {
struct StandaloneLuaTarget {
    std::string interpreter, script, workingDirectory;
    std::vector<std::string> interpreterArguments, arguments;
    std::vector<std::pair<std::string, std::string>> environment;
    bool stopOnEntry = true;
};
// Upstream lua-debug schema: luaexe is always explicit; arg0 precedes program; arg follows it.
// Only a script launch is supported. REPL remains the actual interpreter -i Run configuration.
[[nodiscard]] Result<rpc::Json> standaloneLuaLaunchArguments(const StandaloneLuaTarget& target);
// Installed upstream bin/lua-debug[.exe], NO arguments (descriptorFactory.js), owned pipes/tree.
// Keep its extension/script/native runtime dependencies installed; no silent alternate executable.
[[nodiscard]] AdapterFactory standaloneLuaAdapterFactory(io::IProcessLauncher& launcher, std::string executable);
} // namespace frost::debug
