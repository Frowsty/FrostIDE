// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/debug/debug_session.hpp"

#include <chrono>
namespace frost::debug {
// CMake --debugger-pipe is a named pipe on Windows and an AF_UNIX socket on Linux.
// This connects only to an endpoint allocated by the configure owner, never TCP or stdio.
// The configure BuildSession owns the process; closing this transport invokes cancelConfigure.
// The factory returns immediately. An owned cancellable worker connects under the startup
// deadline while DAP's reader/writer wait; connection errors become the ordinary onClosed path.
[[nodiscard]] AdapterFactory cmakeDapFactory(std::string endpoint, std::function<void()> cancelConfigure,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(10));
} // namespace frost::debug
