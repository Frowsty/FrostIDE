// SPDX-License-Identifier: Apache-2.0
// A thread-safe, replaceable language requester; in-flight requests retain their original server.
#pragma once

#include "frost/core/mutex.hpp"
#include "frost/lang/json_schema.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/msbuild_source.hpp"
#include "frost/syntax/shader_source.hpp"

#include "engine_shader_language_requester.hpp"

#include <map>
#include <memory>
#include <utility>

namespace frost::app {

class StableLspRequester final : public lang::ILspRequester {
public:
    void setServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_server = std::move(server);
        if (m_server && m_applyEditHandler)
            m_server->setApplyEditHandler(m_applyEditHandler);
    }

    void setLuaServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_lua = std::move(server);
        if (m_lua && m_applyEditHandler)
            m_lua->setApplyEditHandler(m_applyEditHandler);
    }
    void setJsonServer(std::shared_ptr<lang::ILspRequester> server,
                       std::function<bool(std::string_view)> handles = {}) {
        LockGuard lock(m_mutex);
        m_json = std::move(server);
        m_jsonHandles = std::move(handles);
    }
    void setShaderSourceServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_shaderSource = std::move(server);
    }
    void setCMakeServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_cmake = std::move(server);
    }
    void setMSBuildServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_msbuild = std::move(server);
    }
    void setShaderServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_shader = std::move(server);
    }
    void setManagedServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_managed = std::move(server);
        if (m_managed && m_applyEditHandler)
            m_managed->setApplyEditHandler(m_applyEditHandler);
    }
    void setRegisteredServers(std::map<std::string, std::shared_ptr<lang::ILspRequester>> routes) {
        LockGuard lock(m_mutex);
        m_registered = std::move(routes);
        for (const auto& [extension, server] : m_registered)
            if (server && m_applyEditHandler)
                server->setApplyEditHandler(m_applyEditHandler);
    }
    void setApplyEditHandler(ApplyEditHandler handler) override {
        LockGuard lock(m_mutex);
        m_applyEditHandler = std::move(handler);
        for (const auto& [extension, server] : m_registered)
            if (server)
                server->setApplyEditHandler(m_applyEditHandler);
        if (m_server)
            m_server->setApplyEditHandler(m_applyEditHandler);
        if (m_lua)
            m_lua->setApplyEditHandler(m_applyEditHandler);
        if (m_managed)
            m_managed->setApplyEditHandler(m_applyEditHandler);
    }
    void setActivePath(std::string_view path) {
        LockGuard lock(m_mutex);
        m_activePath = path;
        m_activeShader = isEngineShaderSource(path);
        m_activeLua = isLua(path);
        m_activeManaged = isCSharp(path);
        m_activeJson = m_jsonHandles ? m_jsonHandles(path) : lang::hasBundledJsonSchema(path);
        m_activeCMake = isCMake(path);
        m_activeMSBuild = project::isMSBuildSource(path);
        m_hasActive = m_activeShader || m_activeLua || m_activeManaged || m_activeJson || m_activeCMake ||
                      m_activeMSBuild || isCpp(path);
    }

    [[nodiscard]] sched::Task<Result<lang::Json>> request(std::string method, lang::Json params,
                                                          std::stop_token token = {}) override {
        // This is deliberately not a coroutine: ownership is captured before the lazy task starts.
        auto selected = serverFor(params);
        return invoke(std::move(selected), std::move(method), std::move(params), std::move(token));
    }

    [[nodiscard]] lang::PositionEncoding encoding() const override {
        auto current = server();
        return current ? current->encoding() : lang::PositionEncoding::Utf16;
    }
    [[nodiscard]] std::optional<lang::ServerCapabilities> serverCapabilities() const override {
        auto current = server();
        return current ? current->serverCapabilities() : std::nullopt;
    }

private:
    static bool isCMake(std::string_view path) {
        const auto slash = path.find_last_of("/\\");
        const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
        return name == "CMakeLists.txt" || path.ends_with(".cmake");
    }
    static bool isLua(std::string_view path) {
        if (path.size() < 4)
            return false;
        auto ext = path.substr(path.size() - 4);
        return ext[0] == '.' && (ext[1] == 'l' || ext[1] == 'L') && (ext[2] == 'u' || ext[2] == 'U') &&
               (ext[3] == 'a' || ext[3] == 'A');
    }
    static bool isCSharp(std::string_view path) {
        if (path.size() < 4)
            return false;
        auto ext = path.substr(path.size() - 3);
        return ext[0] == '.' && (ext[1] == 'c' || ext[1] == 'C') && (ext[2] == 's' || ext[2] == 'S');
    }
    static bool isCpp(std::string_view path) {
        const auto dot = path.find_last_of('.');
        const auto slash = path.find_last_of("/\\");
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
            return false;
        std::string extension(path.substr(dot));
        for (char& c : extension)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + ('a' - 'A'));
        for (std::string_view candidate : {".c", ".cc", ".cpp", ".cxx", ".c++", ".cppm", ".ixx", ".h", ".hh", ".hpp",
                                           ".hxx", ".h++", ".inl", ".ipp", ".tpp", ".m", ".mm", ".cu"})
            if (extension == candidate)
                return true;
        return false;
    }
    [[nodiscard]] std::shared_ptr<lang::ILspRequester> registeredFor(std::string_view path) const
        FROST_REQUIRES(m_mutex) {
        const auto found = m_registered.find(lang::documentKey(path));
        if (found != m_registered.end())
            return found->second;
        return syntax::shaderSourceKind(path) != syntax::ShaderSourceKind::None ? m_shaderSource : nullptr;
    }
    [[nodiscard]] std::shared_ptr<lang::ILspRequester> server() const {
        LockGuard lock(m_mutex);
        if (!m_hasActive)
            return registeredFor(m_activePath);
        return m_hasActive ? (m_activeShader    ? m_shader
                              : m_activeLua     ? m_lua
                              : m_activeManaged ? m_managed
                              : m_activeJson    ? m_json
                              : m_activeMSBuild ? m_msbuild
                              : m_activeCMake   ? m_cmake
                                                : m_server)
                           : nullptr;
    }
    [[nodiscard]] std::shared_ptr<lang::ILspRequester> serverFor(const lang::Json& params) const {
        const auto* doc = params.find("textDocument");
        if (!doc)
            doc = params.find("item");
        if (doc) {
            auto uri = doc->stringAt("uri");
            auto path = lang::uriToPath(uri);
            if (!path)
                return nullptr;
            LockGuard lock(m_mutex);
            return isEngineShaderSource(*path)                                                  ? m_shader
                   : isLua(*path)                                                               ? m_lua
                   : isCSharp(*path)                                                            ? m_managed
                   : (m_jsonHandles ? m_jsonHandles(*path) : lang::hasBundledJsonSchema(*path)) ? m_json
                   : project::isMSBuildSource(*path)                                            ? m_msbuild
                   : isCMake(*path)                                                             ? m_cmake
                   : isCpp(*path)                                                               ? m_server
                                                                                                : registeredFor(*path);
        }
        if (auto active = server())
            return active;
        LockGuard lock(m_mutex);
        return m_server ? m_server : m_managed ? m_managed : m_lua;
    }
    static sched::Task<Result<lang::Json>> invoke(std::shared_ptr<lang::ILspRequester> server, std::string method,
                                                  lang::Json params, std::stop_token token) {
        if (token.stop_requested()) {
            co_return fail(ErrorCode::Cancelled, method + ": request cancelled");
        }
        if (!server) {
            co_return fail(ErrorCode::Busy, method + ": language server is not running");
        }
        co_return co_await server->request(std::move(method), std::move(params), std::move(token));
    }

    mutable Mutex m_mutex;
    std::shared_ptr<lang::ILspRequester> m_server FROST_GUARDED_BY(m_mutex);
    std::shared_ptr<lang::ILspRequester> m_lua FROST_GUARDED_BY(m_mutex);
    std::shared_ptr<lang::ILspRequester> m_managed FROST_GUARDED_BY(m_mutex);
    std::shared_ptr<lang::ILspRequester> m_json FROST_GUARDED_BY(m_mutex);
    std::function<bool(std::string_view)> m_jsonHandles FROST_GUARDED_BY(m_mutex);
    std::shared_ptr<lang::ILspRequester> m_cmake FROST_GUARDED_BY(m_mutex);
    std::shared_ptr<lang::ILspRequester> m_shaderSource FROST_GUARDED_BY(m_mutex);
    std::map<std::string, std::shared_ptr<lang::ILspRequester>> m_registered FROST_GUARDED_BY(m_mutex);
    std::string m_activePath FROST_GUARDED_BY(m_mutex);
    std::shared_ptr<lang::ILspRequester> m_shader FROST_GUARDED_BY(m_mutex);
    bool m_activeShader FROST_GUARDED_BY(m_mutex) = false;
    bool m_activeJson FROST_GUARDED_BY(m_mutex) = false;
    bool m_activeCMake FROST_GUARDED_BY(m_mutex) = false;
    std::shared_ptr<lang::ILspRequester> m_msbuild FROST_GUARDED_BY(m_mutex);
    bool m_activeMSBuild FROST_GUARDED_BY(m_mutex) = false;
    ApplyEditHandler m_applyEditHandler FROST_GUARDED_BY(m_mutex);
    bool m_activeManaged FROST_GUARDED_BY(m_mutex) = false;
    bool m_activeLua FROST_GUARDED_BY(m_mutex) = false;
    bool m_hasActive FROST_GUARDED_BY(m_mutex) = true;
};

} // namespace frost::app
