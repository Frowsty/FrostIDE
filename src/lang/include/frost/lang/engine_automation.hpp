// SPDX-License-Identifier: Apache-2.0
// Trusted FrostEngine editor automation: discovery and newline JSON-RPC commands.
#pragma once
#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

// The longest engine error message shown, in bytes (redactEngineMessage).
inline constexpr std::size_t kEngineMessageLimit = 512;

// An engine-supplied message made safe to show: every occurrence of `secret` (the session token)
// replaced by "<redacted>" before it is shortened to kEngineMessageLimit bytes (at a UTF-8
// boundary, with "..."), and control characters (C0, DEL) and bidirectional formatting characters
// (U+061C, U+200E/F, U+202A-202E, U+2066-2069) each turned into one space, so the label cannot be
// visually reordered. Pure; O(message length).
[[nodiscard]] std::string redactEngineMessage(std::string_view message, std::string_view secret);

/*
 * How long one engine command may take, end to end, before the IDE gives up on it: nullopt means
 * no deadline (test.run runs as long as the tests do and ends only by cancellation); 30 s for
 * script.validate; 5 s for play.*, log.get, editor.focus and the rest. Pure.
 */
[[nodiscard]] std::optional<std::chrono::milliseconds> engineMethodTimeout(std::string_view method) noexcept;

/*
 * Runs one allowlisted command (play.start/stop/state, script.validate, test.run, log.get,
 * editor.focus; script.debug.* is FER-8's) after authenticating and checking the live
 * project.info directory. `sameProject` resolves and compares physical paths; `stillTrusted` is
 * called immediately before connecting and before the command. Call from a worker, never the GUI
 * thread. The shared deadline (time_point::max() for none) and stop token cover the whole
 * conversation. A JSON-RPC error from the engine comes back with the engine's own code and message,
 * "FrostEngine error <code>: <message>", the message passed through redactEngineMessage with the
 * discovery token, so a server that echoes the secret cannot reveal it; -32601 maps to Unsupported,
 * -32600/-32602 to InvalidArgument, the rest to IoError. Timeout after a command was sent does not
 * imply that it did not execute; callers must not automatically retry mutating commands.
 */
[[nodiscard]] Result<Json> runEngineCommand(const Json& discovery, std::string_view projectRoot,
                                            std::string_view method, Json params,
                                            const std::function<bool(std::string_view, std::string_view)>& sameProject,
                                            const std::function<bool()>& stillTrusted, const EngineConnect& connect,
                                            std::chrono::steady_clock::time_point deadline, std::stop_token stop = {});
} // namespace frost::lang
