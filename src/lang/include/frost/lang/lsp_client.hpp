// SPDX-License-Identifier: Apache-2.0
//
// LspClient: the Language Server Protocol 3.17 client over a JsonRpcConnection, written from the
// specification: the initialize / initialized / shutdown / exit lifecycle, capability negotiation
// (LSP 3.17 general.positionEncodings preferring "utf-8"; UTF-16 when the server names none), the
// text-document notifications, and answers to the server requests a client must handle.
/*
 * Lifecycle (LSP 3.17 "Lifecycle Messages"): initialize() sends `initialize` and, on success,
 * `initialized`; until then no other message may be sent, so the document notifications fail
 * with Busy. shutdown() sends `shutdown`, then `exit`, then closes the connection.
 *
 * Server requests answered here: workspace/configuration (one answer per item through
 * InitializeOptions::configuration, null for unknown sections), window/workDoneProgress/create,
 * client/registerCapability, client/unregisterCapability and the workspace refresh requests
 * ("workspace/codeLens/refresh" and the like; null results),
 * window/showMessageRequest (null: no action chosen) and workspace/applyEdit ({applied: false}
 * until the editor integration exists). Everything else gets MethodNotFound.
 *
 * Requests for later slices (completion, definition, hover, ...) go through request(): a
 * Task-returning call with a stop_token, so a newer keystroke can cancel an older completion
 * through $/cancelRequest (PLAN.md 3.4 "Completion").
 *
 * Threads: as JsonRpcConnection (jsonrpc.hpp). Every member is thread-safe; the handlers run on
 * the connection's reader thread, one at a time.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/jsonrpc.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

enum class TextDocumentSyncKind : std::uint8_t { None = 0, Full = 1, Incremental = 2 };

// What the server said it can do (InitializeResult), with the fields W2 and the next slice use;
// `raw` keeps the whole capabilities object for features added later.
struct ServerCapabilities {
    PositionEncoding positionEncoding = PositionEncoding::Utf16; // LSP's default when nothing was negotiated.
    TextDocumentSyncKind sync = TextDocumentSyncKind::Full;
    bool openClose = true;
    bool save = false; // Wants didSave.
    bool saveIncludeText = false;
    bool completion = false;
    bool definition = false;
    bool declaration = false;
    bool hover = false;
    bool references = false;
    bool documentSymbol = false;
    bool signatureHelp = false;
    // The navigation and completion features (W2, ADR 0041).
    bool completionResolve = false; // completionProvider.resolveProvider.
    std::vector<std::string> completionTriggerCharacters;
    std::vector<std::string> signatureHelpTriggerCharacters;
    bool implementation = false;
    bool typeDefinition = false;
    bool documentHighlight = false;
    bool workspaceSymbol = false;
    bool rename = false;
    bool prepareRename = false; // renameProvider.prepareProvider.
    bool codeAction = false;
    bool formatting = false;
    bool rangeFormatting = false;
    bool callHierarchy = false;
    bool typeHierarchy = false;
    bool inlayHint = false;
    std::string serverName;    // serverInfo.name, e.g. "clangd".
    std::string serverVersion; // serverInfo.version.
    Json raw;
};

/*
 * The request seam of the navigation and completion models (W2, ADR 0041): a request to the
 * language server, the negotiated position encoding and capabilities. ClangdService (the
 * running clangd) and LspClient (one connection, used by the tests' scripted server) implement
 * it. Implementations are thread-safe.
 */
class ILspRequester {
public:
    using ApplyEditHandler = std::function<bool(const Json& params)>;

    virtual ~ILspRequester() = default;
    // Sends `method` with `params`; stopping `token` cancels it ($/cancelRequest). Errors: Busy
    // (no initialized server) and those of JsonRpcConnection::request.
    [[nodiscard]] virtual sched::Task<Result<Json>> request(std::string method, Json params,
                                                            std::stop_token token = {}) = 0;
    // The encoding of every LSP position the server sends and expects (UTF-16 until initialized).
    [[nodiscard]] virtual PositionEncoding encoding() const = 0;
    // The server's capabilities while initialized, else nullopt.
    [[nodiscard]] virtual std::optional<ServerCapabilities> serverCapabilities() const = 0;
    // Optional callback invoked when the server sends a `workspace/applyEdit` request.
    virtual void setApplyEditHandler(ApplyEditHandler handler) { static_cast<void>(handler); }

protected:
    ILspRequester() = default;
    ILspRequester(const ILspRequester&) = default;
    ILspRequester& operator=(const ILspRequester&) = default;
};

using LspRequestSender = ILspRequester;

// Reads an InitializeResult. `capabilities.positionEncoding` (LSP 3.17) wins; a server that omits it
// but still answers with clangd's deprecated `offsetEncoding` gets that encoding; otherwise UTF-16,
// the LSP default. Unknown or "utf-32" encodings fall back to UTF-16 (initialize() rejects them).
[[nodiscard]] ServerCapabilities parseInitializeResult(const Json& result);

struct InitializeOptions {
    std::string rootPath;                  // Absolute; sent as rootUri and the one workspace folder.
    std::optional<std::int64_t> processId; // This process's id, so the server can exit if we die.
    std::string clientName = "FrostIDE";
    std::string clientVersion;
    bool utf16Only = false;     // Work around servers that mishandle UTF-8 incremental positions.
    Json initializationOptions; // Server-specific (clangd: fallbackFlags, clangdFileStatus).
    // Answers workspace/configuration items by section; may be empty (every answer is null).
    std::function<Json(std::string_view section)> configuration;
};

// The capabilities FrostIDE announces: synchronization with didSave, publishDiagnostics with
// versionSupport, relatedInformation and codeDescription, general.positionEncodings ["utf-8",
// "utf-16"] (LSP 3.17 only; clangd's deprecated offsetEncoding is not offered), workDoneProgress,
// workspace configuration and folders, and semanticTokens (full and delta) with clangd's inactiveRegionsCapabilities
// (ADR 0047).
[[nodiscard]] Json clientCapabilities();

// The initialize params for `options`. Errors: InvalidArgument (rootPath not absolute).
[[nodiscard]] Result<Json> initializeParams(const InitializeOptions& options);

class LspClient final : public ILspRequester {
public:
    struct Handlers {
        // A server notification other than the ones below (method, params).
        std::function<void(std::string_view method, const Json& params)> onNotification;
        // textDocument/publishDiagnostics params.
        std::function<void(const Json& params)> onDiagnostics;
        // window/logMessage and window/showMessage: LSP MessageType (1 error .. 4 log) and text.
        std::function<void(int type, std::string_view message)> onMessage;
        // Server-initiated workspace/applyEdit request; returns true when applied.
        ApplyEditHandler onApplyEdit;
        std::function<void(const Error& problem)> onProtocolError;
        std::function<void(const Error& why)> onClosed;
    };

    // Starts the connection over `stream` on `scheduler` (see jsonrpc.hpp for the threads).
    LspClient(std::unique_ptr<IByteStream> stream, sched::IScheduler& scheduler, InitializeOptions options,
              Handlers handlers);
    ~LspClient();
    LspClient(const LspClient&) = delete ("a client owns its connection");
    LspClient& operator=(const LspClient&) = delete ("a client owns its connection");

    /*
     * The handshake: initialize, then initialized. Errors: InvalidArgument (options), the
     * connection's errors (IoError when the server dies first, Cancelled), Corrupt (the result is
     * not an object). Call once.
     */
    [[nodiscard]] sched::Task<Result<ServerCapabilities>> initialize(std::stop_token token = {});

    // True once initialize() succeeded and until shutdown().
    [[nodiscard]] bool ready() const noexcept;
    // The negotiated capabilities. Precondition (FROST_PRE): ready() or shut down after it.
    [[nodiscard]] const ServerCapabilities& capabilities() const noexcept;
    [[nodiscard]] PositionEncoding positionEncoding() const noexcept;

    /*
     * shutdown, then exit, then close(), waiting at most `timeout` for the shutdown answer (the
     * scheduler's clock). Errors: Timeout (exit is still sent), IoError. The process's own exit
     * is the owner's to wait for.
     */
    [[nodiscard]] sched::Task<Result<void>> shutdown(IClock::Duration timeout);

    // A request for features beyond W2 (completion, definition, ...). Errors as
    // JsonRpcConnection::request, plus Busy before initialize() finished.
    [[nodiscard]] sched::Task<Result<Json>> request(std::string method, Json params,
                                                    std::stop_token token = {}) override;
    // ILspRequester: positionEncoding() once ready, UTF-16 before.
    [[nodiscard]] PositionEncoding encoding() const override;
    // ILspRequester: capabilities() while ready().
    [[nodiscard]] std::optional<ServerCapabilities> serverCapabilities() const override;
    // Sets or replaces the handler for server-initiated workspace/applyEdit requests.
    void setApplyEditHandler(ApplyEditHandler handler) override;
    // A notification. Errors: Busy (not initialized), IoError (closed).
    [[nodiscard]] Result<void> notify(std::string method, Json params);

    // Text-document notifications (never wait on the server). Errors: as notify().
    [[nodiscard]] Result<void> didOpen(std::string_view uri, std::string_view languageId, std::int64_t version,
                                       std::string_view text);
    [[nodiscard]] Result<void> didChange(std::string_view uri, std::int64_t version,
                                         std::span<const ContentChange> changes);
    // `text` is sent only when the server asked for it (save.includeText).
    [[nodiscard]] Result<void> didSave(std::string_view uri, std::optional<std::string_view> text = std::nullopt);
    [[nodiscard]] Result<void> didClose(std::string_view uri);

    // Closes the connection without the shutdown handshake (the server sees EOF on stdin).
    void close() noexcept;
    [[nodiscard]] JsonRpcConnection& connection() noexcept { return *m_connection; }

private:
    struct Shared;
    std::shared_ptr<Shared> m_shared;
    std::unique_ptr<JsonRpcConnection> m_connection;
};

} // namespace frost::lang
