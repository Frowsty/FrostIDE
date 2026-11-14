// SPDX-License-Identifier: Apache-2.0
// Conservative, schema-driven command admission with no transport side effects.
#include "frost/engine/command_policy.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace frost::engine {
namespace {
template<class Range>
bool contains(const Range& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}
} // namespace

CommandPolicy::CommandPolicy(SchemaDocument schema, std::string projectIdentity, std::string sessionId)
    : m_schema(std::move(schema)), m_projectIdentity(std::move(projectIdentity)), m_sessionId(std::move(sessionId)) {}

Result<void> CommandPolicy::admit(std::string_view command, const rpc::Json& params,
                                  const CommandPolicyContext& context) const {
    if (command.empty() || !params.isObject() || params.find("token") || params.find("secret")) {
        return fail(ErrorCode::InvalidArgument, "engine command parameters are invalid or contain credentials");
    }
    if (!context.trusted) {
        return fail(ErrorCode::Untrusted, "project is not trusted");
    }
    if (!context.connected) {
        return fail(ErrorCode::Busy, "engine session is disconnected");
    }
    if (!context.paired) {
        return fail(ErrorCode::PermissionDenied, "engine session is not paired");
    }
    if (m_projectIdentity.empty() || context.selectedProject != m_projectIdentity ||
        context.sessionProject != m_projectIdentity) {
        return fail(ErrorCode::PermissionDenied, "engine session does not match the selected project");
    }
    if (m_sessionId.empty() || context.selectedSession != m_sessionId || context.sessionId != m_sessionId) {
        return fail(ErrorCode::PermissionDenied, "engine session does not match the selected instance");
    }
    if (m_schema.schemaVersion != kSchemaExportVersion || m_schema.engineVersion.empty()) {
        return fail(ErrorCode::Corrupt, "engine command schema has not been validated");
    }
    const auto found = std::ranges::find(m_schema.commands, command, &SchemaCommand::name);
    if (found == m_schema.commands.end()) {
        return fail(ErrorCode::Unsupported, std::format("command '{}' is absent from the engine schema", command));
    }
    if (std::ranges::count(m_schema.commands, command, &SchemaCommand::name) != 1) {
        return fail(ErrorCode::Corrupt, std::format("command '{}' is ambiguous in the engine schema", command));
    }
    if (!found->idePolicy || !found->idePolicy->allowed) {
        return fail(ErrorCode::PermissionDenied, std::format("command '{}' has no IDE policy opt-in", command));
    }
    constexpr std::array<std::string_view, 5> knownFlags{"Mutating", "RequiresProject", "RequiresEditor", "Headless",
                                                         "Batchable"};
    for (const std::string& flag : found->flags) {
        if (!contains(knownFlags, flag)) {
            return fail(ErrorCode::Unsupported, std::format("command '{}' has unsupported flag '{}'", command, flag));
        }
    }
    if (!contains(context.availableCommands, command)) {
        return fail(ErrorCode::Unsupported, std::format("command '{}' is unavailable in this engine session", command));
    }
    const auto& policy = *found->idePolicy;
    for (const std::string& capability : policy.requiredCapabilities) {
        if (!contains(context.capabilities, capability)) {
            return fail(ErrorCode::Unsupported,
                        std::format("command '{}' requires engine capability '{}'", command, capability));
        }
    }
    if (contains(found->flags, "RequiresEditor") && !context.editorAvailable) {
        return fail(ErrorCode::Unsupported, std::format("command '{}' requires an editor", command));
    }
    constexpr std::array<std::string_view, 5> knownStates{"Edit", "Play", "Simulate", "Paused", "DebuggerPaused"};
    if (!contains(knownStates, context.state) || !contains(policy.states, context.state)) {
        return fail(ErrorCode::Busy, std::format("command '{}' is not admitted in the observed engine state", command));
    }
    if ((contains(found->flags, "Mutating") || policy.requiresMutationPermission) && !context.mutationAllowed) {
        return fail(ErrorCode::PermissionDenied, std::format("command '{}' requires mutation permission", command));
    }
    return {};
}

} // namespace frost::engine
