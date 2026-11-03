// SPDX-License-Identifier: Apache-2.0
// FrostEngine automation discovery, requests and response validation.
#include "frost/lang/engine_automation.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/loopback_stream.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <array>
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
bool allowed(std::string_view method) {
    static constexpr std::array methods{
        "play.start",        "play.stop",         "play.state", "script.validate", "script.debug.start",
        "script.debug.stop", "script.debug.play", "test.run",   "log.get",         "editor.focus"};
    return std::ranges::find(methods, method) != methods.end();
}
Result<Json> call(IEngineConnection& connection, int id, std::string_view method, Json params) {
    auto request =
        Json::object().set("jsonrpc", "2.0").set("id", id).set("method", method).set("params", std::move(params));
    auto bytes = connection.exchange(request.dump());
    if (!bytes)
        return std::unexpected(std::move(bytes.error()));
    auto response = parseJson(*bytes);
    if (!response || !response->isObject() || response->stringAt("jsonrpc") != "2.0" || response->integerAt("id") != id)
        return fail(ErrorCode::Corrupt, "invalid engine response");
    if (response->find("error"))
        return fail(ErrorCode::IoError, "engine rejected the command");
    auto* result = response->find("result");
    if (!result || !result->isObject())
        return fail(ErrorCode::Corrupt, "invalid engine result");
    return std::move(*result);
}
} // namespace

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
    const bool debugStart = method == "script.debug.start";
    if (debugStart &&
        (!*requiresToken || discovery.stringAt("editorInstanceId").empty() ||
         discovery.integerAt("processId").value_or(0) <= 0 || discovery.integerAt("startedAtMs").value_or(0) <= 0 ||
         discovery.stringAt("projectFile").empty()))
        return fail(ErrorCode::PermissionDenied, "engine debug discovery lacks authenticated instance identity");
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "engine request cancelled");
    auto connection = connect(static_cast<std::uint16_t>(*port), deadline, stop);
    if (!connection)
        return std::unexpected(std::move(connection.error()));
    int id = 1;
    if (*requiresToken) {
        auto authenticated = call(**connection, id++, "auth", Json::object().set("token", discovery.stringAt("token")));
        if (!authenticated)
            return std::unexpected(std::move(authenticated.error()));
        if (authenticated->boolAt("authenticated") != true)
            return fail(ErrorCode::PermissionDenied, "engine authentication failed");
    }
    auto info = call(**connection, id++, "server.info", Json::object());
    if (!info)
        return std::unexpected(std::move(info.error()));
    if (info->integerAt("contractVersion").value_or(0) < 1)
        return fail(ErrorCode::Unsupported, "engine automation contract is unsupported");
    auto projectInfo = call(**connection, id++, "project.info", Json::object());
    if (!projectInfo)
        return std::unexpected(std::move(projectInfo.error()));
    if (!sameProject(projectRoot, projectInfo->stringAt("directory")))
        return fail(ErrorCode::PermissionDenied, "running engine has a different project open");
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "engine request cancelled");
    if (!stillTrusted())
        return fail(ErrorCode::Untrusted, "project is no longer trusted");
    auto result = call(**connection, id, method, std::move(params));
    // Only this explicit pre-action admission result proves that play did not start. RPC errors
    // and lost replies may occur after side effects and remain uncertain.
    if (result && method == "script.debug.play") {
        const auto admitted = result->boolAt("admitted");
        if (admitted == false)
            return fail(ErrorCode::PermissionDenied, "Lua debug endpoint was not admitted");
        if (admitted != true)
            return fail(ErrorCode::Corrupt, "invalid Lua debug play result");
    }
    if (!result || !debugStart)
        return result;
    const std::string endpointId(result->stringAt("endpointId"));
    const bool validEndpointId = endpointId.size() == 64 && std::ranges::all_of(endpointId, [](char c) {
                                     return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                                 });
    const auto revokeEndpoint = [&] {
        // A delayed cleanup must never close a replacement endpoint. Only revoke the exact identity returned here.
        if (validEndpointId)
            static_cast<void>(
                call(**connection, id + 1, "script.debug.stop", Json::object().set("endpointId", endpointId)));
    };
    if (stop.stop_requested()) {
        revokeEndpoint();
        return fail(ErrorCode::Cancelled, "engine request cancelled");
    }
    if (!stillTrusted()) {
        revokeEndpoint();
        return fail(ErrorCode::Untrusted, "project is no longer trusted");
    }
    const auto debugPort = result->integerAt("port");
    if (!validEndpointId || result->stringAt("host") != "127.0.0.1" || !debugPort || *debugPort < 1 ||
        *debugPort > 65535 || result->stringAt("token").size() < 32 || result->stringAt("token").size() > 1024 ||
        result->stringAt("projectPath").empty() || result->stringAt("projectFile").empty() ||
        !sameProject(projectRoot, result->stringAt("projectPath")) ||
        !sameProject(discovery.stringAt("projectFile"), result->stringAt("projectFile")) ||
        result->stringAt("editorInstanceId") != discovery.stringAt("editorInstanceId") ||
        result->integerAt("processId") != discovery.integerAt("processId") ||
        result->integerAt("automationStartedAtMs") != discovery.integerAt("startedAtMs")) {
        revokeEndpoint();
        return fail(ErrorCode::PermissionDenied, "engine debug endpoint identity does not match discovery");
    }
    return result;
}
} // namespace frost::lang
