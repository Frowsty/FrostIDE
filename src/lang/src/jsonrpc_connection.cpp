// SPDX-License-Identifier: Apache-2.0
//
// JsonRpcConnection (jsonrpc.hpp): the shared state, the reader and writer coroutines, request
// matching, cancellation and the answers to server requests.

#include "frost/core/mutex.hpp"
#include "frost/lang/jsonrpc.hpp"
#include "frost/sched/lane.hpp"

#include <coroutine>
#include <deque>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace frost::lang {
namespace {

// Late responses to this many cancelled requests are recognised and dropped silently.
constexpr std::size_t kMaxRememberedCancellations = 4096;

// One request awaiting its response: a coroutine waiter or a callback, never both.
struct PendingRequest {
    std::string method;                         // Immutable after creation.
    std::function<void(Result<Json>)> callback; // Immutable after creation; empty for coroutines.
    sched::IScheduler* scheduler = nullptr;     // Immutable after creation.

    Mutex mutex;
    std::optional<Result<Json>> result FROST_GUARDED_BY(mutex);
    std::coroutine_handle<> waiter FROST_GUARDED_BY(mutex);
    sched::Lane lane FROST_GUARDED_BY(mutex) = sched::Lane::UserVisible;

    // Stores the first result and wakes the waiter (or runs the callback). Later calls are ignored.
    void complete(Result<Json> value) {
        std::coroutine_handle<> wake;
        sched::Lane wakeLane = sched::Lane::UserVisible;
        {
            LockGuard lock(mutex);
            if (result.has_value()) {
                return;
            }
            if (callback) {
                result.emplace(Json()); // Marks completion; the value goes to the callback.
            } else {
                result.emplace(std::move(value));
                wake = std::exchange(waiter, {});
                wakeLane = lane;
            }
        }
        if (callback) {
            callback(std::move(value));
        } else if (wake) {
            scheduler->enqueue(wake, wakeLane);
        }
    }

    [[nodiscard]] Result<Json> take() {
        LockGuard lock(mutex);
        return std::move(*result);
    }
};

struct PendingAwaiter {
    PendingRequest& pending;

    [[nodiscard]] bool await_ready() {
        LockGuard lock(pending.mutex);
        return pending.result.has_value();
    }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        LockGuard lock(pending.mutex);
        if (pending.result.has_value()) {
            return false;
        }
        pending.waiter = handle;
        pending.lane = pending.scheduler->currentLane().value_or(sched::Lane::UserVisible);
        return true;
    }
    void await_resume() const noexcept {}
};

[[nodiscard]] std::string requestFrame(std::int64_t id, std::string_view method, Json params) {
    Json message = Json::object().set("jsonrpc", "2.0").set("id", id).set("method", method);
    if (!params.isNull()) {
        message.set("params", std::move(params));
    }
    return encodeFrame(message.dump());
}

[[nodiscard]] std::string notificationFrame(std::string_view method, Json params) {
    Json message = Json::object().set("jsonrpc", "2.0").set("method", method);
    if (!params.isNull()) {
        message.set("params", std::move(params));
    }
    return encodeFrame(message.dump());
}

} // namespace

struct JsonRpcConnection::State : std::enable_shared_from_this<State> {
    State(std::unique_ptr<IByteStream> s, sched::IScheduler& sch, ConnectionHandlers h)
        : stream(std::move(s)), scheduler(sch), handlers(std::move(h)) {}

    std::unique_ptr<IByteStream> stream; // Read by the reader, written by the writer only.
    sched::IScheduler& scheduler;
    ConnectionHandlers handlers; // Immutable; called on the reader's thread.
    std::stop_source readerStop; // close() stops the reader's pending read.
    std::stop_source writerStop; // Never requested: queued messages are written before closeWrite.

    mutable Mutex mutex;
    bool closed FROST_GUARDED_BY(mutex) = false;
    bool writing FROST_GUARDED_BY(mutex) = false;
    bool writeFailed FROST_GUARDED_BY(mutex) = false;
    bool closedReported FROST_GUARDED_BY(mutex) = false;
    std::deque<std::string> outgoing FROST_GUARDED_BY(mutex);
    std::int64_t nextId FROST_GUARDED_BY(mutex) = 1;
    std::unordered_map<std::int64_t, std::shared_ptr<PendingRequest>> pending FROST_GUARDED_BY(mutex);
    std::unordered_set<std::int64_t> cancelled FROST_GUARDED_BY(mutex);

    // Assigns an id and registers `request`; nullopt when the connection is closed.
    std::optional<std::int64_t> registerRequest(const std::shared_ptr<PendingRequest>& request) {
        LockGuard lock(mutex);
        if (closed) {
            return std::nullopt;
        }
        const std::int64_t id = nextId++;
        pending.emplace(id, request);
        return id;
    }

    // Removes a pending request without completing it.
    std::shared_ptr<PendingRequest> unregister(std::int64_t id) {
        LockGuard lock(mutex);
        auto it = pending.find(id);
        if (it == pending.end()) {
            return nullptr;
        }
        std::shared_ptr<PendingRequest> request = std::move(it->second);
        pending.erase(it);
        return request;
    }

    // Queues a frame; false when the connection is closed or a write failed.
    bool send(std::string frame) {
        bool start = false;
        {
            LockGuard lock(mutex);
            if (closed || writeFailed) {
                return false;
            }
            outgoing.push_back(std::move(frame));
            if (!writing) {
                writing = true;
                start = true;
            }
        }
        if (start) {
            scheduler.spawn(sched::Lane::UserVisible, writerLoop(shared_from_this())).detach();
        }
        return true;
    }

    // Sends a response frame even while closing (the reader answers until it stops).
    void sendResponse(Json message) { static_cast<void>(send(encodeFrame(message.dump()))); }

    void cancel(std::int64_t id) {
        std::shared_ptr<PendingRequest> request;
        {
            LockGuard lock(mutex);
            auto it = pending.find(id);
            if (it == pending.end()) {
                return;
            }
            request = std::move(it->second);
            pending.erase(it);
            if (cancelled.size() >= kMaxRememberedCancellations) {
                cancelled.clear();
            }
            cancelled.insert(id);
        }
        static_cast<void>(send(notificationFrame("$/cancelRequest", Json::object().set("id", id))));
        request->complete(fail(ErrorCode::Cancelled, request->method + ": cancelled"));
    }

    // Marks the connection closed and fails every pending request with `why`'s message.
    void closeAndFail(const Error& why) {
        std::vector<std::shared_ptr<PendingRequest>> failed;
        bool closeWriteNow = false;
        {
            LockGuard lock(mutex);
            closed = true;
            closeWriteNow = !writing;
            failed.reserve(pending.size());
            for (auto& [id, request] : pending) {
                failed.push_back(std::move(request));
            }
            pending.clear();
        }
        if (closeWriteNow) {
            stream->closeWrite();
        }
        for (const std::shared_ptr<PendingRequest>& request : failed) {
            request->complete(fail(ErrorCode::IoError,
                                   std::format("{}: the connection closed ({})", request->method, why.message())));
        }
    }

    void protocolError(const Error& problem) const {
        if (handlers.onProtocolError) {
            handlers.onProtocolError(problem);
        }
    }

    void dispatch(std::string_view payload) {
        auto parsed = parseJson(payload);
        if (!parsed) {
            protocolError(Error(ErrorCode::Corrupt, "a message is not JSON: " + parsed.error().message()));
            return;
        }
        if (parsed->isArray()) { // A JSON-RPC batch.
            for (const Json& message : parsed->items()) {
                dispatchMessage(message);
            }
            return;
        }
        dispatchMessage(*parsed);
    }

    void dispatchMessage(const Json& message) {
        if (!message.isObject()) {
            protocolError(Error(ErrorCode::Corrupt, "a message is not a JSON object"));
            return;
        }
        const Json noParams;
        const Json* method = message.find("method");
        const Json* id = message.find("id");
        if (method != nullptr && method->isString()) {
            const Json* params = message.find("params");
            const Json& paramsRef = params != nullptr ? *params : noParams;
            if (id != nullptr && !id->isNull()) {
                answer(*id, *method->asString(), paramsRef);
            } else if (*method->asString() != "$/cancelRequest" && handlers.onNotification) {
                // Server requests are answered synchronously, so a server's $/cancelRequest has nothing to cancel.
                handlers.onNotification(*method->asString(), paramsRef);
            }
            return;
        }
        if (id != nullptr && (message.find("result") != nullptr || message.find("error") != nullptr)) {
            response(*id, message);
            return;
        }
        protocolError(Error(ErrorCode::Corrupt, "a message is neither a request, a notification nor a response"));
    }

    void answer(const Json& id, const std::string& method, const Json& params) {
        std::expected<Json, ResponseError> reply = std::unexpected(
            ResponseError{.code = rpc_error::kMethodNotFound, .message = "method not found: " + method, .data = {}});
        if (handlers.onRequest) {
            reply = handlers.onRequest(method, params);
        }
        Json out = Json::object().set("jsonrpc", "2.0").set("id", id);
        if (reply) {
            out.set("result", std::move(*reply));
        } else {
            Json error = Json::object().set("code", reply.error().code).set("message", reply.error().message);
            if (!reply.error().data.isNull()) {
                error.set("data", std::move(reply.error().data));
            }
            out.set("error", std::move(error));
        }
        sendResponse(std::move(out));
    }

    void response(const Json& idValue, const Json& message) {
        const std::optional<std::int64_t> id = idValue.asInteger();
        if (!id) {
            protocolError(Error(ErrorCode::Corrupt, "a response id is not one of ours: " + idValue.dump()));
            return;
        }
        std::shared_ptr<PendingRequest> request = unregister(*id);
        if (!request) {
            bool wasCancelled = false;
            {
                LockGuard lock(mutex);
                wasCancelled = cancelled.erase(*id) > 0;
            }
            if (!wasCancelled) {
                protocolError(Error(ErrorCode::Corrupt, std::format("a response to unknown request id {}", *id)));
            }
            return;
        }
        if (const Json* error = message.find("error"); error != nullptr && !error->isNull()) {
            ResponseError decoded{.code = error->integerAt("code").value_or(rpc_error::kInternalError),
                                  .message = std::string(error->stringAt("message")),
                                  .data = error->find("data") != nullptr ? *error->find("data") : Json()};
            request->complete(std::unexpected(toError(request->method, decoded)));
            return;
        }
        const Json* result = message.find("result");
        request->complete(result != nullptr ? *result : Json());
    }

    // Called once by the reader when the stream ended.
    void finish(const Error& why) {
        closeAndFail(why);
        bool report = false;
        {
            LockGuard lock(mutex);
            report = !closedReported;
            closedReported = true;
        }
        if (report && handlers.onClosed) {
            handlers.onClosed(why);
        }
    }

    static sched::Task<void> readerLoop(std::shared_ptr<State> self) {
        std::vector<char> buffer(64 * 1024);
        FrameDecoder decoder;
        Error why(ErrorCode::IoError, "the server closed its output");
        for (;;) {
            auto got = co_await self->stream->read(std::span<char>(buffer), self->readerStop.get_token());
            if (!got) {
                why = std::move(got.error());
                break;
            }
            if (*got == 0) {
                break;
            }
            decoder.feed(std::string_view(buffer.data(), *got));
            while (auto frame = decoder.next()) {
                if (!*frame) {
                    self->protocolError(frame->error());
                } else {
                    self->dispatch(**frame);
                }
            }
        }
        self->finish(why);
    }

    static sched::Task<void> writerLoop(std::shared_ptr<State> self) {
        for (;;) {
            std::string frame;
            bool closeWrite = false;
            {
                LockGuard lock(self->mutex);
                if (self->outgoing.empty()) {
                    self->writing = false;
                    closeWrite = self->closed;
                } else {
                    frame = std::move(self->outgoing.front());
                    self->outgoing.pop_front();
                }
            }
            if (frame.empty()) {
                if (closeWrite) {
                    self->stream->closeWrite();
                }
                co_return;
            }
            auto written = co_await self->stream->write(std::move(frame), self->writerStop.get_token());
            if (!written) {
                bool closedNow = false;
                {
                    LockGuard lock(self->mutex);
                    self->writeFailed = true;
                    self->outgoing.clear();
                    self->writing = false;
                    closedNow = self->closed;
                }
                if (!closedNow) {
                    self->protocolError(
                        Error(ErrorCode::IoError, "writing to the server failed: " + written.error().message()));
                }
                co_return;
            }
        }
    }
};

namespace {

struct CancelRequest {
    std::shared_ptr<JsonRpcConnection::State> state;
    std::int64_t id;
    void operator()() const { state->cancel(id); }
};

} // namespace

JsonRpcConnection::JsonRpcConnection(std::unique_ptr<IByteStream> stream, sched::IScheduler& scheduler,
                                     ConnectionHandlers handlers)
    : m_state(std::make_shared<State>(std::move(stream), scheduler, std::move(handlers))) {
    scheduler.spawn(sched::Lane::UserVisible, State::readerLoop(m_state)).detach();
}

JsonRpcConnection::~JsonRpcConnection() { close(); }

sched::Task<Result<Json>> JsonRpcConnection::request(std::string method, Json params, std::stop_token token) {
    std::shared_ptr<State> state = m_state;
    auto pending = std::make_shared<PendingRequest>();
    pending->method = method;
    pending->scheduler = &state->scheduler;
    const std::optional<std::int64_t> id = state->registerRequest(pending);
    if (!id) {
        co_return fail(ErrorCode::IoError, method + ": the connection is closed");
    }
    if (!state->send(requestFrame(*id, method, std::move(params)))) {
        static_cast<void>(state->unregister(*id));
        co_return fail(ErrorCode::IoError, method + ": the connection is closed");
    }
    {
        std::optional<std::stop_callback<CancelRequest>> onStop;
        if (token.stop_possible()) {
            onStop.emplace(token, CancelRequest{state, *id});
        }
        co_await PendingAwaiter{*pending};
    }
    co_return pending->take();
}

std::int64_t JsonRpcConnection::request(std::string method, Json params, std::function<void(Result<Json>)> done) {
    auto pending = std::make_shared<PendingRequest>();
    pending->method = method;
    pending->callback = std::move(done);
    pending->scheduler = &m_state->scheduler;
    const std::optional<std::int64_t> id = m_state->registerRequest(pending);
    if (!id || !m_state->send(requestFrame(*id, method, std::move(params)))) {
        if (id) {
            static_cast<void>(m_state->unregister(*id));
        }
        pending->complete(fail(ErrorCode::IoError, method + ": the connection is closed"));
        return 0;
    }
    return *id;
}

Result<void> JsonRpcConnection::notify(std::string method, Json params) {
    if (!m_state->send(notificationFrame(method, std::move(params)))) {
        return fail(ErrorCode::IoError, method + ": the connection is closed");
    }
    return {};
}

void JsonRpcConnection::cancel(std::int64_t id) { m_state->cancel(id); }

void JsonRpcConnection::close() noexcept {
    m_state->closeAndFail(Error(ErrorCode::Cancelled, "closed by the client"));
    m_state->readerStop.request_stop();
}

bool JsonRpcConnection::closed() const noexcept {
    LockGuard lock(m_state->mutex);
    return m_state->closed;
}

std::size_t JsonRpcConnection::pendingRequests() const {
    LockGuard lock(m_state->mutex);
    return m_state->pending.size();
}

} // namespace frost::lang
