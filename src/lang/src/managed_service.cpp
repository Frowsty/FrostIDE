// SPDX-License-Identifier: Apache-2.0
//
// Managed language service process configuration (ADR 0064).
#include "frost/lang/managed_service.hpp"

namespace frost::lang {

LuaServiceOptions managedServiceOptions(std::string dotnetPath, std::string serverAssembly, std::string workspaceRoot,
                                        bool trusted) {
    LuaServiceOptions options;
    options.serverPath = std::move(dotnetPath);
    options.arguments = {std::move(serverAssembly), "--loglevel", "warning"};
    options.workspaceRoot = std::move(workspaceRoot);
    options.trusted = trusted;
    options.languageId = "csharp";
    options.displayName = "C# language server";
    options.firstArgumentIsAbsolutePath = true;
    options.environment = {{"DOTNET_CLI_TELEMETRY_OPTOUT", std::string("1")},
                           {"DOTNET_NOLOGO", std::string("1")},
                           {"DOTNET_SKIP_FIRST_TIME_EXPERIENCE", std::string("1")},
                           {"DOTNET_GENERATE_ASPNET_CERTIFICATE", std::string("0")},
                           {"DOTNET_ADD_GLOBAL_TOOLS_TO_PATH", std::string("0")}};
    return options;
}

} // namespace frost::lang
