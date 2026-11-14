// SPDX-License-Identifier: Apache-2.0
// Persistent, authenticated, Qt-free FrostEngine contract-v2 connection.
#pragma once

#include "frost/core/error.hpp"
#include "frost/engine/crash_artifact_source.hpp"
#include "frost/engine/native_debugger_pause.hpp"
#include "frost/engine/script_debug_bootstrap.hpp"
#include "frost/rpc/json.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::engine {
// Credentials come from the trusted selected discovery record; never display or log secret.
struct EngineEndpoint {
    std::uint16_t port = 0;
    std::string projectPath, instanceId, generation, secret;
};
struct EngineIdentity {
    std::string projectPath, instanceId, generation, role;
    std::uint64_t connectionGeneration = 0;
    // Authenticated server.info process ID; 0 means omitted/unknown, never discovery authority.
    std::int64_t processId = 0;
    // Actual authenticated live source facts for a later local filesystem observer; not a crash event.
    std::optional<CrashArtifactSource> crashArtifactSource;
    // Actual authenticated server.info metadata; absent/false means no private Lua DAP bootstrap.
    bool scriptDebugAvailable = false;
    std::uint32_t scriptDebugProtocolVersion = 0;
    std::vector<std::string> capabilities;
    std::vector<std::string> availableCommands;
    // Bootstrap payloads are populated for connected() and policy admission only; events omit
    // these large values so retaining log rows cannot multiply the whole command schema.
    rpc::Json schema;
    std::uint64_t heartbeatIntervalMs = 0, heartbeatTimeoutMs = 0;
};
struct EngineState {
    EngineIdentity identity;
    std::string state, mode;
    // Opaque actual host play generation; empty when absent, Edit or invalidated by VM replacement.
    std::string playGeneration;
    // Actual remote-start operation observations; empty on hosts without this additive contract.
    std::string operationId, phase, operationError;
    bool playing = false, paused = false;
    std::uint64_t frame = 0, sequence = 0;
    rpc::Json data;
};
struct EngineLogEntry {
    EngineIdentity identity;
    std::string level, logger, message, file;
    // sequence is the engine log cursor; eventSequence is the global push envelope cursor.
    std::uint64_t line = 0, sequence = 0, dropped = 0, eventSequence = 0;
    std::int64_t timestamp = 0;
    rpc::Json data;
};
struct EngineDiagnostic {
    EngineIdentity identity;
    std::string severity, message, file;
    std::uint64_t line = 0, column = 0, eventSequence = 0;
    rpc::Json data;
};
enum class EngineDisconnectCause : std::uint8_t { Unexpected, ProjectChanged, ServerShutdown, TrustRevoked, Shutdown };
struct EngineDisconnect {
    EngineIdentity identity;
    Error error{ErrorCode::IoError, "engine disconnected"};
    bool reconnecting = false;
    EngineDisconnectCause cause = EngineDisconnectCause::Unexpected;
};
struct EngineSessionCallbacks {
    std::function<void(const EngineIdentity&)> connected;
    std::function<void(const EngineState&)> state;
    std::function<void(const EngineLogEntry&)> log;
    std::function<void(const EngineDiagnostic&)> diagnostic;
    std::function<void(const EngineDisconnect&)> disconnected;
};
struct EngineSessionOptions {
    EngineEndpoint endpoint;
    std::uint64_t trustRevision = 0;
    // Required. Must be thread-safe, nonblocking, and validate this exact captured revision.
    std::function<bool(std::string_view, std::uint64_t)> trusted;
    // Optional: re-read the trusted selected discovery record after disconnect. This must enforce
    // the app's selected-instance/restart policy; the session never picks an arbitrary instance.
    // Runs on the worker and must be nonblocking; retain no references to its result.
    std::function<Result<EngineEndpoint>()> rediscover;
    // Required for user requests. Invoked again immediately before transmission; absence denies.
    std::function<Result<void>(std::string_view, const rpc::Json&, const EngineIdentity&)> admitCommand;
    std::chrono::milliseconds connectTimeout{5000};
    // Fallback for schema commands; known methods use independent deadlines documented below.
    std::chrono::milliseconds requestTimeout{300000};
    std::chrono::milliseconds reconnectInitial{100};
    std::chrono::milliseconds reconnectMaximum{5000};
};

/*
 * Thread-safe request/cancel/status APIs. All event and reply callbacks are serialized on the
 * session worker; arguments are borrowed for the callback duration. Callbacks must not throw and
 * must promptly marshal to their consumer (e.g. the GUI). Never destroy or shutdown a session
 * from its callback. No callbacks occur after shutdown returns. Reader and watchdog retain
 * heartbeat liveness while an engine main-thread command is blocked.
 * Memory: <=128 pending requests, <=1 MiB per outgoing request, <=16 MiB per incoming frame,
 * <=32 MiB/1024 incoming payloads (excluding container overhead). Requests are never replayed;
 * transmitted cancellation or timeout reports uncertainty and cannot roll back an engine
 * operation. Destruction joins workers.
 */
struct EngineCommandObservation {
    rpc::Json data;
    // Last authoritative event-envelope cursor processed before the actual command reply on the
    // serialized session worker. GUI publication may lag this cursor; no state is synthesized.
    std::uint64_t stateSequenceAtReply = 0;
};
class EngineSession {
public:
    using Reply = std::function<void(Result<rpc::Json>)>;
    // Validates selection/options/trust synchronously, then connects asynchronously; no bearer fallback.
    // Errors: InvalidArgument, Untrusted, Internal (crypto unavailable), IoError (worker creation).
    [[nodiscard]] static Result<std::unique_ptr<EngineSession>> create(EngineSessionOptions options,
                                                                       EngineSessionCallbacks callbacks = {});
    ~EngineSession();
    EngineSession(const EngineSession&) = delete;
    EngineSession& operator=(const EngineSession&) = delete;
    /*
     * Returns a local handle or Busy/InvalidArgument. A disconnected session refuses admission.
     * stop is checked while queued and in flight; completion is delivered exactly once.
     * Deadlines: test.run unbounded; script.validate 30s; play commands, log.get and editor.focus 5s;
     * other commands options.requestTimeout. Each deadline belongs to its request.
     */
    [[nodiscard]] Result<std::uint64_t> request(std::string command, rpc::Json params, Reply reply,
                                                std::stop_token stop = {});
    using ObservedReply = std::function<void(Result<EngineCommandObservation>)>;
    [[nodiscard]] Result<std::uint64_t> requestObserved(std::string command, rpc::Json params, ObservedReply reply,
                                                        std::stop_token stop = {});
    using ScriptDebugReply = std::function<void(Result<ScriptDebugBootstrap>)>;
    // Private integration path, never exposed by command()/Console or public JSON snapshots.
    // Captures the exact selected connection; rechecks identity, actual capability, trust and
    // caller admission on the worker immediately before send. Typed completion runs once on the
    // worker and owns the one-use credential. No credential-bearing generic Reply is invoked.
    [[nodiscard]] Result<std::uint64_t>
    bootstrapScriptDebug(const EngineIdentity& expected, std::function<Result<void>(const EngineIdentity&)> admission,
                         ScriptDebugReply reply, std::stop_token stop = {});
    // Revoked when this exact authenticated connection fails or shuts down. Use alongside project
    // and session cancellation to close a blocked DAP stream; never grants independent authority.
    [[nodiscard]] Result<std::stop_token> debugConnectionStopToken(const EngineIdentity& expected) const;
    // One immutable native registry/controller/PID binding per current authenticated connection.
    // Caller must prove the native adapter uniquely belongs to this exact trusted engine before
    // registering. Lease affects heartbeat expiry only; EOF/trust/errors/write/request deadlines
    // still revoke. Reconnect requires a new lease and fresh actual native evidence.
    [[nodiscard]] Result<NativePauseLease> bindNativeDebuggerPause(const EngineIdentity& expected,
                                                                   NativeDebuggerBinding binding);
    void cancel(std::uint64_t requestId);
    // Idempotent; call from the owner, after preventing concurrent shutdown/destruction.
    void shutdown() noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    // Fully written user command frames; excludes auth/schema/subscription and refused admission.
    [[nodiscard]] std::uint64_t sentCommandCount() const noexcept;
    struct Impl;

private:
    explicit EngineSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::engine
