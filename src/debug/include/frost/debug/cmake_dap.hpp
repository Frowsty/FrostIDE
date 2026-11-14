// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/debug/debug_session.hpp"
#include <chrono>
namespace frost::debug {
// CMake --debugger-pipe is a named pipe on Windows and an AF_UNIX socket on Linux.
// This connects only to an endpoint allocated by the configure owner, never TCP or stdio.
// The configure BuildSession owns the process; closing this transport invokes cancelConfigure.
[[nodiscard]] AdapterFactory cmakeDapFactory(std::string endpoint, std::function<void()> cancelConfigure,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(10));
} // namespace frost::debug
