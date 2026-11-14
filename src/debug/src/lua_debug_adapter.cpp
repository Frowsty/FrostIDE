// SPDX-License-Identifier: Apache-2.0
// One-use handoff from private Lua authentication to the ordinary real DAP client.
#include "frost/debug/lua_debug_adapter.hpp"

#include <mutex>
#include <optional>
#include <utility>

#include <sodium.h>

namespace frost::debug {
AdapterFactory luaDebugAdapterFactory(LuaDebugConnection connection) {
    struct Pending {
        std::mutex mutex;
        std::optional<LuaDebugConnection> connection;
    };
    auto pending = std::make_shared<Pending>();
    pending->connection.emplace(std::move(connection));
    return [pending](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        std::optional<LuaDebugConnection> connection;
        {
            std::lock_guard lock(pending->mutex);
            connection = std::move(pending->connection);
            pending->connection.reset();
        }
        if (!connection)
            return fail(ErrorCode::Busy, "the authenticated Lua debug connection was already adopted");
        auto admission = std::move(*connection).takeWriteAdmission();
        auto originalAdmission = std::move(handlers.beforeWrite);
        handlers.beforeWrite = [admission = std::move(admission),
                                original = std::move(originalAdmission)](std::string_view command) -> Result<void> {
            if (original) {
                auto admitted = original(command);
                if (!admitted)
                    return admitted;
            }
            return admission ? admission(command)
                             : Result<void>(fail(ErrorCode::PermissionDenied, "Lua write admission is missing"));
        };
        auto transport = std::move(*connection).takeTransport();
        if (!transport)
            return fail(ErrorCode::PermissionDenied, "no authenticated Lua DAP transport is available");
        // Authentication consumed client sequences 1 and 2. No ID rewriting or local DAP reply
        // generation occurs: initialize and all subsequent operations reach the real endpoint.
        return makeDapClient(std::move(transport), std::move(handlers), 3);
    };
}
AdapterFactory luaDebugAdapterFactory(std::uint16_t, std::string token, std::chrono::milliseconds, bool) {
    sodium_memzero(token.data(), token.size());
    return [](DapHandlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        return fail(ErrorCode::Unsupported, "the experimental Lua debug adapter has been retired");
    };
}
AdapterFactory luaDebugAdapterFactory(std::unique_ptr<io::LoopbackStream> stream, std::int64_t, std::uint64_t) {
    if (stream)
        stream->close();
    return [](DapHandlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        return fail(ErrorCode::Unsupported, "an experimental stream is not authenticated standard Lua DAP");
    };
}
} // namespace frost::debug
