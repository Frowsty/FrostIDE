// SPDX-License-Identifier: Apache-2.0
// Authenticated connection to the experimental FrostEngine Lua debug endpoint.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/loopback_stream.hpp"

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string_view>

namespace frost::debug {

struct LuaDebugPlayConnection {
    std::unique_ptr<io::LoopbackStream> stream;
    std::uint64_t connectionGeneration = 0;
    std::uint64_t operationId = 0;
    std::int64_t nextWireRequest = 0;
};

// Reserves and arms one exact play operation before returning the authenticated stream.
[[nodiscard]] Result<LuaDebugPlayConnection> connectLuaDebugPlayEndpoint(std::uint16_t port, std::string_view token,
                                                                         io::LoopbackStream::Deadline deadline,
                                                                         std::stop_token stop = {});

// Authenticates before returning a duplex stream. The caller must first enforce project trust and
// verify the endpoint belongs to the intended project. The token is an out-of-band secret. The
// returned stream carries the engine's experimental protocol, not DAP. The deadline bounds the
// connection and handshake; errors close the stream. When armPause is true, the engine acknowledges
// that a pause is queued before this returns, so a subsequent play.start cannot outrun it. Call off
// the UI thread.
[[nodiscard]] Result<std::unique_ptr<io::LoopbackStream>>
connectLuaDebugEndpoint(std::uint16_t port, std::string_view token, io::LoopbackStream::Deadline deadline,
                        bool armPause = false, std::stop_token stop = {},
                        std::uint64_t* connectionGeneration = nullptr);

} // namespace frost::debug
