// SPDX-License-Identifier: Apache-2.0
// A thread-safe, replaceable language requester; in-flight requests retain their original server.
#pragma once

#include "frost/core/mutex.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/uri.hpp"

#include <memory>
#include <utility>

namespace frost::app {

class StableLspRequester final : public lang::ILspRequester {
public:
    void setServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_server = std::move(server);
    }

    void setLuaServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_lua = std::move(server);
    }
    void setManagedServer(std::shared_ptr<lang::ILspRequester> server) {
        LockGuard lock(m_mutex);
        m_managed = std::move(server);
    }
    void setActivePath(std::string_view path) {
        LockGuard lock(m_mutex);
        m_activeLua = isLua(path);
        m_activeManaged = isCSharp(path);
        m_hasActive = m_activeLua || m_activeManaged || isCpp(path);
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
    [[nodiscard]] std::shared_ptr<lang::ILspRequester> server() const {
        LockGuard lock(m_mutex);
        return m_hasActive ? (m_activeLua ? m_lua : m_activeManaged ? m_managed : m_server) : nullptr;
    }
    [[nodiscard]] std::shared_ptr<lang::ILspRequester> serverFor(const lang::Json& params) const {
        const auto* doc = params.find("textDocument");
        if (doc) {
            auto uri = doc->stringAt("uri");
            auto path = lang::uriToPath(uri);
            if (!path)
                return nullptr;
            LockGuard lock(m_mutex);
            return isLua(*path) ? m_lua : isCSharp(*path) ? m_managed : isCpp(*path) ? m_server : nullptr;
        }
        return server();
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
    bool m_activeManaged FROST_GUARDED_BY(m_mutex) = false;
    bool m_activeLua FROST_GUARDED_BY(m_mutex) = false;
    bool m_hasActive FROST_GUARDED_BY(m_mutex) = true;
};

} // namespace frost::app
