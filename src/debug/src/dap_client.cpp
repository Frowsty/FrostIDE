// SPDX-License-Identifier: Apache-2.0
//
// makeDapClient and processTransport (see frost/debug/dap_client.hpp): the reader and writer threads,
// request matching, reverse requests and cancellation, and the transport over an adapter's stdio.

#include "frost/debug/dap_client.hpp"

#include "frost/core/assert.hpp"
#include "frost/core/log.hpp"
#include "frost/core/mutex.hpp"
#include "frost/debug/dap_types.hpp"
#include "frost/io/loopback_stream.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/framing.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace frost::debug {
namespace {

constexpr std::size_t kReadChunk = 64 * 1024;
// How long the reader waits for the adapter's exit code after EOF (it normally exits at once).
constexpr auto kExitCodeWait = std::chrono::seconds(2);

// --- The process transport ---

class ProcessTransport final : public IDapTransport {
public:
    explicit ProcessTransport(std::unique_ptr<io::IProcess> process) : m_process(std::move(process)) {
        io::IProcess* raw = m_process.get();
        m_stderr = std::thread([raw] {
            std::array<char, 4096> buffer{};
            std::string line;
            for (;;) {
                auto read = raw->readStderr(buffer);
                if (!read || *read == 0) {
                    break;
                }
                line.append(buffer.data(), *read);
                for (std::size_t end = line.find('\n'); end != std::string::npos; end = line.find('\n')) {
                    std::string_view text(line.data(), end);
                    if (text.ends_with('\r')) {
                        text.remove_suffix(1);
                    }
                    if (!text.empty()) {
                        log::info("debug", "lldb-dap: {}", text);
                    }
                    line.erase(0, end + 1);
                }
            }
            if (!line.empty()) {
                log::info("debug", "lldb-dap: {}", line);
            }
        });
    }

    ~ProcessTransport() override {
        close();
        if (m_stderr.joinable()) {
            m_stderr.join();
        }
    }
    ProcessTransport(const ProcessTransport&) = delete;
    ProcessTransport& operator=(const ProcessTransport&) = delete;

    Result<std::size_t> read(std::span<char> buffer) override { return m_process->readStdout(buffer); }

    Result<void> write(std::string_view bytes) override { return m_process->writeStdin(bytes); }

    void close() noexcept override {
        if (m_closed.exchange(true)) {
            return;
        }
        m_process->closeStdin();
        // The whole container ends: lldb-dap and the debuggee it started.
        static_cast<void>(m_process->terminate());
        m_process->abandonOutput();
    }

    std::optional<int> exitCode() noexcept override {
        auto status = m_process->wait(kExitCodeWait);
        if (!status) {
            return std::nullopt;
        }
        return status->exitCode;
    }

private:
    std::unique_ptr<io::IProcess> m_process;
    std::thread m_stderr;
    std::atomic<bool> m_closed{false};
};

class LoopbackTransport final : public IDapTransport {
public:
    explicit LoopbackTransport(std::unique_ptr<io::LoopbackStream> stream) : m_stream(std::move(stream)) {}
    Result<std::size_t> read(std::span<char> buffer) override {
        auto result = m_stream->read(buffer);
        if (!result && m_closed.load())
            return std::size_t{0};
        return result;
    }
    Result<void> write(std::string_view bytes) override { return m_stream->write(bytes); }
    void close() noexcept override {
        m_closed.store(true);
        m_stream->close();
    }

private:
    std::unique_ptr<io::LoopbackStream> m_stream;
    std::atomic<bool> m_closed{false};
};

// --- The client ---

struct ClientState {
    struct Pending {
        std::string command;
        IDebugAdapterClient::Completion done;
    };

    ClientState(std::unique_ptr<IDapTransport> stream, DapHandlers callbacks)
        : transport(std::move(stream)), handlers(std::move(callbacks)) {}

    std::unique_ptr<IDapTransport> transport;
    const DapHandlers handlers;

    mutable Mutex mutex;
    std::int64_t nextSeq FROST_GUARDED_BY(mutex) = 1;
    std::map<std::int64_t, Pending> pending FROST_GUARDED_BY(mutex);
    std::set<std::int64_t> cancelled FROST_GUARDED_BY(mutex); // Their late responses are dropped.
    bool closed FROST_GUARDED_BY(mutex) = false;
    std::atomic<bool> closeRequested{false};
    std::atomic<bool> supportsCancel{false};

    Mutex queueMutex;
    std::condition_variable queueChanged;
    std::deque<std::string> outgoing FROST_GUARDED_BY(queueMutex);
    bool writerStop FROST_GUARDED_BY(queueMutex) = false;

    std::thread reader;
    std::thread writer;

    void report(const Error& problem) const {
        log::warn("debug", "DAP: {}", problem.message());
        if (handlers.onProtocolError) {
            handlers.onProtocolError(problem);
        }
    }

    // Queues one message (the seq is filled in here, in queue order). Returns the seq, 0 when closed.
    std::int64_t send(rpc::Json message,
                      std::optional<std::pair<std::string, IDebugAdapterClient::Completion>> request) {
        std::int64_t seq = 0;
        {
            LockGuard lock(mutex);
            if (closed) {
                return 0;
            }
            seq = nextSeq++;
            message.set("seq", seq);
            if (request) {
                pending.emplace(seq, Pending{std::move(request->first), std::move(request->second)});
            }
            // Queued under `mutex` too, so the frames reach the adapter in seq order.
            LockGuard queue(queueMutex);
            outgoing.push_back(rpc::encodeFrame(message.dump()));
        }
        queueChanged.notify_all();
        return seq;
    }

    void writerLoop() {
        for (;;) {
            std::string frame;
            {
                UniqueLock lock(queueMutex);
                while (outgoing.empty() && !writerStop) {
                    queueChanged.wait(lock.native());
                }
                if (writerStop) {
                    return;
                }
                frame = std::move(outgoing.front());
                outgoing.pop_front();
            }
            if (auto written = transport->write(frame); !written) {
                log::warn("debug", "DAP: writing to the adapter failed: {}", written.error().message());
                transport->close(); // The reader sees EOF and ends the connection.
                return;
            }
        }
    }

    void stopWriter() {
        {
            LockGuard lock(queueMutex);
            writerStop = true;
            outgoing.clear();
        }
        queueChanged.notify_all();
    }

    void dispatchResponse(const rpc::Json& message) {
        const std::optional<std::int64_t> requestSeq = message.integerAt("request_seq");
        Pending entry;
        {
            LockGuard lock(mutex);
            const auto it = requestSeq ? pending.find(*requestSeq) : pending.end();
            if (it == pending.end()) {
                if (requestSeq && cancelled.erase(*requestSeq) > 0) {
                    return; // The answer to a cancelled request.
                }
                report(
                    Error(ErrorCode::Corrupt, std::format("a response to unknown request {}", requestSeq.value_or(0))));
                return;
            }
            entry = std::move(it->second);
            pending.erase(it);
        }
        const rpc::Json* body = message.find("body");
        const rpc::Json empty;
        if (message.boolAt("success").value_or(false)) {
            entry.done(body != nullptr ? *body : empty);
        } else {
            entry.done(std::unexpected(
                dap::responseError(entry.command, message.stringAt("message"), body != nullptr ? *body : empty)));
        }
    }

    void dispatchReverseRequest(const rpc::Json& message) {
        const std::string command(message.stringAt("command"));
        const rpc::Json* arguments = message.find("arguments");
        const rpc::Json empty;
        Result<rpc::Json> answer =
            handlers.onReverseRequest
                ? handlers.onReverseRequest(command, arguments != nullptr ? *arguments : empty)
                : Result<rpc::Json>(
                      fail(ErrorCode::Unsupported, std::format("{} is not supported by FrostIDE", command)));
        rpc::Json response = rpc::Json::object()
                                 .set("type", "response")
                                 .set("request_seq", message.integerAt("seq").value_or(0))
                                 .set("command", command)
                                 .set("success", answer.has_value());
        if (answer) {
            if (!answer->isNull()) {
                response.set("body", std::move(*answer));
            }
        } else {
            response.set("message", answer.error().message());
        }
        static_cast<void>(send(std::move(response), std::nullopt));
    }

    void dispatch(std::string_view payload) {
        auto parsed = rpc::parseJson(payload);
        if (!parsed || !parsed->isObject()) {
            report(Error(ErrorCode::Corrupt, "a DAP message that is not a JSON object"));
            return;
        }
        const std::string_view type = parsed->stringAt("type");
        if (type == "response") {
            dispatchResponse(*parsed);
        } else if (type == "event") {
            if (handlers.onEvent) {
                const rpc::Json* body = parsed->find("body");
                const rpc::Json empty;
                handlers.onEvent(parsed->stringAt("event"), body != nullptr ? *body : empty);
            }
        } else if (type == "request") {
            dispatchReverseRequest(*parsed);
        } else {
            report(Error(ErrorCode::Corrupt, std::format("a DAP message of unknown type '{}'", type)));
        }
    }

    void readerLoop() {
        rpc::FrameDecoder decoder;
        std::vector<char> buffer(kReadChunk);
        Error why(ErrorCode::IoError, "the debug adapter closed its output");
        for (;;) {
            auto read = transport->read(buffer);
            if (!read) {
                why = Error(ErrorCode::IoError,
                            std::format("reading from the debug adapter failed: {}", read.error().message()));
                break;
            }
            if (*read == 0) {
                break;
            }
            decoder.feed(std::string_view(buffer.data(), *read));
            while (auto frame = decoder.next()) {
                if (!*frame) {
                    report(frame->error());
                    continue;
                }
                dispatch(**frame);
            }
        }
        if (closeRequested.load()) {
            why = Error(ErrorCode::Cancelled, "the debug session closed the connection");
        } else if (const std::optional<int> code = transport->exitCode()) {
            why = Error(ErrorCode::IoError, std::format("the debug adapter exited with code {}", *code));
        }
        finish(why);
    }

    void finish(const Error& why) {
        std::map<std::int64_t, Pending> orphans;
        {
            LockGuard lock(mutex);
            closed = true;
            orphans.swap(pending);
            cancelled.clear();
        }
        stopWriter();
        for (auto& [seq, entry] : orphans) {
            entry.done(fail(ErrorCode::IoError, std::format("{}: the connection ended before the answer ({})",
                                                            entry.command, why.message())));
        }
        if (handlers.onClosed) {
            handlers.onClosed(why);
        }
    }
};

class DapClient final : public IDebugAdapterClient {
public:
    DapClient(std::unique_ptr<IDapTransport> transport, DapHandlers handlers)
        : m_state(std::make_shared<ClientState>(std::move(transport), std::move(handlers))) {
        ClientState* raw = m_state.get();
        m_state->writer = std::thread([raw] { raw->writerLoop(); });
        m_state->reader = std::thread([raw] { raw->readerLoop(); });
    }

    ~DapClient() override {
        close();
        // The reader returns once the transport reports EOF (close() ended the stream).
        if (m_state->reader.joinable()) {
            FROST_ASSERT(m_state->reader.get_id() != std::this_thread::get_id());
            m_state->reader.join();
        }
        m_state->stopWriter();
        if (m_state->writer.joinable()) {
            m_state->writer.join();
        }
    }
    DapClient(const DapClient&) = delete;
    DapClient& operator=(const DapClient&) = delete;

    std::int64_t request(std::string command, rpc::Json arguments, Completion done) override {
        rpc::Json message = rpc::Json::object().set("type", "request").set("command", command);
        if (!arguments.isNull()) {
            message.set("arguments", std::move(arguments));
        }
        std::string name = command;
        const std::int64_t seq = m_state->send(std::move(message), std::pair{std::move(command), done});
        if (seq == 0) {
            done(fail(ErrorCode::IoError, std::format("{}: the debug adapter connection is closed", name)));
        }
        return seq;
    }

    void cancel(std::int64_t seq) override {
        Completion done;
        std::string command;
        {
            LockGuard lock(m_state->mutex);
            const auto it = m_state->pending.find(seq);
            if (it == m_state->pending.end()) {
                return;
            }
            done = std::move(it->second.done);
            command = std::move(it->second.command);
            m_state->pending.erase(it);
            m_state->cancelled.insert(seq);
        }
        if (m_state->supportsCancel.load()) {
            static_cast<void>(m_state->send(rpc::Json::object()
                                                .set("type", "request")
                                                .set("command", "cancel")
                                                .set("arguments", rpc::Json::object().set("requestId", seq)),
                                            std::pair{std::string("cancel"), Completion([](Result<rpc::Json>) {})}));
        }
        done(fail(ErrorCode::Cancelled, std::format("{} was cancelled", command)));
    }

    void setSupportsCancel(bool supported) noexcept override { m_state->supportsCancel.store(supported); }

    void close() noexcept override {
        m_state->closeRequested.store(true);
        m_state->transport->close();
    }

    bool closed() const noexcept override {
        LockGuard lock(m_state->mutex);
        return m_state->closed;
    }

    std::size_t pendingRequests() const override {
        LockGuard lock(m_state->mutex);
        return m_state->pending.size();
    }

private:
    std::shared_ptr<ClientState> m_state;
};

} // namespace

std::unique_ptr<IDapTransport> loopbackTransport(std::unique_ptr<io::LoopbackStream> stream) {
    return std::make_unique<LoopbackTransport>(std::move(stream));
}

std::unique_ptr<IDapTransport> processTransport(std::unique_ptr<io::IProcess> process) {
    return std::make_unique<ProcessTransport>(std::move(process));
}

std::unique_ptr<IDebugAdapterClient> makeDapClient(std::unique_ptr<IDapTransport> transport, DapHandlers handlers) {
    return std::make_unique<DapClient>(std::move(transport), std::move(handlers));
}

} // namespace frost::debug
