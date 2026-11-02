// SPDX-License-Identifier: Apache-2.0
//
// The DAP client (PLAN.md M4, ADR 0050): IDapTransport, the byte stream to a debug adapter (its
// stdio, or the fake-dap double), and IDebugAdapterClient, which speaks DAP 1.71's base protocol over
// it: Content-Length framed JSON messages with `seq` numbers, requests matched to their responses by
// `request_seq`, events, the adapter's reverse requests (runInTerminal, startDebugging) answered or
// declined, the cancel request, and the end of the stream (the adapter exited or crashed).
/*
 * Threads. makeDapClient runs two threads: a reader, which blocks in IDapTransport::read, decodes
 * frames and dispatches them, and a writer, which writes queued frames in the order request() and
 * the reverse-request answers queued them. Every public member is thread-safe. Handlers and
 * completions run on the reader thread, one at a time, in arrival order (except as noted below), so
 * they must be quick; they may call request() and cancel() but must not destroy the client.
 *
 * Completions. Each request's completion runs exactly once: on the reader thread with the response
 * (the body on success; dap::responseError on failure), on the thread calling cancel() with
 * Cancelled, or with IoError when the stream ends first (on the reader thread) or when request() is
 * called after the client closed (inline, before request() returns). Callers must therefore not hold
 * a lock the completion takes while they call request().
 *
 * Robustness. A malformed frame is reported through onProtocolError and the decoder resynchronises;
 * a frame that is not a JSON object with a known `type` is reported and dropped, as is a response
 * whose request_seq is unknown (late answers to cancelled requests are dropped silently).
 *
 * Lifetime. close() ends the transport, which makes the adapter exit (the process transport ends
 * its whole tree); pending requests complete with IoError and onClosed runs once. The destructor
 * closes and joins both threads; it must not run on the reader thread (from a handler).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/rpc/json.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace frost::io {
class IProcess;
} // namespace frost::io

namespace frost::debug {

/*
 * A blocking byte stream to a debug adapter. read() is called from one thread only (the client's
 * reader), write() from one other thread only (the writer); close() from any thread, any number of
 * times. Owned through std::unique_ptr, hence the public virtual destructor.
 */
class IDapTransport {
public:
    virtual ~IDapTransport() = default;
    IDapTransport(const IDapTransport&) = delete ("a transport has one owner");
    IDapTransport& operator=(const IDapTransport&) = delete ("a transport has one owner");

    // Reads at least one byte into `buffer`, blocking; 0 means the adapter closed its end (EOF).
    // Errors: IoError.
    [[nodiscard]] virtual Result<std::size_t> read(std::span<char> buffer) = 0;
    // Writes all of `bytes`, blocking. Errors: IoError (the adapter is gone).
    [[nodiscard]] virtual Result<void> write(std::string_view bytes) = 0;
    // Ends the stream: a blocked read returns EOF and the adapter is made to exit. Idempotent.
    virtual void close() noexcept = 0;
    // The adapter's exit code once read() returned EOF, when the transport knows it (a process).
    [[nodiscard]] virtual std::optional<int> exitCode() noexcept { return std::nullopt; }

protected:
    IDapTransport() = default;
};

/*
 * A transport over a debug adapter process started with stdin, stdout and stderr as pipes (stderr
 * is drained on a thread of its own into frost::log, category "debug"). close() closes stdin and
 * terminates the process's container (its whole tree: the debuggee too). Precondition: `process`
 * has those three pipes.
 */
[[nodiscard]] std::unique_ptr<IDapTransport> processTransport(std::unique_ptr<io::IProcess> process);

// What the adapter sends that the client hands to its owner. Each callback may be empty.
struct DapHandlers {
    // An event ("stopped", "output", "initialized", ...) with its body (Null when it has none).
    std::function<void(std::string_view event, const rpc::Json& body)> onEvent{};
    /*
     * A reverse request (DAP 1.71: runInTerminal, startDebugging). The returned body is sent as a
     * successful response, an Error as a failed one with its message. Without a handler every
     * reverse request is declined ("<command> is not supported by FrostIDE").
     */
    std::function<Result<rpc::Json>(std::string_view command, const rpc::Json& arguments)> onReverseRequest{};
    // A protocol problem (a malformed frame or message, an unknown response), for the log.
    std::function<void(const Error& problem)> onProtocolError{};
    // The stream ended; runs once, after every pending request completed. `why` is IoError (EOF,
    // with the adapter's exit code when known, or a read or write error) or Cancelled (close()).
    std::function<void(const Error& why)> onClosed{};
};

/*
 * One connection to a debug adapter (header comment for threads and completions). The interface lets
 * DebugSession run over any implementation; makeDapClient is the real one.
 */
class IDebugAdapterClient {
public:
    // The response body on success, or the failed response's error (dap::responseError).
    using Completion = std::function<void(Result<rpc::Json> body)>;

    virtual ~IDebugAdapterClient() = default;
    IDebugAdapterClient(const IDebugAdapterClient&) = delete ("a client owns its connection");
    IDebugAdapterClient& operator=(const IDebugAdapterClient&) = delete ("a client owns its connection");

    // Sends request `command` with `arguments` (Null: none) and returns its seq (1, 2, 3, ...); `done`
    // runs once (header comment). After close(), returns 0 and runs `done` inline with IoError.
    virtual std::int64_t request(std::string command, rpc::Json arguments, Completion done) = 0;
    /*
     * Cancels request `seq`: its completion runs at once with Cancelled, a DAP cancel request is sent
     * when the adapter supports it (setSupportsCancel), and the late response is dropped. Unknown or
     * finished requests are ignored.
     */
    virtual void cancel(std::int64_t seq) = 0;
    // Whether the adapter takes the cancel request (its supportsCancelRequest capability).
    virtual void setSupportsCancel(bool supported) noexcept = 0;
    // Ends the connection (header comment, "Lifetime"). Idempotent; never blocks on the adapter.
    virtual void close() noexcept = 0;
    // True once the stream ended or close() was called.
    [[nodiscard]] virtual bool closed() const noexcept = 0;
    // Requests still waiting for their response.
    [[nodiscard]] virtual std::size_t pendingRequests() const = 0;

protected:
    IDebugAdapterClient() = default;
};

// The client over `transport`, reading and writing on two threads of its own (header comment).
[[nodiscard]] std::unique_ptr<IDebugAdapterClient> makeDapClient(std::unique_ptr<IDapTransport> transport,
                                                                 DapHandlers handlers);

} // namespace frost::debug
