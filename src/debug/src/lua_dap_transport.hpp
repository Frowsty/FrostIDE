// SPDX-License-Identifier: Apache-2.0
// Direct standard-DAP transport after the private Lua endpoint authentication exchange.
#pragma once
#include "frost/debug/lua_debug_endpoint.hpp"

namespace frost::debug::detail {
[[nodiscard]] std::unique_ptr<IDapTransport> makeLuaDapTransport(std::unique_ptr<io::LoopbackStream> stream,
                                                                 LuaDebugIdentity identity, std::string credential,
                                                                 LuaDebugAdmission admission, std::stop_token stop,
                                                                 std::int64_t lastServerSequence,
                                                                 LuaDebugTransportCallbacks callbacks);
} // namespace frost::debug::detail
