// SPDX-License-Identifier: Apache-2.0
// Persistent engine transport: mutual HMAC, bounded NDJSON, worker callbacks and reconnect.
#include "frost/engine/session.hpp"

#include "frost/io/loopback_stream.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <sodium.h>

namespace frost::engine {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t kFrameLimit = 16 * 1024 * 1024;
constexpr std::size_t kQueueLimit = 32 * 1024 * 1024;
constexpr std::size_t kRequestLimit = 1024 * 1024;
constexpr std::size_t kPendingLimit = 128;
constexpr std::array<std::string_view, 4> kCapabilities{"session.v2", "auth.hmac-sha256", "events.v1", "heartbeat.v1"};
constexpr std::array<std::string_view, 5> kEvents{"play.state", "log.entry", "diagnostic", "project.changed",
                                                  "server.shutdown"};

bool hex(std::string_view value, std::size_t size) {
    return value.size() == size &&
           std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool validEndpoint(const EngineEndpoint& endpoint) {
    return endpoint.port != 0 && endpoint.projectPath.size() <= 32768 && io::isAbsolutePath(endpoint.projectPath) &&
           endpoint.projectPath.find_first_of("\r\n") == std::string::npos &&
           endpoint.projectPath.find('\0') == std::string::npos && hex(endpoint.instanceId, 32) &&
           hex(endpoint.generation, 32) && !endpoint.secret.empty() && endpoint.secret.size() <= 4096;
}
Clock::time_point requestDeadline(std::string_view method, std::chrono::milliseconds fallback) {
    if (method == "test.run")
        return Clock::time_point::max();
    auto timeout = fallback;
    if (method == "script.validate")
        timeout = 30s;
    else if (method.starts_with("play.") || method == "log.get" || method == "editor.focus")
        timeout = 5s;
    const auto now = Clock::now();
    // Treat a caller's explicitly enormous fallback as unbounded without overflowing the clock.
    if (timeout >= std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now))
        return Clock::time_point::max();
    return now + timeout;
}
std::int64_t ticks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
rpc::Json identityJson(const EngineIdentity& identity) {
    return rpc::Json::object()
        .set("instanceId", identity.instanceId)
        .set("generation", identity.generation)
        .set("projectPath", identity.projectPath);
}
// Bootstrap schema belongs to the connection, not every retained log/diagnostic row.
EngineIdentity eventIdentity(const EngineIdentity& source) {
    EngineIdentity result;
    result.projectPath = source.projectPath;
    result.instanceId = source.instanceId;
    result.generation = source.generation;
    result.role = source.role;
    result.connectionGeneration = source.connectionGeneration;
    result.processId = source.processId;
    result.capabilities = source.capabilities;
    result.heartbeatIntervalMs = source.heartbeatIntervalMs;
    result.heartbeatTimeoutMs = source.heartbeatTimeoutMs;
    return result;
}
bool matches(const rpc::Json& json, const EngineIdentity& identity) {
    return json.stringAt("instanceId") == identity.instanceId && json.stringAt("generation") == identity.generation &&
           json.stringAt("projectPath") == identity.projectPath;
}
rpc::Json strings(std::span<const std::string_view> values) {
    auto result = rpc::Json::array();
    for (const auto& value : values)
        result.push(value);
    return result;
}
std::string mac(std::string_view secret, std::string_view transcript) {
    crypto_auth_hmacsha256_state state{};
    std::array<unsigned char, crypto_auth_hmacsha256_BYTES> bytes{};
    crypto_auth_hmacsha256_init(&state, reinterpret_cast<const unsigned char*>(secret.data()), secret.size());
    crypto_auth_hmacsha256_update(&state, reinterpret_cast<const unsigned char*>(transcript.data()), transcript.size());
    crypto_auth_hmacsha256_final(&state, bytes.data());
    std::array<char, crypto_auth_hmacsha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
    sodium_memzero(&state, sizeof(state));
    return encoded.data();
}
std::string transcript(std::string_view role, const EngineIdentity& identity, std::string_view client,
                       std::string_view server, std::uint64_t interval, std::uint64_t timeout) {
    return "FrostEngine.Session.v2\nrole=" + std::string(role) + "\ninstanceId=" + identity.instanceId +
           "\ngeneration=" + identity.generation + "\nprojectPath=" + identity.projectPath +
           "\nclientNonce=" + std::string(client) + "\nserverNonce=" + std::string(server) +
           "\ncapabilities=session.v2,auth.hmac-sha256,events.v1,heartbeat.v1\nheartbeatIntervalMs=" +
           std::to_string(interval) + "\nheartbeatTimeoutMs=" + std::to_string(timeout) + "\n";
}
// Server-controlled strings and successful payloads cannot accidentally expose the paired token.
std::string scrub(std::string value, std::string_view secret) {
    if (!secret.empty()) {
        std::size_t offset = 0;
        while ((offset = value.find(secret, offset)) != std::string::npos) {
            value.replace(offset, secret.size(), "[redacted]");
            offset += 10;
        }
    }
    return value;
}
rpc::Json redact(const rpc::Json& input, std::string_view secret) {
    if (const auto* value = input.asString())
        return scrub(*value, secret);
    if (input.isArray()) {
        auto result = rpc::Json::array();
        for (const auto& child : input.items())
            result.push(redact(child, secret));
        return result;
    }
    if (input.isObject()) {
        auto result = rpc::Json::object();
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto& key = input.keys()[i];
            if (key == "token" || key == "secret" || key == "proof" || key == "serverProof")
                result.set(scrub(key, secret), "[redacted]");
            else
                result.set(scrub(key, secret), redact(input.items()[i], secret));
        }
        return result;
    }
    return input;
}
Result<rpc::Json> responseResult(const rpc::Json& response, std::string_view secret = {}) {
    if (const auto* error = response.find("error")) {
        // Authentication failures deliberately disclose no peer-controlled text.
        if (secret.empty())
            return fail(ErrorCode::PermissionDenied, "engine refused authentication or bootstrap");
        const auto safe = redact(*error, secret);
        const auto code = safe.integerAt("code");
        if (!code)
            return fail(ErrorCode::Corrupt, "engine reply has malformed error");
        ErrorCode category = ErrorCode::IoError;
        switch (*code) {
        case -32602: category = ErrorCode::InvalidArgument; break;
        case -32601: category = ErrorCode::Unsupported; break;
        case -32001: category = ErrorCode::NotFound; break;
        case -32003: category = ErrorCode::Busy; break;
        case -32004: category = ErrorCode::PermissionDenied; break;
        default: break;
        }
        std::string message = "engine refused request (" + std::to_string(*code) +
                              "): " + std::string(safe.stringAt("message").substr(0, 4096));
        if (const auto* data = safe.find("data"))
            message += " [data: " + data->dump().substr(0, 4096) + "]";
        return fail(category, std::move(message));
    }
    const auto* result = response.find("result");
    if (!result)
        return fail(ErrorCode::Corrupt, "engine reply has no result");
    return *result;
}
struct Handshake {
    EngineIdentity identity;
    rpc::Json snapshot;
    std::uint64_t sequence = 0;
    std::string buffered;
    std::chrono::milliseconds heartbeatTimeout{5000};
};
Result<rpc::Json> exchange(io::LoopbackStream& stream, std::uint64_t id, std::string_view method, rpc::Json params,
                           std::string& buffered, const EngineIdentity* identity = nullptr,
                           bool attachIdentity = true) {
    auto outgoing = rpc::Json::object()
                        .set("jsonrpc", "2.0")
                        .set("id", id)
                        .set("method", std::string(method))
                        .set("params", std::move(params));
    if (identity && attachIdentity)
        outgoing.set("session", identityJson(*identity));
    auto written = stream.write(outgoing.dump() + "\n");
    if (!written)
        return std::unexpected(written.error());
    std::array<char, 8192> bytes{};
    for (;;) {
        const auto newline = buffered.find('\n');
        if (newline != std::string::npos) {
            if (newline > kFrameLimit)
                return fail(ErrorCode::OutOfRange, "engine handshake frame too large");
            auto parsed = rpc::parseJson(std::string_view(buffered).substr(0, newline));
            buffered.erase(0, newline + 1);
            if (!parsed || !parsed->isObject() || parsed->stringAt("jsonrpc") != "2.0")
                return fail(ErrorCode::Corrupt, "invalid engine handshake");
            if (parsed->unsignedAt("id") == id)
                return responseResult(*parsed);
            // Heartbeats may be interleaved with main-thread bootstrap replies.
            if (identity && parsed->stringAt("method") == "engine.heartbeat") {
                const auto* heartbeat = parsed->find("params");
                if (heartbeat && matches(*heartbeat, *identity) && heartbeat->unsignedAt("contractVersion") == 2)
                    continue;
            }
            return fail(ErrorCode::Corrupt, "unexpected engine handshake message");
        }
        if (buffered.size() > kFrameLimit)
            return fail(ErrorCode::OutOfRange, "engine handshake frame too large");
        auto read = stream.read(bytes);
        if (!read)
            return std::unexpected(read.error());
        if (*read == 0)
            return fail(ErrorCode::IoError, "engine closed during authentication");
        buffered.append(bytes.data(), *read);
    }
}
Result<Handshake> authenticate(io::LoopbackStream& stream, const EngineEndpoint& endpoint, std::uint64_t generation,
                               const std::function<bool()>& trusted) {
    Handshake result;
    result.identity.projectPath = endpoint.projectPath;
    result.identity.instanceId = endpoint.instanceId;
    result.identity.generation = endpoint.generation;
    result.identity.connectionGeneration = generation;
    for (const auto capability : kCapabilities)
        result.identity.capabilities.emplace_back(capability);
    std::array<unsigned char, 32> nonce{};
    std::array<char, 65> nonceText{};
    randombytes_buf(nonce.data(), nonce.size());
    sodium_bin2hex(nonceText.data(), nonceText.size(), nonce.data(), nonce.size());
    const std::string client(nonceText.data());
    if (!trusted())
        return fail(ErrorCode::Untrusted, "engine project trust changed");
    auto challenge = exchange(stream, 1, "auth.challenge",
                              rpc::Json::object()
                                  .set("contractVersion", 2)
                                  .set("clientNonce", client)
                                  .set("projectPath", endpoint.projectPath)
                                  .set("requiredCapabilities", strings(kCapabilities)),
                              result.buffered);
    if (!challenge)
        return std::unexpected(challenge.error());
    const auto interval = challenge->unsignedAt("heartbeatIntervalMs").value_or(0);
    const auto timeout = challenge->unsignedAt("heartbeatTimeoutMs").value_or(0);
    const auto server = challenge->stringAt("serverNonce");
    const auto proof = challenge->stringAt("serverProof");
    const auto* caps = challenge->find("capabilities");
    if (!matches(*challenge, result.identity) || challenge->unsignedAt("contractVersion") != 2 ||
        challenge->stringAt("algorithm") != "hmac-sha256" || challenge->stringAt("clientNonce") != client ||
        !hex(server, 64) || !hex(proof, 64) || !caps || *caps != strings(kCapabilities) || interval < 1 ||
        interval > 60000 || timeout <= interval || timeout > 300000)
        return fail(ErrorCode::PermissionDenied, "engine session contract or identity mismatch");
    const auto expected =
        mac(endpoint.secret, transcript("server", result.identity, client, server, interval, timeout));
    if (sodium_memcmp(expected.data(), proof.data(), expected.size()) != 0)
        return fail(ErrorCode::PermissionDenied, "engine server authentication failed");
    if (!trusted())
        return fail(ErrorCode::Untrusted, "engine project trust changed");
    auto proved =
        exchange(stream, 2, "auth.prove",
                 rpc::Json::object().set("proof", mac(endpoint.secret, transcript("client", result.identity, client,
                                                                                  server, interval, timeout))),
                 result.buffered, &result.identity, false);
    if (!proved)
        return std::unexpected(proved.error());
    if (!matches(*proved, result.identity) || proved->boolAt("authenticated") != true ||
        proved->unsignedAt("contractVersion") != 2)
        return fail(ErrorCode::PermissionDenied, "engine authentication identity changed");
    result.identity.heartbeatIntervalMs = interval;
    result.identity.heartbeatTimeoutMs = timeout;
    if (!trusted())
        return fail(ErrorCode::Untrusted, "engine project trust changed");
    auto info = exchange(stream, 3, "server.info", rpc::Json::object(), result.buffered, &result.identity);
    if (!info)
        return std::unexpected(info.error());
    const auto role = info->stringAt("role");
    if (!matches(*info, result.identity) || info->unsignedAt("contractVersion") != 2 ||
        info->unsignedAt("sessionProtocolVersion") != 2 || info->boolAt("authenticated") != true ||
        (role != "editor" && role != "player" && role != "headless"))
        return fail(ErrorCode::Corrupt, "engine server info does not match authenticated session");
    result.identity.role = role;
    // PID is usable for native DAP correlation only after the mutual proof and exact session
    // identity checks above. Older v2 hosts may omit it; malformed claimed IDs fail closed.
    if (const auto* pid = info->find("processId")) {
        const auto value = pid->asInteger();
        if (pid->kind() != rpc::Json::Kind::Integer || !value || *value <= 0)
            return fail(ErrorCode::Corrupt, "engine server info has an invalid process ID");
        result.identity.processId = *value;
    }
    if (!trusted())
        return fail(ErrorCode::Untrusted, "engine project trust changed");
    auto schema = exchange(stream, 4, "schema.export", rpc::Json::object(), result.buffered, &result.identity);
    if (!schema)
        return std::unexpected(schema.error());
    result.identity.schema = redact(*schema, endpoint.secret);
    if (const auto* commands = result.identity.schema.find("commands"); commands && commands->isArray())
        for (const auto& command : commands->items())
            if (!command.stringAt("name").empty())
                result.identity.availableCommands.emplace_back(command.stringAt("name"));
    if (!trusted())
        return fail(ErrorCode::Untrusted, "engine project trust changed");
    auto subscribed = exchange(stream, 5, "server.subscribe", rpc::Json::object().set("events", strings(kEvents)),
                               result.buffered, &result.identity);
    if (!subscribed)
        return std::unexpected(subscribed.error());
    if (!matches(*subscribed, result.identity))
        return fail(ErrorCode::Corrupt, "engine subscription identity mismatch");
    const auto serial = subscribed->unsignedAt("sequence");
    if (!serial)
        return fail(ErrorCode::Corrupt, "engine subscription has no sequence");
    result.sequence = *serial;
    if (const auto* snapshots = subscribed->find("snapshots"))
        if (const auto* state = snapshots->find("play.state"))
            result.snapshot = *state;
    if (!result.snapshot.isObject())
        return fail(ErrorCode::Corrupt, "engine subscription has no play state snapshot");
    result.heartbeatTimeout = std::chrono::milliseconds(timeout);
    return result;
}
} // namespace

struct EngineSession::Impl {
    struct Pending {
        std::uint64_t id = 0;
        std::string command;
        rpc::Json params;
        Reply reply;
        std::stop_token stop;
        Clock::time_point deadline;
        bool sent = false, cancelled = false;
    };
    struct Connection {
        std::shared_ptr<io::LoopbackStream> stream;
        EngineIdentity identity;
        std::string secret;
        std::atomic<bool> failed{false};
        std::atomic<std::int64_t> lastHeartbeat{ticks()};
        std::atomic<std::int64_t> writeStarted{0};
        std::chrono::milliseconds heartbeatTimeout{5000};
        ErrorCode failure = ErrorCode::IoError; // Protected by owner's mutex.
        std::jthread reader;
        ~Connection() { sodium_memzero(secret.data(), secret.size()); }
    };
    EngineSessionOptions options;
    EngineSessionCallbacks callbacks;
    std::atomic<bool> online{false}, stopping{false};
    std::atomic<std::uint64_t> generation{0}, sentCount{0};
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::uint64_t, Pending> pending;
    std::deque<std::string> incoming;
    std::size_t incomingBytes = 0, incomingCount = 0;
    std::uint64_t nextId = 16;
    std::shared_ptr<Connection> connection;
    std::jthread worker, watchdog;

    bool trusted(std::string_view path) const {
        return options.trusted && options.trusted(path, options.trustRevision);
    }
    void breakConnection(const std::shared_ptr<Connection>& conn, ErrorCode code) {
        {
            std::lock_guard lock(mutex);
            if (!conn->failed.exchange(true))
                conn->failure = code;
        }
        conn->stream->close();
        cv.notify_all();
    }
    void readLoop(const std::shared_ptr<Connection>& conn, std::string buffered) {
        std::array<char, 8192> bytes{};
        while (!stopping && !conn->failed) {
            for (;;) {
                const auto newline = buffered.find('\n');
                if (newline == std::string::npos)
                    break;
                if (newline > kFrameLimit) {
                    breakConnection(conn, ErrorCode::OutOfRange);
                    return;
                }
                std::string line = buffered.substr(0, newline);
                buffered.erase(0, newline + 1);
                auto message = rpc::parseJson(line);
                if (!message || !message->isObject() || message->stringAt("jsonrpc") != "2.0") {
                    breakConnection(conn, ErrorCode::Corrupt);
                    return;
                }
                if (message->stringAt("method") == "engine.heartbeat") {
                    const auto* params = message->find("params");
                    if (!params || !matches(*params, conn->identity) || params->unsignedAt("contractVersion") != 2) {
                        breakConnection(conn, ErrorCode::Corrupt);
                        return;
                    }
                    conn->lastHeartbeat = ticks();
                    continue;
                }
                bool overflow = false;
                {
                    std::lock_guard lock(mutex);
                    overflow = incomingBytes + line.size() > kQueueLimit || incomingCount >= 1024;
                    if (!overflow) {
                        incomingBytes += line.size();
                        ++incomingCount;
                        incoming.push_back(std::move(line));
                    }
                }
                if (overflow) {
                    breakConnection(conn, ErrorCode::OutOfRange);
                    return;
                }
                cv.notify_all();
            }
            if (buffered.size() > kFrameLimit) {
                breakConnection(conn, ErrorCode::OutOfRange);
                return;
            }
            auto read = conn->stream->read(bytes);
            if (!read || *read == 0) {
                breakConnection(conn, ErrorCode::IoError);
                return;
            }
            buffered.append(bytes.data(), *read);
        }
    }
    void watchLoop(std::stop_token stop) {
        while (!stop.stop_requested() && !stopping) {
            std::shared_ptr<Connection> conn;
            {
                std::lock_guard lock(mutex);
                conn = connection;
            }
            if (conn && !conn->failed) {
                const auto now = ticks();
                const auto started = conn->writeStarted.load();
                if (now - conn->lastHeartbeat.load() > conn->heartbeatTimeout.count() ||
                    (started != 0 && now - started > options.connectTimeout.count()))
                    breakConnection(conn, ErrorCode::Timeout);
            }
            std::this_thread::sleep_for(20ms);
        }
    }
    void state(const std::shared_ptr<Connection>& conn, rpc::Json data, std::uint64_t sequence) {
        if (!data.isObject()) {
            breakConnection(conn, ErrorCode::Corrupt);
            return;
        }
        const auto value = data.stringAt("state");
        const auto mode = data.stringAt("mode");
        const auto paused = data.boolAt("paused");
        const auto frames = data.unsignedAt("frames");
        if ((value != "Edit" && value != "Play" && value != "Simulate" && value != "Paused" &&
             value != "DebuggerPaused") ||
            (mode != "Edit" && mode != "Play" && mode != "Simulate") || !paused || !frames) {
            breakConnection(conn, ErrorCode::Corrupt);
            return;
        }
        EngineState observed;
        observed.identity = eventIdentity(conn->identity);
        observed.state = value;
        observed.mode = mode;
        observed.playing = mode != "Edit";
        observed.paused = *paused;
        observed.frame = *frames;
        observed.sequence = sequence;
        observed.data = std::move(data);
        if (callbacks.state)
            callbacks.state(observed);
    }
    void message(const std::shared_ptr<Connection>& conn, std::string_view line, std::uint64_t& sequence) {
        auto parsed = rpc::parseJson(line);
        if (!parsed) {
            breakConnection(conn, ErrorCode::Corrupt);
            return;
        }
        if (auto id = parsed->unsignedAt("id")) {
            Reply reply;
            {
                std::lock_guard lock(mutex);
                const auto found = pending.find(*id);
                if (found == pending.end() || !found->second.sent)
                    return;
                reply = std::move(found->second.reply);
                pending.erase(found);
            }
            if (reply) {
                auto result = responseResult(*parsed, conn->secret);
                if (result)
                    *result = redact(*result, conn->secret);
                reply(std::move(result));
            }
            return;
        }
        if (parsed->stringAt("method") != "engine.event") {
            breakConnection(conn, ErrorCode::Corrupt);
            return;
        }
        const auto* params = parsed->find("params");
        if (!params || !matches(*params, conn->identity)) {
            breakConnection(conn, ErrorCode::Corrupt);
            return;
        }
        const auto serial = params->unsignedAt("sequence");
        if (!serial || *serial <= sequence) {
            breakConnection(conn, ErrorCode::Corrupt);
            return;
        }
        sequence = *serial;
        const auto* raw = params->find("data");
        if (!raw) {
            breakConnection(conn, ErrorCode::Corrupt);
            return;
        }
        auto data = redact(*raw, conn->secret);
        const auto event = params->stringAt("event");
        if (event == "log.entry" || event == "diagnostic") {
            const auto level = data.stringAt("level");
            const auto* text = data.find("message");
            if (!text || !text->isString() ||
                (level != "trace" && level != "info" && level != "warn" && level != "error" && level != "critical")) {
                breakConnection(conn, ErrorCode::Corrupt);
                return;
            }
        }
        if (event == "play.state")
            state(conn, std::move(data), sequence);
        else if (event == "log.entry" && callbacks.log) {
            EngineLogEntry entry;
            entry.identity = eventIdentity(conn->identity);
            entry.level = data.stringAt("level");
            entry.logger = data.stringAt("logger");
            entry.message = data.stringAt("message");
            if (const auto* source = data.find("source")) {
                entry.file = source->stringAt("path");
                entry.line = source->unsignedAt("line").value_or(0);
            }
            entry.timestamp = data.integerAt("timestamp").value_or(0);
            entry.sequence = data.unsignedAt("sequence").value_or(0);
            entry.eventSequence = sequence;
            entry.dropped = data.unsignedAt("dropped").value_or(0);
            entry.data = std::move(data);
            callbacks.log(entry);
        } else if (event == "diagnostic" && callbacks.diagnostic) {
            EngineDiagnostic entry;
            entry.identity = eventIdentity(conn->identity);
            entry.severity = data.stringAt("level");
            if (entry.severity == "warn")
                entry.severity = "warning";
            else if (entry.severity == "critical")
                entry.severity = "error";
            entry.message = data.stringAt("message");
            entry.file = data.stringAt("path");
            entry.line = data.unsignedAt("line").value_or(0);
            entry.column = data.unsignedAt("column").value_or(0);
            entry.eventSequence = sequence;
            entry.data = std::move(data);
            callbacks.diagnostic(entry);
        } else if (event == "project.changed" || event == "server.shutdown")
            breakConnection(conn, ErrorCode::IoError);
    }
    void completeAll(ErrorCode code) {
        std::map<std::uint64_t, Pending> abandoned;
        {
            std::lock_guard lock(mutex);
            abandoned.swap(pending);
        }
        for (auto& [id, item] : abandoned) {
            static_cast<void>(id);
            if (item.reply)
                item.reply(fail(code, item.sent ? "engine request interrupted; outcome uncertain"
                                                : "engine request was not sent"));
        }
    }
    void workLoop(std::stop_token stop) {
        auto endpoint = options.endpoint;
        auto backoff = options.reconnectInitial;
        while (!stop.stop_requested() && !stopping) {
            EngineIdentity attempted;
            attempted.projectPath = endpoint.projectPath;
            attempted.instanceId = endpoint.instanceId;
            attempted.generation = endpoint.generation;
            Error error(ErrorCode::IoError, "engine connection failed");
            std::shared_ptr<Connection> conn;
            rpc::Json snapshot;
            std::uint64_t initialSequence = 0;
            std::string buffered;
            if (!trusted(endpoint.projectPath))
                error = Error(ErrorCode::Untrusted, "engine project trust changed");
            else {
                auto stream = io::LoopbackStream::connect(endpoint.port, Clock::now() + options.connectTimeout, stop);
                if (!stream)
                    error = stream.error();
                else {
                    auto handshake = authenticate(**stream, endpoint, generation.load() + 1,
                                                  [&] { return trusted(endpoint.projectPath); });
                    if (!handshake)
                        error = handshake.error();
                    else if (!trusted(endpoint.projectPath))
                        error = Error(ErrorCode::Untrusted, "engine project trust changed");
                    else {
                        conn = std::make_shared<Connection>();
                        conn->stream = std::shared_ptr<io::LoopbackStream>(std::move(*stream));
                        conn->stream->beginDuplex();
                        conn->identity = std::move(handshake->identity);
                        conn->secret = endpoint.secret;
                        conn->heartbeatTimeout = handshake->heartbeatTimeout;
                        initialSequence = handshake->sequence;
                        snapshot = std::move(handshake->snapshot);
                        buffered = std::move(handshake->buffered);
                    }
                }
            }
            if (conn && !stopping) {
                {
                    std::lock_guard lock(mutex);
                    incoming.clear();
                    incomingBytes = 0;
                    incomingCount = 0;
                    connection = conn;
                }
                generation = conn->identity.connectionGeneration;
                online = true;
                backoff = options.reconnectInitial;
                try {
                    conn->reader = std::jthread([this, conn, bytes = std::move(buffered)] { readLoop(conn, bytes); });
                } catch (const std::system_error&) {
                    breakConnection(conn, ErrorCode::IoError);
                }
                if (!conn->failed) {
                    if (callbacks.connected)
                        callbacks.connected(conn->identity);
                    state(conn, redact(snapshot, conn->secret), initialSequence);
                }
                std::uint64_t sequence = initialSequence;
                while (!stopping && !conn->failed) {
                    if (!trusted(conn->identity.projectPath)) {
                        breakConnection(conn, ErrorCode::Untrusted);
                        break;
                    }
                    std::deque<std::string> messages;
                    std::vector<Pending> outgoing, expired;
                    {
                        std::unique_lock lock(mutex);
                        cv.wait_for(lock, 20ms);
                        messages.swap(incoming);
                        for (auto it = pending.begin(); it != pending.end();) {
                            auto& item = it->second;
                            if (item.cancelled || item.stop.stop_requested() || Clock::now() >= item.deadline) {
                                expired.push_back(std::move(item));
                                it = pending.erase(it);
                            } else if (!item.sent) {
                                outgoing.push_back(item);
                                ++it;
                            } else
                                ++it;
                        }
                    }
                    for (auto& item : expired)
                        if (item.reply)
                            item.reply(fail(item.cancelled || item.stop.stop_requested() ? ErrorCode::Cancelled
                                                                                         : ErrorCode::Timeout,
                                            item.sent ? "engine request interrupted; outcome uncertain"
                                                      : "engine request was not sent"));
                    for (auto& line : messages) {
                        if (conn->failed || stopping)
                            break;
                        message(conn, line, sequence);
                        {
                            std::lock_guard lock(mutex);
                            incomingBytes -= line.size();
                            --incomingCount;
                        }
                    }
                    for (auto& item : outgoing) {
                        if (conn->failed || stopping)
                            break;
                        auto admitted =
                            options.admitCommand
                                ? options.admitCommand(item.command, item.params, conn->identity)
                                : Result<void>(fail(ErrorCode::PermissionDenied, "engine command policy unavailable"));
                        if (!trusted(conn->identity.projectPath))
                            admitted = fail(ErrorCode::Untrusted, "engine project trust changed");
                        if (!admitted) {
                            Reply reply;
                            {
                                std::lock_guard lock(mutex);
                                auto it = pending.find(item.id);
                                if (it != pending.end()) {
                                    reply = std::move(it->second.reply);
                                    pending.erase(it);
                                }
                            }
                            if (reply) {
                                const auto safe = redact(rpc::Json(admitted.error().message()), conn->secret);
                                reply(fail(admitted.error().code(), std::string(safe.asString()->substr(0, 4096))));
                            }
                            continue;
                        }
                        // Cancellation may arrive during policy callbacks. Recheck immediately at
                        // transmission admission, then record uncertainty before touching the socket.
                        {
                            std::lock_guard lock(mutex);
                            auto found = pending.find(item.id);
                            if (found == pending.end() || found->second.cancelled ||
                                found->second.stop.stop_requested() || Clock::now() >= found->second.deadline)
                                continue;
                            found->second.sent = true;
                        }
                        auto request = rpc::Json::object()
                                           .set("jsonrpc", "2.0")
                                           .set("id", item.id)
                                           .set("method", item.command)
                                           .set("params", std::move(item.params))
                                           .set("session", identityJson(conn->identity));
                        conn->writeStarted = ticks();
                        auto sent = conn->stream->write(request.dump() + "\n");
                        conn->writeStarted = 0;
                        if (!sent) {
                            breakConnection(conn, sent.error().code());
                            break;
                        }
                        ++sentCount;
                    }
                }
                online = false;
                conn->stream->close();
                if (conn->reader.joinable())
                    conn->reader.join();
                {
                    std::lock_guard lock(mutex);
                    error = Error(conn->failure, "engine connection interrupted");
                    connection.reset();
                    incoming.clear();
                    incomingBytes = 0;
                    incomingCount = 0;
                }
                attempted = eventIdentity(conn->identity);
            }
            online = false;
            completeAll(stopping ? ErrorCode::Cancelled : error.code());
            if (stopping || stop.stop_requested())
                break;
            const bool reconnect =
                bool(options.rediscover) && trusted(endpoint.projectPath) && error.code() != ErrorCode::Untrusted;
            if (callbacks.disconnected)
                callbacks.disconnected(EngineDisconnect{attempted, error, reconnect});
            if (!reconnect)
                break;
            {
                std::unique_lock lock(mutex);
                cv.wait_for(lock, backoff, [&] { return stopping.load() || stop.stop_requested(); });
            }
            if (stopping || stop.stop_requested())
                break;
            auto resolved = options.rediscover();
            if (resolved && validEndpoint(*resolved) && resolved->projectPath == options.endpoint.projectPath &&
                trusted(resolved->projectPath)) {
                sodium_memzero(endpoint.secret.data(), endpoint.secret.size());
                endpoint = std::move(*resolved);
            }
            backoff = std::min(backoff * 2, options.reconnectMaximum);
        }
        completeAll(ErrorCode::Cancelled);
        sodium_memzero(endpoint.secret.data(), endpoint.secret.size());
    }
};

EngineSession::EngineSession(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
Result<std::unique_ptr<EngineSession>> EngineSession::create(EngineSessionOptions options,
                                                             EngineSessionCallbacks callbacks) {
    if (!validEndpoint(options.endpoint) || !options.trusted || options.connectTimeout <= 0ms ||
        options.requestTimeout <= 0ms || options.reconnectInitial <= 0ms ||
        options.reconnectMaximum < options.reconnectInitial)
        return fail(ErrorCode::InvalidArgument, "invalid engine session options");
    if (!options.trusted(options.endpoint.projectPath, options.trustRevision))
        return fail(ErrorCode::Untrusted, "engine project is not trusted");
    if (sodium_init() < 0)
        return fail(ErrorCode::Internal, "engine authentication unavailable");
    auto impl = std::make_unique<Impl>();
    impl->options = std::move(options);
    impl->callbacks = std::move(callbacks);
    auto result = std::unique_ptr<EngineSession>(new EngineSession(std::move(impl)));
    auto* state = result->m_impl.get();
    try {
        state->watchdog = std::jthread([state](std::stop_token stop) { state->watchLoop(stop); });
        state->worker = std::jthread([state](std::stop_token stop) { state->workLoop(stop); });
    } catch (const std::system_error&) {
        result->shutdown();
        return fail(ErrorCode::IoError, "cannot start engine session workers");
    }
    return result;
}
EngineSession::~EngineSession() {
    shutdown();
    sodium_memzero(m_impl->options.endpoint.secret.data(), m_impl->options.endpoint.secret.size());
}
Result<std::uint64_t> EngineSession::request(std::string command, rpc::Json params, Reply reply, std::stop_token stop) {
    if (command.empty() || !params.isObject() || !reply || command == "auth" || command.starts_with("auth.") ||
        command == "server.configure" || command == "server.subscribe" || command == "server.shutdown" ||
        command == "engine.event" || command == "engine.heartbeat" ||
        command.size() + params.dump().size() > kRequestLimit)
        return fail(ErrorCode::InvalidArgument, "invalid engine request");
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->online || m_impl->stopping)
        return fail(ErrorCode::Busy, "engine session is disconnected");
    if (m_impl->pending.size() >= kPendingLimit)
        return fail(ErrorCode::Busy, "engine request queue is full");
    // Account for JSON escaping, the identity envelope, ID digits and final newline as well as
    // params. The queued request must fit the complete wire-frame bound, including a long path.
    const auto envelope = rpc::Json::object()
                              .set("jsonrpc", "2.0")
                              .set("id", m_impl->nextId)
                              .set("method", command)
                              .set("params", rpc::Json::object())
                              .set("session", identityJson(m_impl->connection->identity));
    if (envelope.dump().size() - 2 + params.dump().size() + 1 > kRequestLimit)
        return fail(ErrorCode::InvalidArgument, "engine request frame exceeds its limit");
    const auto id = m_impl->nextId++;
    const auto deadline = requestDeadline(command, m_impl->options.requestTimeout);
    m_impl->pending.emplace(
        id, Impl::Pending{id, std::move(command), std::move(params), std::move(reply), stop, deadline, false, false});
    m_impl->cv.notify_all();
    return id;
}
void EngineSession::cancel(std::uint64_t id) {
    std::lock_guard lock(m_impl->mutex);
    if (auto item = m_impl->pending.find(id); item != m_impl->pending.end())
        item->second.cancelled = true;
    m_impl->cv.notify_all();
}
void EngineSession::shutdown() noexcept {
    m_impl->stopping = true;
    m_impl->online = false;
    m_impl->worker.request_stop();
    m_impl->watchdog.request_stop();
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->connection)
            m_impl->connection->stream->close();
    }
    m_impl->cv.notify_all();
    if (m_impl->worker.joinable())
        m_impl->worker.join();
    if (m_impl->watchdog.joinable())
        m_impl->watchdog.join();
}
bool EngineSession::connected() const noexcept { return m_impl->online; }
std::uint64_t EngineSession::generation() const noexcept { return m_impl->generation; }
std::uint64_t EngineSession::sentCommandCount() const noexcept { return m_impl->sentCount; }
} // namespace frost::engine
