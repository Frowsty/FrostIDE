// SPDX-License-Identifier: Apache-2.0
// Experimental DAP adapter for FrostEngine's authenticated Lua debug endpoint.
#pragma once

#include "frost/debug/debug_session.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace frost::io {
class LoopbackStream;
}

namespace frost::debug {

// Connects and authenticates on factory invocation. The caller must enforce project trust and verify
// endpoint identity before creating this factory. Session setup, pause/continue, captured stacks/sources
// and owned-play Stop are supported; breakpoints, variables and stepping fail explicitly. pauseOnStart arms a
// stop before the factory returns so play may start after setup without racing the first Lua line. The secret is held
// by the returned closure and never logged. This adapter does not terminate an attached engine.
[[nodiscard]] AdapterFactory luaDebugAdapterFactory(std::uint16_t port, std::string token,
                                                    std::chrono::milliseconds timeout, bool pauseOnStart = false);

// Takes a stream that was authenticated and optionally armed on a worker. The returned factory
// consumes it exactly once, so starting a session never waits for the network on the GUI thread.
[[nodiscard]] AdapterFactory luaDebugAdapterFactory(std::unique_ptr<io::LoopbackStream> authenticatedStream,
                                                    std::int64_t nextWireRequest = 2, std::uint64_t playOperation = 0);

} // namespace frost::debug
