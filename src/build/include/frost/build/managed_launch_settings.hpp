// SPDX-License-Identifier: Apache-2.0
// launchSettings Project profiles, preserving explicit unsupported command kinds.
#pragma once
#include "frost/core/error.hpp"

#include <map>
#include <string>
#include <vector>
namespace frost::build {
struct ManagedLaunchProfile {
    std::string name, commandName, workingDirectory, applicationUrl, launchUrl, unavailable;
    std::vector<std::string> arguments;
    std::map<std::string, std::string, std::less<>> environment;
    bool launchBrowser = false;
};
// Pure, bounded1MiB/256 profiles. Project profiles use direct evaluated DLL launch. No shell,
// environment expansion, IIS Express/Docker/WSL or native browser debugger is inferred.
// Malformed/duplicate fields refuse; unsupported profile kinds remain visible with a reason.
[[nodiscard]] Result<std::vector<ManagedLaunchProfile>> parseManagedLaunchSettings(std::string_view json);
} // namespace frost::build
