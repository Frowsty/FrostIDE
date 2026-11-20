// SPDX-License-Identifier: Apache-2.0
// Native current-user-only C2 discovery/config files, shared by the GUI and stdio proxy.
#pragma once
#include "frost/core/error.hpp"

#include <string>
#include <string_view>
namespace frost::app {
[[nodiscard]] Result<void> ensureAgentPrivateDirectory(std::string_view path);
// Create-new only. No credential files are read or modified. Both APIs cap bytes at 64 KiB.
[[nodiscard]] Result<void> writeAgentPrivateFile(std::string_view path, std::string_view bytes);
[[nodiscard]] Result<std::string> readAgentPrivateFile(std::string_view path);
} // namespace frost::app
