// SPDX-License-Identifier: Apache-2.0
// Native auth and launch policy does not offer a production subscription bypass.
#include "frost/agent/protocol.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include <algorithm>
namespace frost::agent {
Result<AuthRoute> nativeAuthRoute(const Json& status) {
    if (!status.isObject() || !status.boolAt("loggedIn").value_or(false))
        return fail(ErrorCode::PermissionDenied, "Sign in with your CLI in Agent Terminal, then recheck native session authentication");
    const auto method = status.stringAt("authMethod");
    if (method == "claude.ai" || method == "oauth_token" || method == "subscription") {
        if constexpr (kSubscriptionApprovalRecorded) return AuthRoute::Subscription;
        return fail(ErrorCode::PermissionDenied, "Native subscription authentication requires recorded written provider approval. Use an API key/apiKeyHelper, a supported third-party provider, or Agent Terminal");
    }
    if (method == "api_key" || method == "apiKey") return AuthRoute::ApiKey;
    if (method == "api_key_helper" || method == "apiKeyHelper") return AuthRoute::ApiKeyHelper;
    if (method == "bedrock") return AuthRoute::Bedrock;
    if (method == "vertex") return AuthRoute::Vertex;
    return fail(ErrorCode::Unsupported, "Unknown authentication method; use Agent Terminal or configure an explicitly supported native provider");
}
Result<io::ProcessSpec> nativeLaunch(const NativeLaunch& launch, const Json& status) {
    auto route = nativeAuthRoute(status);
    if (!route) return std::unexpected(route.error());
    for (const auto* path : {&launch.executable, &launch.root, &launch.settingsFile, &launch.mcpConfig})
        if (!io::isAbsolutePath(*path)) return fail(ErrorCode::InvalidArgument, "native session executable/root/config paths must be absolute");
    if ((!launch.resumeAt.empty() || launch.forkSession) && launch.resume.empty())
        return fail(ErrorCode::InvalidArgument, "fork/resume-at requires an indexed session identity");
    for (const auto* value : {&launch.resume, &launch.resumeAt, &launch.model, &launch.effort})
        if (value->find(char(0)) != std::string::npos || value->size() > 256)
            return fail(ErrorCode::InvalidArgument, "invalid native session option");
    io::ProcessSpec spec;
    spec.executable = launch.executable;
    spec.workingDirectory = launch.root;
    spec.arguments = {"-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
        "--include-partial-messages", "--replay-user-messages", "--permission-prompt-tool", "stdio", "--ide",
        "--settings", launch.settingsFile, "--mcp-config", launch.mcpConfig,
        "--permission-mode", "default", "--tools", "AskUserQuestion,ExitPlanMode"};
    if (*route != AuthRoute::Subscription) spec.arguments.insert(spec.arguments.begin(), "--bare");
    if (!launch.resume.empty()) { spec.arguments.push_back("--resume"); spec.arguments.push_back(launch.resume); }
    if (launch.forkSession) spec.arguments.push_back("--fork-session");
    if (!launch.resumeAt.empty()) { spec.arguments.push_back("--resume-session-at"); spec.arguments.push_back(launch.resumeAt); }
    if (!launch.model.empty()) { spec.arguments.push_back("--model"); spec.arguments.push_back(launch.model); }
    if (!launch.effort.empty()) { spec.arguments.push_back("--effort"); spec.arguments.push_back(launch.effort); }
    spec.environment.emplace_back("CLAUDE_CODE_ENABLE_SDK_FILE_CHECKPOINTING", "true");
    // Explicit provider selection never inherits a conflicting subscription route into native chat.
    spec.environment.emplace_back("CLAUDE_CODE_USE_BEDROCK", *route == AuthRoute::Bedrock ? std::optional<std::string>("1") : std::nullopt);
    spec.environment.emplace_back("CLAUDE_CODE_USE_VERTEX", *route == AuthRoute::Vertex ? std::optional<std::string>("1") : std::nullopt);
    return spec;
}
Result<std::vector<Json>> NdjsonFramer::feed(std::string_view bytes) {
    if (m_failed) return fail(ErrorCode::Corrupt, "native protocol framer is closed");
    std::vector<Json> events;
    for (char byte : bytes) {
        if (byte != '\n') {
            if (m_line.size() >= kMaximumLine) { m_failed = true; m_line.clear(); return fail(ErrorCode::OutOfRange, "native protocol line exceeds cap"); }
            m_line.push_back(byte);
            continue;
        }
        if (!m_line.empty() && m_line.back() == '\r') m_line.pop_back();
        if (m_line.empty() || !io::isValidUtf8(m_line)) { m_failed = true; return fail(ErrorCode::Corrupt, "native protocol requires nonempty UTF-8 JSON lines"); }
        auto parsed = rpc::parseJson(m_line);
        m_line.clear();
        if (!parsed || !parsed->isObject()) { m_failed = true; return fail(ErrorCode::Corrupt, "malformed native JSON event"); }
        events.push_back(std::move(*parsed));
    }
    return events;
}
Result<void> NdjsonFramer::finish() const {
    if (m_failed || !m_line.empty()) return fail(ErrorCode::Corrupt, "native protocol ended with an incomplete event");
    return {};
}
bool NativeProtocol::supports(std::string_view subtype) const {
    return subtype == "initialize" || (ready() && m_capabilities.contains(subtype));
}
Result<Json> NativeProtocol::control(std::string subtype, Json fields, Time now) {
    if (!fields.isObject() || !supports(subtype) || m_pending.size() >= 32)
        return fail(ErrorCode::Unsupported, "control is unavailable before compatible capabilities or pending-control capacity");
    const auto id = "frost-" + std::to_string(++m_next);
    fields.set("subtype", std::move(subtype));
    m_pending.emplace(id, now + std::chrono::seconds(60));
    return Json::object().set("type", "control_request").set("request_id", id).set("request", std::move(fields));
}
Result<void> NativeProtocol::receive(const Json& event) {
    if (!event.isObject()) return fail(ErrorCode::Corrupt, "native event must be an object");
    if (event.stringAt("type") == "control_response") {
        const auto* response = event.find("response");
        if (!response || !response->isObject() || !m_pending.erase(std::string(response->stringAt("request_id"))))
            return fail(ErrorCode::InvalidArgument, "unmatched native control response");
    } else if (event.stringAt("type") == "system" && event.stringAt("subtype") == "init") {
        const auto version = event.stringAt("claude_code_version");
        unsigned major = 0, minor = 0, patch = 0;
        const auto parse = [&](std::string_view part, unsigned& number) {
            if (part.empty() || part.size() > 8) return false;
            for (char byte : part) { if (byte < '0' || byte > '9') return false; number = number * 10 + static_cast<unsigned>(byte - '0'); }
            return true;
        };
        const auto first = version.find('.'), last = version.rfind('.');
        if (first == std::string_view::npos || first == last || !parse(version.substr(0, first), major) ||
            !parse(version.substr(first + 1, last - first - 1), minor) || !parse(version.substr(last + 1), patch) ||
            (major < 2 || (major == 2 && (minor < 1 || (minor == 1 && patch < 316)))))
            return fail(ErrorCode::Unsupported, "native chat requires a compatible CLI version at least 2.1.316");
        m_initialized = true;
        m_ide = event.boolAt("ide_connected").value_or(false);
        m_session = std::string(event.stringAt("session_id"));
        if (const auto* servers = event.find("mcp_servers"); servers && servers->isArray())
            for (const auto& server : servers->items()) if (server.stringAt("name") == "frostide" && server.stringAt("status") == "connected") m_mcp = true;
        if (const auto* controls = event.find("supported_control_requests"); controls && controls->isArray())
            for (const auto& control : controls->items()) if (control.isString()) m_capabilities.emplace(control.asString());
    }
    return {};
}
Result<Json> NativeProtocol::userTurn(std::string text) const {
    if (!ready()) return fail(ErrorCode::Busy, "wait for actual compatible CLI initialization, IDE and frostide MCP connection");
    if (text.empty() || text.size() > 256 * 1024 || !io::isValidUtf8(text)) return fail(ErrorCode::InvalidArgument, "invalid or oversized user turn");
    return Json::object().set("type", "user").set("message", Json::object().set("role", "user").set("content", std::move(text)));
}
std::vector<Json> NativeProtocol::expire(Time now) {
    std::vector<Json> cancelled;
    for (auto item = m_pending.begin(); item != m_pending.end();) {
        if (item->second > now) { ++item; continue; }
        cancelled.push_back(Json::object().set("type", "control_cancel_request").set("request_id", item->first));
        item = m_pending.erase(item);
    }
    return cancelled;
}
Json StopHookGuard::evaluate(std::string fingerprint, std::string text, bool active) {
    if (active || fingerprint.empty() || text.empty()) return Json::object();
    if (fingerprint != m_fingerprint) { m_fingerprint = std::move(fingerprint); m_blocks = 0; }
    text.resize(io::validUtf8Prefix(std::string_view(text).substr(0, 4096)));
    if (m_blocks++ < 3) return Json::object().set("decision", "block").set("reason", std::move(text));
    return Json::object().set("warning", std::move(text));
}
void StopHookGuard::reset() { m_fingerprint.clear(); m_blocks = 0; }
}
