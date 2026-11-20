// SPDX-License-Identifier: Apache-2.0
// Standalone CLI diff/merge windows, sharing R15's editor models without opening a workspace.
#pragma once
#include "frost/app/command_line.hpp"
#include "frost/core/error.hpp"
namespace frost::app {
// GUI thread; blocks until the viewer closes. 1 means merge cancelled, 0 means success.
[[nodiscard]] Result<int> runCliEditor(const CommandLine& command);
}
