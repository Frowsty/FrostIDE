// SPDX-License-Identifier: Apache-2.0
// Adopt an authenticated standard-DAP engine endpoint without synthesizing adapter replies.
#pragma once
#include "frost/debug/debug_session.hpp"
#include "frost/debug/lua_debug_endpoint.hpp"

namespace frost::debug {
// Consumes a worker-authenticated connection exactly once. Factory invocation only adopts the
// stream and starts the ordinary DAP client; no GUI network handshake. Capabilities, threads,
// frames and events are exclusively actual engine DAP output. Detach never terminates the host.
[[nodiscard]] AdapterFactory luaDebugAdapterFactory(LuaDebugConnection connection);

// Retired experimental overloads remain compile compatible but refuse before any network I/O.
[[nodiscard]] AdapterFactory luaDebugAdapterFactory(std::uint16_t port, std::string token,
                                                    std::chrono::milliseconds timeout, bool pauseOnStart = false);
[[nodiscard]] AdapterFactory luaDebugAdapterFactory(std::unique_ptr<io::LoopbackStream> authenticatedStream,
                                                    std::int64_t nextWireRequest = 2, std::uint64_t playOperation = 0);
} // namespace frost::debug
