// SPDX-License-Identifier: Apache-2.0
// Private, credential-bearing Lua DAP bootstrap. Never serialize this value into public state.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace frost::engine {

// Owned in memory only. The exact authenticated R6 connection and actual server PID bind this
// one-use DAP lease; reconnecting requires a fresh bootstrap. Move-only, with credential erasure.
class ScriptDebugBootstrap {
public:
    explicit ScriptDebugBootstrap(std::string credential = {});
    ~ScriptDebugBootstrap();
    ScriptDebugBootstrap(const ScriptDebugBootstrap&) = delete;
    ScriptDebugBootstrap& operator=(const ScriptDebugBootstrap&) = delete;
    ScriptDebugBootstrap(ScriptDebugBootstrap&&) noexcept = default;
    ScriptDebugBootstrap& operator=(ScriptDebugBootstrap&&) noexcept;

    std::string projectPath, instanceId, generation, debugSessionId;
    std::uint16_t port = 0;
    std::int64_t processId = 0;
    std::uint64_t connectionGeneration = 0;
    std::chrono::steady_clock::time_point expiresAt{};
    // Consumes the credential for the separately layered debug transport. No formatter/JSON
    // conversion exists; callers must never persist or include this in diagnostics or snapshots.
    [[nodiscard]] std::string takeCredential() && noexcept { return std::move(m_credential); }

private:
    std::string m_credential;
};

} // namespace frost::engine
