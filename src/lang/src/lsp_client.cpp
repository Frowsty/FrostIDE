// SPDX-License-Identifier: Apache-2.0
//
// LspClient (lsp_client.hpp): capabilities, the lifecycle, server requests and the text-document
// notifications.

#include "frost/lang/lsp_client.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/document_features.hpp"
#include "frost/lang/semantic_tokens.hpp"
#include "frost/lang/uri.hpp"

#include "timeout.hpp"

#include <algorithm>
#include <format>

namespace frost::lang {
namespace {

[[nodiscard]] bool hasProvider(const Json& capabilities, std::string_view name) {
    const Json* value = capabilities.find(name);
    if (value == nullptr || value->isNull()) {
        return false;
    }
    if (const std::optional<bool> flag = value->asBool()) {
        return *flag;
    }
    return value->isObject(); // An options object means "supported".
}

// The strings of a JSON array (other items and kinds are skipped).
[[nodiscard]] std::vector<std::string> stringList(const Json* list) {
    std::vector<std::string> out;
    if (list != nullptr) {
        for (const Json& item : list->items()) {
            if (const std::string* text = item.asString()) {
                out.push_back(*text);
            }
        }
    }
    return out;
}

// The SymbolKind value set 1..26 (LSP 3.17), announced so servers send every kind.
[[nodiscard]] Json symbolKinds() {
    Json kinds = Json::array();
    for (int kind = 1; kind <= 26; ++kind) {
        kinds.push(kind);
    }
    return Json::object().set("valueSet", std::move(kinds));
}

[[nodiscard]] Json markupFormats() { return Json::array({Json("markdown"), Json("plaintext")}); }

[[nodiscard]] std::string_view lastPathComponent(std::string_view path) noexcept {
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
        path.remove_suffix(1);
    }
    const std::size_t separator = path.find_last_of("/\\");
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

} // namespace

ServerCapabilities parseInitializeResult(const Json& result) {
    ServerCapabilities out;
    const Json* capabilities = result.find("capabilities");
    if (capabilities == nullptr || !capabilities->isObject()) {
        return out;
    }
    out.raw = *capabilities;
    /*
     * positionEncoding (LSP 3.17) is the negotiated encoding; when a server omits it, the
     * specification's default is UTF-16. FrostIDE no longer offers clangd's deprecated
     * offsetEncoding extension, but a server that still answers with it unasked has said which
     * encoding it uses, so that answer counts when positionEncoding is absent
     * (LspClient::initialize rejects it unless it names one of the offered encodings).
     */
    std::string_view encoding = capabilities->stringAt("positionEncoding");
    if (encoding.empty()) {
        encoding = result.stringAt("offsetEncoding");
    }
    if (encoding.empty()) {
        encoding = capabilities->stringAt("offsetEncoding");
    }
    out.positionEncoding = parsePositionEncoding(encoding).value_or(PositionEncoding::Utf16);

    if (const Json* sync = capabilities->find("textDocumentSync"); sync != nullptr) {
        if (const std::optional<std::int64_t> kind = sync->asInteger()) {
            out.sync = static_cast<TextDocumentSyncKind>(std::clamp<std::int64_t>(*kind, 0, 2));
            out.openClose = out.sync != TextDocumentSyncKind::None;
        } else if (sync->isObject()) {
            out.openClose = sync->boolAt("openClose").value_or(false);
            out.sync = static_cast<TextDocumentSyncKind>(
                std::clamp<std::int64_t>(sync->integerAt("change").value_or(0), 0, 2));
            if (const Json* save = sync->find("save"); save != nullptr) {
                out.save = save->asBool().value_or(save->isObject());
                out.saveIncludeText = save->isObject() && save->boolAt("includeText").value_or(false);
            }
        }
    }
    out.completion = hasProvider(*capabilities, "completionProvider");
    out.definition = hasProvider(*capabilities, "definitionProvider");
    out.declaration = hasProvider(*capabilities, "declarationProvider");
    out.hover = hasProvider(*capabilities, "hoverProvider");
    out.references = hasProvider(*capabilities, "referencesProvider");
    out.documentSymbol = hasProvider(*capabilities, "documentSymbolProvider");
    out.signatureHelp = hasProvider(*capabilities, "signatureHelpProvider");
    if (const Json* completion = capabilities->find("completionProvider"); completion && completion->isObject()) {
        out.completionResolve = completion->boolAt("resolveProvider").value_or(false);
        out.completionTriggerCharacters = stringList(completion->find("triggerCharacters"));
    }
    if (const Json* signature = capabilities->find("signatureHelpProvider"); signature && signature->isObject()) {
        out.signatureHelpTriggerCharacters = stringList(signature->find("triggerCharacters"));
    }
    out.implementation = hasProvider(*capabilities, "implementationProvider");
    out.typeDefinition = hasProvider(*capabilities, "typeDefinitionProvider");
    out.documentHighlight = hasProvider(*capabilities, "documentHighlightProvider");
    out.workspaceSymbol = hasProvider(*capabilities, "workspaceSymbolProvider");
    out.rename = hasProvider(*capabilities, "renameProvider");
    if (const Json* rename = capabilities->find("renameProvider"); rename && rename->isObject()) {
        out.prepareRename = rename->boolAt("prepareProvider").value_or(false);
    }
    out.codeAction = hasProvider(*capabilities, "codeActionProvider");
    out.formatting = hasProvider(*capabilities, "documentFormattingProvider");
    out.rangeFormatting = hasProvider(*capabilities, "documentRangeFormattingProvider");
    out.callHierarchy = hasProvider(*capabilities, "callHierarchyProvider");
    out.typeHierarchy = hasProvider(*capabilities, "typeHierarchyProvider");
    out.inlayHint =
        hasProvider(*capabilities, "inlayHintProvider") || hasProvider(*capabilities, "clangdInlayHintsProvider");
    out.documentLink = hasProvider(*capabilities, "documentLinkProvider");
    out.foldingRange = hasProvider(*capabilities, "foldingRangeProvider");
    out.ast = hasProvider(*capabilities, "astProvider");
    if (const Json* info = result.find("serverInfo"); info != nullptr) {
        out.serverName = std::string(info->stringAt("name"));
        out.serverVersion = std::string(info->stringAt("version"));
    }
    return out;
}

Json clientCapabilities() {
    Json encodings = Json::array({Json("utf-8"), Json("utf-16")});
    Json synchronization = Json::object()
                               .set("dynamicRegistration", false)
                               .set("willSave", false)
                               .set("willSaveWaitUntil", false)
                               .set("didSave", true);
    Json publishDiagnostics = Json::object()
                                  .set("relatedInformation", true)
                                  .set("versionSupport", true)
                                  .set("codeDescriptionSupport", true)
                                  .set("dataSupport", false)
                                  .set("tagSupport", Json::object().set("valueSet", Json::array({Json(1), Json(2)})));
    // The navigation and completion features (W2, ADR 0041).
    Json completionKinds = Json::array();
    for (int kind = 1; kind <= 25; ++kind) {
        completionKinds.push(kind);
    }
    Json completion =
        Json::object()
            .set("dynamicRegistration", false)
            .set("contextSupport", true)
            .set("completionItem",
                 Json::object()
                     .set("snippetSupport", true)
                     .set("commitCharactersSupport", true)
                     .set("documentationFormat", markupFormats())
                     .set("deprecatedSupport", true)
                     .set("preselectSupport", true)
                     .set("insertReplaceSupport", false)
                     .set("labelDetailsSupport", true)
                     .set("resolveSupport",
                          Json::object().set("properties", Json::array({Json("documentation"), Json("detail"),
                                                                        Json("additionalTextEdits")}))))
            .set("completionItemKind", Json::object().set("valueSet", std::move(completionKinds)));
    Json signatureHelp = Json::object()
                             .set("dynamicRegistration", false)
                             .set("contextSupport", true)
                             .set("signatureInformation",
                                  Json::object()
                                      .set("documentationFormat", markupFormats())
                                      .set("activeParameterSupport", true)
                                      .set("parameterInformation", Json::object().set("labelOffsetSupport", false)));
    Json links = Json::object().set("dynamicRegistration", false).set("linkSupport", true);
    Json codeAction =
        Json::object()
            .set("dynamicRegistration", false)
            .set("isPreferredSupport", true)
            .set("codeActionLiteralSupport",
                 Json::object().set("codeActionKind",
                                    Json::object().set("valueSet", Json::array({Json(""), Json("quickfix"),
                                                                                Json("refactor"), Json("source")}))));
    Json textDocument =
        Json::object()
            .set("synchronization", std::move(synchronization))
            .set("publishDiagnostics", std::move(publishDiagnostics))
            .set("completion", std::move(completion))
            .set("hover", Json::object().set("dynamicRegistration", false).set("contentFormat", markupFormats()))
            .set("signatureHelp", std::move(signatureHelp))
            .set("declaration", links)
            .set("definition", links)
            .set("typeDefinition", links)
            .set("implementation", links)
            .set("references", Json::object().set("dynamicRegistration", false))
            .set("documentHighlight", Json::object().set("dynamicRegistration", false))
            .set("documentSymbol", Json::object()
                                       .set("dynamicRegistration", false)
                                       .set("hierarchicalDocumentSymbolSupport", true)
                                       .set("symbolKind", symbolKinds()))
            .set("codeAction", std::move(codeAction))
            .set("formatting", Json::object().set("dynamicRegistration", false))
            .set("rangeFormatting", Json::object().set("dynamicRegistration", false))
            .set("rename", Json::object().set("dynamicRegistration", false).set("prepareSupport", true))
            .set("callHierarchy", Json::object().set("dynamicRegistration", false))
            .set("typeHierarchy", Json::object().set("dynamicRegistration", false))
            .set("inlayHint", Json::object().set("dynamicRegistration", false))
            // Document links and folding ranges (CPP-5, document_features.hpp).
            .set("documentLink", documentLinkClientCapabilities())
            .set("foldingRange", foldingRangeClientCapabilities())
            // Semantic highlighting and clangd's inactive regions (M3, ADR 0047).
            .set("semanticTokens", semanticTokensClientCapabilities())
            .set("inactiveRegionsCapabilities", inactiveRegionsClientCapabilities());
    Json workspace =
        Json::object()
            .set("configuration", true)
            .set("workspaceFolders", true)
            .set("applyEdit", true)
            .set("executeCommand", Json::object().set("dynamicRegistration", false))
            .set("symbol", Json::object().set("dynamicRegistration", false).set("symbolKind", symbolKinds()))
            .set("workspaceEdit", Json::object().set("documentChanges", true))
            .set("semanticTokens", Json::object().set("refreshSupport", false))
            .set("inlayHint", Json::object().set("refreshSupport", true))
            .set("didChangeConfiguration", Json::object().set("dynamicRegistration", false));
    // LSP 3.17 negotiation only: clangd's pre-3.17 offsetEncoding extension is deprecated, and
    // clangd 23 reads general.positionEncodings and answers capabilities.positionEncoding.
    return Json::object()
        .set("general", Json::object().set("positionEncodings", encodings))
        .set("textDocument", std::move(textDocument))
        .set("workspace", std::move(workspace))
        .set("window", Json::object().set("workDoneProgress", true));
}

Result<Json> initializeParams(const InitializeOptions& options) {
    auto rootUri = pathToUri(options.rootPath);
    if (!rootUri) {
        return std::unexpected(std::move(rootUri.error().withContext("the workspace root")));
    }
    Json params = Json::object();
    params.set("processId", options.processId ? Json(*options.processId) : Json());
    Json clientInfo = Json::object().set("name", options.clientName);
    if (!options.clientVersion.empty()) {
        clientInfo.set("version", options.clientVersion);
    }
    params.set("clientInfo", std::move(clientInfo));
    params.set("locale", "en");
    params.set("rootPath", options.rootPath);
    params.set("rootUri", *rootUri);
    Json capabilities = clientCapabilities();
    if (options.utf16Only) {
        // Every LSP server must support UTF-16, so offering only it is always a valid offer.
        capabilities.set("general", Json::object().set("positionEncodings", Json::array({Json("utf-16")})));
    }
    params.set("capabilities", std::move(capabilities));
    if (!options.initializationOptions.isNull()) {
        params.set("initializationOptions", options.initializationOptions);
    }
    params.set("trace", "off");
    params.set("workspaceFolders",
               Json::array({Json::object().set("uri", *rootUri).set("name", lastPathComponent(options.rootPath))}));
    return params;
}

struct LspClient::Shared {
    InitializeOptions options;
    Handlers handlers;
    sched::IScheduler* scheduler = nullptr;

    mutable Mutex mutex;
    bool ready FROST_GUARDED_BY(mutex) = false;
    bool everReady FROST_GUARDED_BY(mutex) = false;
    ApplyEditHandler applyEditHandler FROST_GUARDED_BY(mutex);
    ServerCapabilities capabilities; // Written once before `ready` is set, read-only afterwards.

    std::expected<Json, ResponseError> answer(std::string_view method, const Json& params) const {
        if (method == "workspace/configuration") {
            Json results = Json::array();
            if (const Json* items = params.find("items"); items != nullptr && items->isArray()) {
                for (const Json& item : items->items()) {
                    results.push(options.configuration ? options.configuration(item.stringAt("section")) : Json());
                }
            }
            return results;
        }
        if (method == "window/workDoneProgress/create" || method == "client/registerCapability" ||
            method == "client/unregisterCapability" || method == "window/showMessageRequest" ||
            (method.starts_with("workspace/") && method.ends_with("/refresh"))) {
            return Json();
        }
        if (method == "workspace/applyEdit") {
            ApplyEditHandler handler;
            {
                LockGuard lock(mutex);
                handler = applyEditHandler;
            }
            if (handler && handler(params)) {
                return Json::object().set("applied", true);
            }
            return Json::object().set("applied", false).set("failureReason", "FrostIDE does not apply edits yet");
        }
        return std::unexpected(ResponseError{
            .code = rpc_error::kMethodNotFound, .message = std::format("unhandled method {}", method), .data = {}});
    }

    void notification(std::string_view method, const Json& params) const {
        if (method == "textDocument/publishDiagnostics") {
            if (handlers.onDiagnostics) {
                handlers.onDiagnostics(params);
            }
        } else if (method == "window/logMessage" || method == "window/showMessage") {
            if (handlers.onMessage) {
                handlers.onMessage(static_cast<int>(params.integerAt("type").value_or(4)), params.stringAt("message"));
            }
        } else if (handlers.onNotification) {
            handlers.onNotification(method, params);
        }
    }

    [[nodiscard]] bool isReady() const {
        LockGuard lock(mutex);
        return ready;
    }
};

LspClient::LspClient(std::unique_ptr<IByteStream> stream, sched::IScheduler& scheduler, InitializeOptions options,
                     Handlers handlers)
    : m_shared(std::make_shared<Shared>()) {
    m_shared->options = std::move(options);
    m_shared->applyEditHandler = handlers.onApplyEdit;
    m_shared->handlers = std::move(handlers);
    m_shared->scheduler = &scheduler;
    ConnectionHandlers connection;
    connection.onRequest = [shared = m_shared](std::string_view method, const Json& params) {
        return shared->answer(method, params);
    };
    connection.onNotification = [shared = m_shared](std::string_view method, const Json& params) {
        shared->notification(method, params);
    };
    connection.onProtocolError = [shared = m_shared](const Error& problem) {
        if (shared->handlers.onProtocolError) {
            shared->handlers.onProtocolError(problem);
        }
    };
    connection.onClosed = [shared = m_shared](const Error& why) {
        {
            LockGuard lock(shared->mutex);
            shared->ready = false;
        }
        if (shared->handlers.onClosed) {
            shared->handlers.onClosed(why);
        }
    };
    m_connection = std::make_unique<JsonRpcConnection>(std::move(stream), scheduler, std::move(connection));
}

LspClient::~LspClient() { close(); }

sched::Task<Result<ServerCapabilities>> LspClient::initialize(std::stop_token token) {
    auto params = initializeParams(m_shared->options);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    auto result = co_await m_connection->request("initialize", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    if (!result->isObject()) {
        co_return fail(ErrorCode::Corrupt, "initialize: the result is not an object");
    }
    /*
     * A server must select one of the offered general.positionEncodings (LSP 3.17) in
     * capabilities.positionEncoding, or omit it for UTF-16. A legacy offsetEncoding answer (clangd's
     * deprecated extension, which FrostIDE no longer offers) must also name an offered encoding and
     * agree with positionEncoding. In particular, never silently interpret LuaLS UTF-8 positions as
     * UTF-16 after restricting its offer.
     */
    const Json* serverCapabilities = result->find("capabilities");
    std::optional<PositionEncoding> selected;
    const auto acceptEncoding = [&](std::string_view name) {
        if (name.empty())
            return true;
        const auto encoding = parsePositionEncoding(name);
        if (!encoding || (m_shared->options.utf16Only && *encoding != PositionEncoding::Utf16) ||
            (selected && *selected != *encoding))
            return false;
        selected = *encoding;
        return true;
    };
    if (!acceptEncoding(serverCapabilities ? serverCapabilities->stringAt("positionEncoding") : "") ||
        !acceptEncoding(result->stringAt("offsetEncoding")) ||
        !acceptEncoding(serverCapabilities ? serverCapabilities->stringAt("offsetEncoding") : "")) {
        co_return fail(ErrorCode::Corrupt,
                       "initialize: server selected an unadvertised or conflicting position encoding");
    }
    m_shared->capabilities = parseInitializeResult(*result);
    if (auto sent = m_connection->notify("initialized", Json::object()); !sent) {
        co_return std::unexpected(std::move(sent.error()));
    }
    {
        LockGuard lock(m_shared->mutex);
        m_shared->ready = true;
        m_shared->everReady = true;
    }
    co_return m_shared->capabilities;
}

bool LspClient::ready() const noexcept { return m_shared->isReady(); }

const ServerCapabilities& LspClient::capabilities() const noexcept {
    {
        LockGuard lock(m_shared->mutex);
        FROST_PRE(m_shared->everReady);
    }
    return m_shared->capabilities;
}

PositionEncoding LspClient::positionEncoding() const noexcept { return capabilities().positionEncoding; }

PositionEncoding LspClient::encoding() const {
    return ready() ? m_shared->capabilities.positionEncoding : PositionEncoding::Utf16;
}

std::optional<ServerCapabilities> LspClient::serverCapabilities() const {
    return ready() ? std::optional<ServerCapabilities>(m_shared->capabilities) : std::nullopt;
}

void LspClient::setApplyEditHandler(ApplyEditHandler handler) {
    LockGuard lock(m_shared->mutex);
    m_shared->applyEditHandler = std::move(handler);
}

sched::Task<Result<void>> LspClient::shutdown(IClock::Duration timeout) {
    detail::Deadline deadline(*m_shared->scheduler, timeout);
    auto answered = co_await m_connection->request("shutdown", Json(), deadline.token());
    const bool timedOut = deadline.expired();
    deadline.disarm();
    {
        LockGuard lock(m_shared->mutex);
        m_shared->ready = false;
    }
    static_cast<void>(m_connection->notify("exit", Json()));
    m_connection->close();
    if (!answered) {
        if (timedOut) {
            co_return fail(ErrorCode::Timeout, "shutdown: no answer in time");
        }
        co_return std::unexpected(std::move(answered.error()));
    }
    co_return Result<void>{};
}

sched::Task<Result<Json>> LspClient::request(std::string method, Json params, std::stop_token token) {
    if (!m_shared->isReady()) {
        co_return fail(ErrorCode::Busy, method + ": the server is not initialized");
    }
    co_return co_await m_connection->request(std::move(method), std::move(params), std::move(token));
}

Result<void> LspClient::notify(std::string method, Json params) {
    if (!m_shared->isReady()) {
        return fail(ErrorCode::Busy, method + ": the server is not initialized");
    }
    return m_connection->notify(std::move(method), std::move(params));
}

Result<void> LspClient::didOpen(std::string_view uri, std::string_view languageId, std::int64_t version,
                                std::string_view text) {
    return notify(
        "textDocument/didOpen",
        Json::object().set(
            "textDocument",
            Json::object().set("uri", uri).set("languageId", languageId).set("version", version).set("text", text)));
}

Result<void> LspClient::didChange(std::string_view uri, std::int64_t version, std::span<const ContentChange> changes) {
    Json list = Json::array();
    for (const ContentChange& change : changes) {
        list.push(toJson(change));
    }
    return notify("textDocument/didChange",
                  Json::object()
                      .set("textDocument", Json::object().set("uri", uri).set("version", version))
                      .set("contentChanges", std::move(list)));
}

Result<void> LspClient::didSave(std::string_view uri, std::optional<std::string_view> text) {
    Json params = Json::object().set("textDocument", Json::object().set("uri", uri));
    if (text && m_shared->capabilities.saveIncludeText) {
        params.set("text", *text);
    }
    return notify("textDocument/didSave", std::move(params));
}

Result<void> LspClient::didClose(std::string_view uri) {
    return notify("textDocument/didClose", Json::object().set("textDocument", Json::object().set("uri", uri)));
}

void LspClient::close() noexcept {
    {
        LockGuard lock(m_shared->mutex);
        m_shared->ready = false;
    }
    m_connection->close();
}

} // namespace frost::lang
