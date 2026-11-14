// SPDX-License-Identifier: Apache-2.0
//
// FeatureServer: an initialized LspClient over the scripted FakeServer for the navigation and
// completion model tests (ADR 0041). Each test registers an answer per method; the server records
// every request's params, so tests can check what the models sent.
#pragma once

#include "frost/core/mutex.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_client.hpp"

#include "lang/fake_server.hpp"
#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace frost::test {

class FeatureServer {
public:
    // Answers `params` with a result; a result object holding "__error" is sent as an error
    // response with that message instead.
    using Handler = std::function<lang::Json(const lang::Json& params)>;

    explicit FeatureServer(lang::Json capabilities = defaultCapabilities())
        : m_pipe(m_env.pool()),
          m_server(m_pipe,
                   [this, capabilities = std::move(capabilities)](FakeServer& server, const lang::Json& message) {
                       return serve(server, message, capabilities);
                   }),
          m_client(m_pipe.clientStream(), m_env.pool(), initOptions(), {}) {
        auto initialized = m_env.run(m_client.initialize());
        EXPECT_TRUE(initialized.has_value()) << initialized.error().describe();
    }
    ~FeatureServer() { m_client.close(); }
    FeatureServer(const FeatureServer&) = delete ("owns a server thread");
    FeatureServer& operator=(const FeatureServer&) = delete ("owns a server thread");

    // clangd-like capabilities: UTF-8 positions and every feature provider.
    [[nodiscard]] static lang::Json defaultCapabilities() {
        return lang::Json::object()
            .set("positionEncoding", "utf-8")
            .set("textDocumentSync", 2)
            .set("completionProvider",
                 lang::Json::object()
                     .set("resolveProvider", false)
                     .set("triggerCharacters", lang::Json::array({lang::Json("."), lang::Json(">"), lang::Json(":")})))
            .set("signatureHelpProvider",
                 lang::Json::object().set("triggerCharacters", lang::Json::array({lang::Json("("), lang::Json(",")})))
            .set("hoverProvider", true)
            .set("definitionProvider", true)
            .set("declarationProvider", true)
            .set("implementationProvider", true)
            .set("referencesProvider", true)
            .set("documentHighlightProvider", true)
            .set("documentSymbolProvider", true)
            .set("workspaceSymbolProvider", true)
            .set("renameProvider", lang::Json::object().set("prepareProvider", true))
            .set("codeActionProvider", true)
            .set("documentFormattingProvider", true)
            .set("documentRangeFormattingProvider", true);
    }

    void on(std::string method, Handler handler) {
        LockGuard lock(m_mutex);
        m_handlers[std::move(method)] = std::move(handler);
    }

    // The params of every `method` request so far, in order.
    [[nodiscard]] std::vector<lang::Json> paramsOf(std::string_view method) const {
        std::vector<lang::Json> out;
        for (const lang::Json& message : m_server.received()) {
            if (message.stringAt("method") == method) {
                const lang::Json* params = message.find("params");
                out.push_back(params != nullptr ? *params : lang::Json());
            }
        }
        return out;
    }

    [[nodiscard]] lang::LspClient& client() noexcept { return m_client; }
    // The pool the client and the models' coroutines run on.
    [[nodiscard]] sched::IScheduler& scheduler() noexcept { return m_env.pool(); }

    template<class T>
    T run(sched::Task<T> task) {
        return m_env.run(std::move(task));
    }

private:
    static lang::InitializeOptions initOptions() {
        lang::InitializeOptions init;
#ifdef _WIN32
        init.rootPath = "C:\\work\\proj";
#else
        init.rootPath = "/work/proj";
#endif
        return init;
    }

    bool serve(FakeServer& server, const lang::Json& message, const lang::Json& capabilities) {
        const std::string method(message.stringAt("method"));
        const lang::Json* id = message.find("id");
        if (id == nullptr) {
            return true; // A notification.
        }
        if (method == "initialize") {
            server.respond(*id, lang::Json::object().set("capabilities", capabilities));
            return true;
        }
        if (method == "shutdown") {
            server.respond(*id, lang::Json());
            return true;
        }
        Handler handler;
        {
            LockGuard lock(m_mutex);
            auto it = m_handlers.find(method);
            if (it != m_handlers.end()) {
                handler = it->second;
            }
        }
        const lang::Json* params = message.find("params");
        lang::Json result = handler ? handler(params != nullptr ? *params : lang::Json()) : lang::Json();
        if (result.isObject() && result.find("__error") != nullptr) {
            server.send(lang::Json::object()
                            .set("jsonrpc", "2.0")
                            .set("id", *id)
                            .set("error", lang::Json::object()
                                              .set("code", -32803)
                                              .set("message", std::string(result.stringAt("__error")))));
            return true;
        }
        server.respond(*id, std::move(result));
        return true;
    }

    LangEnv m_env;
    MemoryPipe m_pipe;
    mutable Mutex m_mutex;
    std::map<std::string, Handler> m_handlers FROST_GUARDED_BY(m_mutex);
    FakeServer m_server;
    lang::LspClient m_client;
};

// An absolute path for test documents ("C:\work\proj\<name>" or "/work/proj/<name>").
[[nodiscard]] inline std::string workPath(std::string_view name) {
#ifdef _WIN32
    return std::string("C:\\work\\proj\\") + std::string(name);
#else
    return std::string("/work/proj/") + std::string(name);
#endif
}

} // namespace frost::test
