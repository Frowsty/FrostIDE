// SPDX-License-Identifier: Apache-2.0
// Translate the implemented engine Lua debug commands to DAP without owning the engine process.
#include "frost/debug/lua_debug_adapter.hpp"

#include "frost/debug/lua_debug_endpoint.hpp"
#include "frost/io/loopback_stream.hpp"
#include "frost/rpc/framing.hpp"
#include "frost/rpc/json.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace frost::debug {
namespace {

// One DAP writer calls write; one DAP reader calls read. The network reader only creates DAP
// responses and events; all Lua access remains in the engine's owner thread.
class LuaAdapterTransport final : public IDapTransport {
public:
    explicit LuaAdapterTransport(std::unique_ptr<io::LoopbackStream> stream, std::int64_t nextWireRequest = 2,
                                 std::uint64_t playOperation = 0)
        : m_stream(std::move(stream)), m_nextWireRequest(nextWireRequest), m_playOperation(playOperation) {
        m_reader = std::thread([this] { receive(); });
    }
    ~LuaAdapterTransport() override {
        close();
        if (m_reader.joinable())
            m_reader.join();
    }

    Result<std::size_t> read(std::span<char> buffer) override {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [this] { return m_closed || m_networkClosed || !m_output.empty(); });
        if (m_output.empty())
            return std::size_t{0};
        std::string& front = m_output.front();
        const std::size_t count = std::min(buffer.size(), front.size() - m_offset);
        std::copy_n(front.data() + m_offset, count, buffer.data());
        m_offset += count;
        if (m_offset == front.size()) {
            m_queuedBytes -= front.size();
            m_output.pop_front();
            m_offset = 0;
        }
        return count;
    }

    Result<void> write(std::string_view bytes) override {
        m_input.feed(bytes);
        while (auto frame = m_input.next()) {
            if (!*frame)
                return std::unexpected(std::move(frame->error()));
            auto request = rpc::parseJson(**frame);
            if (!request || request->stringAt("type") != "request")
                return fail(ErrorCode::Corrupt, "invalid Lua debug adapter request");
            const auto id = request->integerAt("seq");
            const std::string command(request->stringAt("command"));
            if (!id || *id <= 0 || command.empty())
                return fail(ErrorCode::Corrupt, "invalid Lua debug request identity");
            if (command == "initialize") {
                respond(*id, command, true, rpc::Json::object().set("supportsConfigurationDoneRequest", true));
            } else if (command == "attach") {
                respond(*id, command, true, rpc::Json::object());
                event("initialized", rpc::Json::object());
            } else if (command == "configurationDone") {
                respond(*id, command, true, rpc::Json::object());
            } else if (command == "threads") {
                respond(*id, command, true,
                        rpc::Json::object().set(
                            "threads", rpc::Json::array({rpc::Json::object().set("id", 1).set("name", "Lua")})));
            } else if (command == "pause" || command == "continue" || command == "stackTrace" || command == "source" ||
                       command == "frost.stopPlay") {
                if (command == "frost.stopPlay" && m_playOperation == 0) {
                    respond(*id, command, false, {}, "no owned Lua play operation");
                    continue;
                }
                rpc::Json outgoing =
                    rpc::Json::object().set("command", command == "frost.stopPlay" ? "stopPlay" : command);
                if (command == "frost.stopPlay")
                    outgoing.set("operationId", m_playOperation);
                Pending pending{.command = command, .dapId = *id};
                {
                    std::lock_guard lock(m_mutex);
                    if (m_pending.size() >= 128 || m_closed || m_networkClosed ||
                        m_nextWireRequest == std::numeric_limits<std::int64_t>::max()) {
                        respondLocked(*id, command, false, {}, "Lua control queue is unavailable");
                        continue;
                    }
                    const rpc::Json* arguments = request->find("arguments");
                    if (command == "stackTrace") {
                        pending.stopEpoch = m_stopEpoch;
                        if (!m_stopped) {
                            respondLocked(*id, command, false, {}, "Lua is not stopped");
                            continue;
                        }
                        pending.start = arguments ? arguments->integerAt("startFrame").value_or(0) : 0;
                        pending.count = arguments ? arguments->integerAt("levels").value_or(0) : 0;
                        if (pending.start < 0 || pending.count < 0) {
                            respondLocked(*id, command, false, {}, "invalid stack range");
                            continue;
                        }
                    } else if (command == "source") {
                        pending.stopEpoch = m_stopEpoch;
                        const auto reference = arguments ? arguments->integerAt("sourceReference") : std::nullopt;
                        const auto source = reference ? m_sources.find(*reference) : m_sources.end();
                        if (!m_stopped || source == m_sources.end()) {
                            respondLocked(*id, command, false, {}, "loaded source reference is stale or unavailable");
                            continue;
                        }
                        outgoing.set("sourceId", source->second.id).set("stopGeneration", source->second.stop);
                    }
                    const auto wireId = m_nextWireRequest++;
                    outgoing.set("requestId", wireId);
                    m_pending.emplace(wireId, std::move(pending));
                }
                if (auto sent = m_stream->write(rpc::encodeFrame(outgoing.dump())); !sent)
                    return std::unexpected(std::move(sent.error()));
            } else if (command == "disconnect") {
                respond(*id, command, true, rpc::Json::object());
                m_stream->close();
            } else {
                respond(*id, command, false, {}, "operation is not implemented by the Lua debugger");
            }
        }
        return {};
    }

    void close() noexcept override {
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
        }
        m_stream->close();
        m_changed.notify_all();
    }

private:
    struct Pending {
        std::string command;
        std::int64_t dapId = 0;
        std::int64_t start = 0;
        std::int64_t count = 0;
        std::uint64_t stopEpoch = 0;
    };
    struct SourceReference {
        std::string id;
        std::int64_t stop = 0;
    };
    static constexpr std::size_t MaxOutputBytes = 4 * 1024 * 1024;

    void queueLocked(rpc::Json value) {
        value.set("seq", m_sequence++);
        std::string wire = rpc::encodeFrame(value.dump());
        if (wire.size() > MaxOutputBytes - m_queuedBytes) {
            m_closed = true;
            m_stream->close();
        } else {
            m_queuedBytes += wire.size();
            m_output.push_back(std::move(wire));
        }
        m_changed.notify_all();
    }
    void respondLocked(std::int64_t id, const std::string& command, bool success, rpc::Json body,
                       std::string message = {}) {
        rpc::Json response = rpc::Json::object()
                                 .set("type", "response")
                                 .set("request_seq", id)
                                 .set("command", command)
                                 .set("success", success)
                                 .set("body", std::move(body));
        if (!message.empty())
            response.set("message", std::move(message));
        queueLocked(std::move(response));
    }
    void respond(std::int64_t id, const std::string& command, bool success, rpc::Json body, std::string message = {}) {
        std::lock_guard lock(m_mutex);
        respondLocked(id, command, success, std::move(body), std::move(message));
    }
    void event(std::string name, rpc::Json body) {
        std::lock_guard lock(m_mutex);
        queueLocked(
            rpc::Json::object().set("type", "event").set("event", std::move(name)).set("body", std::move(body)));
    }
    void receive() {
        rpc::FrameDecoder decoder(1024 * 1024);
        std::array<char, 16384> bytes{};
        for (;;) {
            auto count = m_stream->read(bytes);
            if (!count || *count == 0)
                break;
            decoder.feed(std::string_view(bytes.data(), *count));
            bool malformed = false;
            while (auto frame = decoder.next()) {
                if (!*frame) {
                    malformed = true;
                    break;
                }
                auto message = rpc::parseJson(**frame);
                if (!message || !message->isObject()) {
                    malformed = true;
                    break;
                }
                const std::string name(message->stringAt("event"));
                std::lock_guard lock(m_mutex);
                if (const auto requestId = message->integerAt("requestId")) {
                    const auto found = m_pending.find(*requestId);
                    if (found == m_pending.end())
                        continue;
                    const Pending pending = std::move(found->second);
                    m_pending.erase(found);
                    bool success = message->boolAt("success").value_or(false);
                    bool terminalPlay = false;
                    if ((pending.command == "stackTrace" || pending.command == "source") &&
                        (!m_stopped || pending.stopEpoch != m_stopEpoch))
                        success = false;
                    rpc::Json body = rpc::Json::object();
                    const rpc::Json* engineBody = message->find("body");
                    if (success && pending.command == "continue") {
                        body.set("allThreadsContinued", true);
                    } else if (success && pending.command == "frost.stopPlay") {
                        terminalPlay = message->boolAt("completed").value_or(false);
                        body.set("completed", terminalPlay);
                    } else if (success && pending.command == "source") {
                        if (!engineBody || !engineBody->find("content") || !engineBody->find("content")->asString())
                            success = false;
                        else
                            body.set("content", engineBody->stringAt("content")).set("mimeType", "text/x-lua");
                    } else if (success && pending.command == "stackTrace") {
                        const rpc::Json* frames = engineBody ? engineBody->find("frames") : nullptr;
                        const auto stop = engineBody ? engineBody->integerAt("stopGeneration") : std::nullopt;
                        if (!m_stopped || !frames || !frames->isArray() || !stop || *stop <= 0 ||
                            frames->items().size() > 64 || engineBody->boolAt("hasMore") != false) {
                            success = false;
                        } else {
                            rpc::Json mapped = rpc::Json::array();
                            const std::int64_t total = static_cast<std::int64_t>(frames->items().size());
                            const auto start = std::min(pending.start, total);
                            const auto end =
                                pending.count == 0 ? total : start + std::min(pending.count, total - start);
                            for (std::int64_t index = start; index < end; ++index) {
                                const rpc::Json& stackFrame = frames->items()[static_cast<std::size_t>(index)];
                                const auto frameId = stackFrame.integerAt("id");
                                const auto line = stackFrame.integerAt("line");
                                if (!frameId || *frameId <= 0 || !line || *line < 0 || !stackFrame.isObject()) {
                                    success = false;
                                    break;
                                }
                                auto item = rpc::Json::object()
                                                .set("id", *frameId)
                                                .set("name", stackFrame.stringAt("name"))
                                                .set("line", *line)
                                                .set("column", 1);
                                const auto sourceId = stackFrame.stringAt("sourceId");
                                if (!sourceId.empty()) {
                                    if (sourceId.size() > 128 || m_sources.size() >= 1024 ||
                                        m_nextSourceReference == std::numeric_limits<std::int64_t>::max()) {
                                        success = false;
                                        break;
                                    }
                                    const auto reference = m_nextSourceReference++;
                                    m_sources.emplace(reference, SourceReference{std::string(sourceId), *stop});
                                    item.set("source", rpc::Json::object()
                                                           .set("name", stackFrame.stringAt("sourceName"))
                                                           .set("sourceReference", reference));
                                }
                                mapped.push(std::move(item));
                            }
                            if (success)
                                body.set("stackFrames", std::move(mapped)).set("totalFrames", total);
                        }
                    }
                    respondLocked(pending.dapId, pending.command, success, std::move(body),
                                  success                               ? ""
                                  : pending.command == "frost.stopPlay" ? "notStopped"
                                                                        : "engine rejected the Lua debug request");
                    if (terminalPlay && !m_playTerminated) {
                        m_playTerminated = true;
                        queueLocked(rpc::Json::object()
                                        .set("type", "event")
                                        .set("event", "terminated")
                                        .set("body", rpc::Json::object()));
                    }
                } else if (name == "playStopped") {
                    const auto operation = message->integerAt("operationId");
                    if (operation && *operation > 0 && static_cast<std::uint64_t>(*operation) == m_playOperation &&
                        !m_playTerminated) {
                        m_playTerminated = true;
                        queueLocked(rpc::Json::object()
                                        .set("type", "event")
                                        .set("event", "terminated")
                                        .set("body", rpc::Json::object()));
                    }
                } else if (name == "stopped") {
                    if (m_stopEpoch == std::numeric_limits<std::uint64_t>::max()) {
                        malformed = true;
                        break;
                    }
                    ++m_stopEpoch;
                    m_stopped = true;
                    m_sources.clear();
                    const std::string reason(message->stringAt("reason", "pause"));
                    queueLocked(rpc::Json::object()
                                    .set("type", "event")
                                    .set("event", "stopped")
                                    .set("body", rpc::Json::object()
                                                     .set("reason", reason)
                                                     .set("threadId", 1)
                                                     .set("allThreadsStopped", true)));
                } else if (name == "continued") {
                    if (m_stopEpoch == std::numeric_limits<std::uint64_t>::max()) {
                        malformed = true;
                        break;
                    }
                    ++m_stopEpoch;
                    m_stopped = false;
                    m_sources.clear();
                    queueLocked(
                        rpc::Json::object()
                            .set("type", "event")
                            .set("event", "continued")
                            .set("body", rpc::Json::object().set("threadId", 1).set("allThreadsContinued", true)));
                }
            }
            if (malformed)
                break;
        }
        // An ended DAP session can remain alive in the UI. Close the peer now so an engine
        // stopped inside its hook resumes even if the session object is retained.
        m_stream->close();
        {
            std::lock_guard lock(m_mutex);
            m_networkClosed = true;
        }
        m_changed.notify_all();
    }

    std::unique_ptr<io::LoopbackStream> m_stream;
    std::thread m_reader;
    rpc::FrameDecoder m_input{1024 * 1024};
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::string> m_output;
    std::size_t m_offset = 0;
    std::size_t m_queuedBytes = 0;
    std::int64_t m_sequence = 1;
    // ID 1 may already have been used by the preconnected armPause handshake.
    std::int64_t m_nextWireRequest = 2;
    std::uint64_t m_playOperation = 0;
    bool m_playTerminated = false;
    std::map<std::int64_t, Pending> m_pending;
    std::map<std::int64_t, SourceReference> m_sources;
    std::int64_t m_nextSourceReference = 1;
    std::uint64_t m_stopEpoch = 0;
    bool m_stopped = false;
    bool m_closed = false;
    bool m_networkClosed = false;
};

} // namespace

AdapterFactory luaDebugAdapterFactory(std::unique_ptr<io::LoopbackStream> authenticatedStream,
                                      std::int64_t nextWireRequest, std::uint64_t playOperation) {
    struct PendingStream {
        std::mutex mutex;
        std::unique_ptr<io::LoopbackStream> stream;
    };
    auto pending = std::make_shared<PendingStream>();
    pending->stream = std::move(authenticatedStream);
    return [pending, nextWireRequest,
            playOperation](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        std::unique_ptr<io::LoopbackStream> stream;
        {
            std::lock_guard lock(pending->mutex);
            stream = std::move(pending->stream);
        }
        if (!stream || nextWireRequest < 2 || nextWireRequest >= std::numeric_limits<std::int64_t>::max() ||
            playOperation > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return fail(ErrorCode::InvalidArgument, "the Lua debug connection is unavailable or already in use");
        return makeDapClient(std::make_unique<LuaAdapterTransport>(std::move(stream), nextWireRequest, playOperation),
                             std::move(handlers));
    };
}

AdapterFactory luaDebugAdapterFactory(std::uint16_t port, std::string token, std::chrono::milliseconds timeout,
                                      bool pauseOnStart) {
    return [port, token = std::move(token), timeout,
            pauseOnStart](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        auto stream = connectLuaDebugEndpoint(port, token, std::chrono::steady_clock::now() + timeout, pauseOnStart);
        if (!stream)
            return std::unexpected(std::move(stream.error()));
        return makeDapClient(std::make_unique<LuaAdapterTransport>(std::move(*stream)), std::move(handlers));
    };
}

} // namespace frost::debug
