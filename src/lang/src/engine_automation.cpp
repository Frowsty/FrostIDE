// SPDX-License-Identifier: Apache-2.0
// FrostEngine automation discovery, requests, per-method deadlines and response validation.
#include "frost/lang/engine_automation.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/loopback_stream.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace frost::lang {
namespace {
class SocketConnection final : public IEngineConnection {
public:
    explicit SocketConnection(std::unique_ptr<io::LoopbackStream> socket) : m_socket(std::move(socket)) {}
    Result<std::string> exchange(std::string_view request) override { return m_socket->exchange(request); }

private:
    std::unique_ptr<io::LoopbackStream> m_socket;
};
// The commands the IDE sends. script.debug.* is not here: the engine's Lua debug endpoint is on no
// branch, and FER-8 reimplements it (PLAN.md D34).
bool allowed(std::string_view method) {
    static constexpr std::array methods{"play.start", "play.stop", "play.state",  "script.validate",
                                        "test.run",   "log.get",   "editor.focus"};
    return std::ranges::find(methods, method) != methods.end();
}
// The engine's JSON-RPC error as an Error carrying its own code and message (redactEngineMessage).
Error engineError(const Json& error, std::string_view secret) {
    const auto code = error.integerAt("code");
    const std::string message = redactEngineMessage(error.stringAt("message"), secret);
    // JSON-RPC 2.0's reserved codes keep their meaning; everything else is the engine's own refusal.
    ErrorCode kind = ErrorCode::IoError;
    if (code == -32601)
        kind = ErrorCode::Unsupported;
    else if (code == -32602 || code == -32600)
        kind = ErrorCode::InvalidArgument;
    std::string text = code ? std::format("FrostEngine error {}", *code) : std::string("FrostEngine error");
    if (!message.empty())
        text += ": " + message;
    return Error(kind, std::move(text));
}
Result<Json> call(IEngineConnection& connection, int id, std::string_view method, Json params,
                  std::string_view secret) {
    auto request =
        Json::object().set("jsonrpc", "2.0").set("id", id).set("method", method).set("params", std::move(params));
    auto bytes = connection.exchange(request.dump());
    if (!bytes)
        return std::unexpected(std::move(bytes.error()));
    auto response = parseJson(*bytes);
    if (!response || !response->isObject() || response->stringAt("jsonrpc") != "2.0" || response->integerAt("id") != id)
        return fail(ErrorCode::Corrupt, "invalid engine response");
    if (const auto* error = response->find("error")) {
        if (!error->isObject())
            return fail(ErrorCode::Corrupt, "invalid engine error");
        return std::unexpected(engineError(*error, secret));
    }
    auto* result = response->find("result");
    if (!result || !result->isObject())
        return fail(ErrorCode::Corrupt, "invalid engine result");
    return std::move(*result);
}
} // namespace

std::string redactEngineMessage(std::string_view message, std::string_view secret) {
    // A hostile or buggy server can echo the session token back; never show it. Redact before
    // shortening, so a token cut at the limit cannot leak its first half.
    std::string text(message.substr(0, 64 * 1024));
    if (!secret.empty()) {
        constexpr std::string_view kRedacted = "<redacted>";
        for (std::size_t at = text.find(secret); at != std::string::npos; at = text.find(secret, at + kRedacted.size()))
            text.replace(at, secret.size(), kRedacted);
    }
    if (text.size() > kEngineMessageLimit) {
        std::size_t end = kEngineMessageLimit;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            --end; // Do not split a UTF-8 sequence.
        text.resize(end);
        text += "...";
    }
    // One line of printable text: control characters (bidi overrides are left to the label) become spaces.
    for (char& c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            c = ' ';
    }
    return text;
}

std::optional<std::chrono::milliseconds> engineMethodTimeout(std::string_view method) noexcept {
    using namespace std::chrono_literals;
    // A test run lasts as long as the tests do; only the user's Cancel ends it.
    if (method == "test.run")
        return std::nullopt;
    // Validation may compile a large script in the engine.
    if (method == "script.validate")
        return 30s;
    // State queries, play control, the log and focus answer at once or the editor is wedged.
    return 5s;
}

EngineConnect loopbackEngineConnector() {
    return [](std::uint16_t port, auto deadline, std::stop_token stop) -> Result<std::unique_ptr<IEngineConnection>> {
        auto socket = io::LoopbackStream::connect(port, deadline, stop);
        if (!socket)
            return std::unexpected(std::move(socket.error()));
        return std::unique_ptr<IEngineConnection>(new SocketConnection(std::move(*socket)));
    };
}

Result<Json> runEngineCommand(const Json& discovery, std::string_view projectRoot, std::string_view method, Json params,
                              const std::function<bool(std::string_view, std::string_view)>& sameProject,
                              const std::function<bool()>& stillTrusted, const EngineConnect& connect,
                              std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
    if (!allowed(method) || !params.isObject() || params.find("token") || !io::isAbsolutePath(projectRoot) ||
        !sameProject || !stillTrusted || !connect)
        return fail(ErrorCode::InvalidArgument, "invalid engine command");
    if (!stillTrusted())
        return fail(ErrorCode::Untrusted, "project is not trusted");
    const auto port = discovery.integerAt("port");
    const auto requiresToken = discovery.boolAt("tokenRequired");
    const auto project = discovery.stringAt("projectPath");
    if (discovery.stringAt("host") != "127.0.0.1" || !port || *port < 1 || *port > 65535 || !requiresToken ||
        project.empty() || !sameProject(projectRoot, project) ||
        (*requiresToken && discovery.stringAt("token").empty()))
        return fail(ErrorCode::Corrupt, "engine discovery does not match this project");
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "engine request cancelled");
    auto connection = connect(static_cast<std::uint16_t>(*port), deadline, stop);
    if (!connection)
        return std::unexpected(std::move(connection.error()));
    const std::string_view secret = *requiresToken ? discovery.stringAt("token") : std::string_view{};
    int id = 1;
    if (*requiresToken) {
        auto authenticated =
            call(**connection, id++, "auth", Json::object().set("token", discovery.stringAt("token")), secret);
        if (!authenticated)
            return std::unexpected(std::move(authenticated.error()));
        if (authenticated->boolAt("authenticated") != true)
            return fail(ErrorCode::PermissionDenied, "engine authentication failed");
    }
    auto info = call(**connection, id++, "server.info", Json::object(), secret);
    if (!info)
        return std::unexpected(std::move(info.error()));
    if (info->integerAt("contractVersion").value_or(0) < 1)
        return fail(ErrorCode::Unsupported, "engine automation contract is unsupported");
    auto projectInfo = call(**connection, id++, "project.info", Json::object(), secret);
    if (!projectInfo)
        return std::unexpected(std::move(projectInfo.error()));
    if (!sameProject(projectRoot, projectInfo->stringAt("directory")))
        return fail(ErrorCode::PermissionDenied, "running engine has a different project open");
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "engine request cancelled");
    if (!stillTrusted())
        return fail(ErrorCode::Untrusted, "project is no longer trusted");
    return call(**connection, id, method, std::move(params), secret);
}
} // namespace frost::lang
