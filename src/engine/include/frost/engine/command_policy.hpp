// SPDX-License-Identifier: Apache-2.0
// Schema opt-in admission for authenticated FrostEngine commands (FER-3).
#pragma once

#include "frost/engine/schema.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace frost::engine {

// One current observation, supplied on the session worker immediately before sending a command.
// Project values are canonical physical identity keys, resolved by the trusted caller; raw engine
// paths are not identity keys. Session values identify the selected and authenticated connection.
// This value owns its strings and does not grant trust or mutation permission by default.
struct CommandPolicyContext {
    bool connected = false;
    bool paired = false;
    bool trusted = false;
    bool mutationAllowed = false;
    bool editorAvailable = false;
    std::string selectedProject;
    std::string sessionProject;
    std::string selectedSession;
    std::string sessionId;
    std::string state; // Edit, Play, Simulate, Paused or DebuggerPaused, from engine observations.
    std::vector<std::string> availableCommands;
    std::vector<std::string> capabilities;
};

// Owns one authenticated engine's parsed schema; replace it on reconnect or schema changes.
// Read-only calls are thread-safe when callers keep their context immutable during admission.
class CommandPolicy {
public:
    // Bind the schema to the physical project and authenticated instance/generation that exported
    // it. Empty bindings deny every command. All arguments are owned; no references are retained.
    CommandPolicy(SchemaDocument schema, std::string projectIdentity, std::string sessionId);

    // Pure; linear scans of commands, parameters, flags and availability, plus one capability
    // lookup per required capability. Success authorizes only this command at the given
    // observation. Call again immediately before every transport send; a cached success is not a
    // permission. Unknown commands/flags, absent opt-in metadata, unavailable commands/capabilities,
    // unsuitable states, unpaired/disconnected sessions, mismatched identity and revoked trust or
    // mutation permission fail closed. Params must be an object without caller-supplied credentials.
    // Errors use Unsupported, PermissionDenied, Untrusted, Busy, Corrupt or InvalidArgument and name
    // the observed condition; no engine response or successful execution is inferred here.
    [[nodiscard]] Result<void> admit(std::string_view command, const rpc::Json& params,
                                     const CommandPolicyContext& context) const;
    [[nodiscard]] const SchemaDocument& schema() const noexcept { return m_schema; }

private:
    SchemaDocument m_schema;
    std::string m_projectIdentity;
    std::string m_sessionId;
};

} // namespace frost::engine
