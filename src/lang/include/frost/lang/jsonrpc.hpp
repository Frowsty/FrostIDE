// SPDX-License-Identifier: Apache-2.0
//
// JSON-RPC 2.0 over a byte stream with LSP's base protocol framing (LSP 3.17, "Base Protocol":
// "Content-Length: N\r\n\r\n" + N bytes of UTF-8 JSON), written from the specification: the frame
// codec (frost::rpc's, re-exported here), the byte-stream seam (IByteStream; a child's stdio through processByteStream)
// and JsonRpcConnection, which matches responses to requests, cancels with $/cancelRequest, delivers notifications and
// answers the server's own requests.
/*
 * Threads (PLAN.md section 3.3). A connection runs one reader coroutine and at most one writer
 * coroutine on the IScheduler it is given (a ThreadPool in production; the reactor resumes them
 * after pipe I/O). Every public member is thread-safe. Handlers (ConnectionHandlers) run on the
 * reader's thread, one at a time, in arrival order, so they must be quick and must not block on
 * the connection (call request() from a handler and co_await it elsewhere, never syncWait).
 *
 * Ordering. Outgoing messages are written in the order send calls return (one FIFO queue, one
 * writer at a time), so a didChange queued after a didOpen reaches the server after it.
 *
 * Robustness. A malformed frame header is reported through onProtocolError and the decoder
 * re-synchronises at the next "Content-Length:" header; a frame whose body is not JSON, not an
 * object, or not a JSON-RPC message is reported and dropped; a response with an unknown id is
 * reported and dropped (late responses to cancelled requests are dropped silently). When the
 * stream ends (EOF, a read error, close()), every pending request completes with IoError and
 * onClosed runs once. Server requests may use onAsyncRequest to defer a response without holding
 * the reader thread; its completion can run on any thread and is ignored after close/destruction.
 *
 * Lifetime. The connection's state is shared with its coroutines, so destroying the connection
 * never blocks and never leaves a coroutine pointing at freed memory: the destructor closes the
 * connection, and the coroutines finish on the scheduler. The scheduler must outlive them (shut it
 * down after the reactor, as async_process.hpp prescribes).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"
#include "frost/rpc/framing.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace frost::io {
class IAsyncProcess;
} // namespace frost::io

namespace frost::lang {

// --- Framing ------------------------------------------------------------------------------------

// The Content-Length framing is shared with the DAP client and lives in frost::rpc (ADR 0050).
using rpc::encodeFrame;
using rpc::FrameDecoder;

// --- Byte streams -------------------------------------------------------------------------------

/*
 * The connection's transport: a bidirectional byte stream (a language server's stdout and stdin).
 * One read and one write may be outstanding at a time (JsonRpcConnection guarantees that); a
 * destroyed stream ends its pending operations with Cancelled.
 */
class IByteStream {
public:
    virtual ~IByteStream() = default;
    IByteStream(const IByteStream&) = delete ("a stream has one owner");
    IByteStream& operator=(const IByteStream&) = delete ("a stream has one owner");

    // Reads at least one byte into `buffer`; 0 means EOF. Errors: Cancelled, IoError.
    [[nodiscard]] virtual sched::Task<Result<std::size_t>> read(std::span<char> buffer, std::stop_token token) = 0;
    // Writes all of `bytes`. Errors: Cancelled, IoError (the peer closed its end or exited).
    [[nodiscard]] virtual sched::Task<Result<void>> write(std::string bytes, std::stop_token token) = 0;
    // Closes the write direction, so the peer reads EOF. Idempotent; any thread.
    virtual void closeWrite() noexcept = 0;

protected:
    IByteStream() = default;
};

// A stream over a child's stdout (read) and stdin (write); it shares ownership of the process.
// Precondition: both pipes were configured as Stdio::Pipe.
[[nodiscard]] std::unique_ptr<IByteStream> processByteStream(std::shared_ptr<io::IAsyncProcess> process);

// --- Messages -----------------------------------------------------------------------------------

// JSON-RPC and LSP error codes (LSP 3.17 "ErrorCodes" and "LSPErrorCodes").
namespace rpc_error {
inline constexpr std::int64_t kParseError = -32700;
inline constexpr std::int64_t kInvalidRequest = -32600;
inline constexpr std::int64_t kMethodNotFound = -32601;
inline constexpr std::int64_t kInvalidParams = -32602;
inline constexpr std::int64_t kInternalError = -32603;
inline constexpr std::int64_t kServerNotInitialized = -32002;
inline constexpr std::int64_t kRequestFailed = -32803;
inline constexpr std::int64_t kServerCancelled = -32802;
inline constexpr std::int64_t kContentModified = -32801;
inline constexpr std::int64_t kRequestCancelled = -32800;
} // namespace rpc_error

// The error member of a response.
struct ResponseError {
    std::int64_t code = rpc_error::kInternalError;
    std::string message;
    Json data;
};

/*
 * How a server's error response becomes a frost::Error: kRequestCancelled -> Cancelled;
 * kContentModified and kServerCancelled -> Busy (retry with fresh state); kMethodNotFound ->
 * Unsupported; kInvalidParams and kInvalidRequest -> InvalidArgument; everything else -> IoError.
 * The message reads "<method>: server error <code>: <message>".
 */
[[nodiscard]] Error toError(std::string_view method, const ResponseError& error);

// What the server sent that the connection hands to the owner. Each callback may be empty.
struct ConnectionHandlers {
    // A server notification. Reader thread (see the header comment).
    std::function<void(std::string_view method, const Json& params)> onNotification;
    // A server request; the return value is sent as the response. Without a handler, or when it
    // returns kMethodNotFound, the server gets MethodNotFound. Reader thread.
    std::function<std::expected<Json, ResponseError>(std::string_view method, const Json& params)> onRequest;
    // Optional asynchronous server-request handler. Return true when it owns the request and
    // will invoke `done` later; false falls through to onRequest. The callback is safe to retain.
    std::function<bool(std::string_view method, const Json& params,
                       std::function<void(std::expected<Json, ResponseError>)> done)>
        onAsyncRequest;
    // A protocol problem (malformed frame or message, unknown response id), for the log.
    std::function<void(const Error& problem)> onProtocolError;
    // The stream ended; runs once, after every pending request was completed. `why` is IoError
    // (EOF or a read error) or Cancelled (close()).
    std::function<void(const Error& why)> onClosed;
};

class JsonRpcConnection {
public:
    // Starts reading `stream` on `scheduler` at once. `scheduler` must outlive the coroutines (see
    // the header comment).
    JsonRpcConnection(std::unique_ptr<IByteStream> stream, sched::IScheduler& scheduler, ConnectionHandlers handlers);
    // close(); never blocks.
    ~JsonRpcConnection();
    JsonRpcConnection(const JsonRpcConnection&) = delete ("a connection owns its stream");
    JsonRpcConnection& operator=(const JsonRpcConnection&) = delete ("a connection owns its stream");

    /*
     * Sends a request (ids are 1, 2, 3, ... per connection) and completes with its result. When
     * `token` is stopped first, $/cancelRequest is sent and the task completes with Cancelled at
     * once; a late response is dropped. Errors: Cancelled; IoError (closed before or while
     * waiting); the server's error through toError(). Resumes on the awaiter's lane.
     */
    [[nodiscard]] sched::Task<Result<Json>> request(std::string method, Json params, std::stop_token token = {});

    // Callback flavour of request(): `done` runs exactly once, on the thread that completes the
    // request (the reader's scheduler thread for a response; the thread calling cancel() or
    // close(); inline when the connection is already closed). Returns the request id, 0 when closed.
    std::int64_t request(std::string method, Json params, std::function<void(Result<Json>)> done);

    // Cancels request `id` as a stopped token would ($/cancelRequest, then Cancelled). Unknown or
    // finished ids are ignored.
    void cancel(std::int64_t id);

    // Queues a notification. Errors: IoError (the connection is closed). O(size of params).
    [[nodiscard]] Result<void> notify(const std::string& method, Json params);

    // Stops the reader, closes the stream's write side after the queued messages and completes
    // pending requests with IoError. Idempotent; any thread; never blocks.
    void close() noexcept;

    // True once the stream has ended or close() was called.
    [[nodiscard]] bool closed() const noexcept;
    // Requests awaiting a response.
    [[nodiscard]] std::size_t pendingRequests() const;

    struct State; // src/lang/src/jsonrpc_connection.cpp.

private:
    std::shared_ptr<State> m_state;
};

} // namespace frost::lang
