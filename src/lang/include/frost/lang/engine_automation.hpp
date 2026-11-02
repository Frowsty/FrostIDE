// SPDX-License-Identifier: Apache-2.0
// Trusted FrostEngine editor automation: discovery and newline JSON-RPC commands.
#pragma once
#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace frost::io {
class IFileSystem;
}
namespace frost::lang {
// An injectable connection, confined to one worker thread and one command.
class IEngineConnection {
public:
    virtual ~IEngineConnection() = default;
    [[nodiscard]] virtual Result<std::string> exchange(std::string_view request) = 0;
};
using EngineConnect = std::function<Result<std::unique_ptr<IEngineConnection>>(
    std::uint16_t, std::chrono::steady_clock::time_point, std::stop_token)>;
// Connects only to 127.0.0.1, never to a host named by a discovery document.
[[nodiscard]] EngineConnect loopbackEngineConnector();

// Runs one allowlisted command after authenticating and checking the live project.info directory.
// `sameProject` resolves and compares physical paths; `stillTrusted` is called immediately before
// connecting and before the command. Call from a worker, never the GUI thread. The shared deadline
// and stop token cover the whole conversation. Server messages are deliberately not returned on
// error, because an untrusted server can echo a secret. Timeout after a command was sent does not
// imply that it did not execute; callers must not automatically retry mutating commands.
[[nodiscard]] Result<Json> runEngineCommand(const Json& discovery, std::string_view projectRoot,
                                            std::string_view method, Json params,
                                            const std::function<bool(std::string_view, std::string_view)>& sameProject,
                                            const std::function<bool()>& stillTrusted, const EngineConnect& connect,
                                            std::chrono::steady_clock::time_point deadline, std::stop_token stop = {});
} // namespace frost::lang
