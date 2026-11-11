// SPDX-License-Identifier: Apache-2.0
// Isolated helper compatibility entry backed by the same authenticated contract-v2 session/policy.
#include "frost/engine/command_policy.hpp"
#include "frost/engine/session.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/engine_automation.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"

#include "engine_locator.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace frost::app {
namespace {
struct HelperSessionState {
    std::mutex mutex;
    std::condition_variable changed;
    std::unique_ptr<engine::CommandPolicy> policy;
    engine::CommandPolicyContext context;
    bool connected = false, finished = false;
    std::optional<Error> failure;
    std::optional<lang::Json> result;
};
std::string key(const engine::EngineIdentity& identity) { return identity.instanceId + "/" + identity.generation; }
} // namespace

// Worker/helper-process only. A retained instance id is mandatory when discovery is ambiguous.
// The helper waits for one reply, never retries a mutation, and destroys its socket on exit.
Result<lang::Json> executeEngineSessionHelper(const lang::Json& input) {
    const auto root = input.stringAt("root");
    const auto directory = input.stringAt("directory");
    const auto trustPath = input.stringAt("trustPath");
    const auto method = input.stringAt("method");
    const auto* params = input.find("params");
    if (!io::isAbsolutePath(root) || !io::isAbsolutePath(directory) || !io::isAbsolutePath(trustPath) ||
        method.empty() || !params || !params->isObject())
        return fail(ErrorCode::InvalidArgument, "Invalid engine helper request");
    auto& fs = io::realFileSystem();
    auto physicalRoot = fs.canonicalPath(root);
    if (!physicalRoot)
        return std::unexpected(physicalRoot.error());
    auto trustStore = project::TrustStore::open(fs, trustPath);
    if (!trustStore || !(*trustStore)->isTrusted(*physicalRoot))
        return fail(ErrorCode::Untrusted, "Project is not trusted");
    const auto admission = (*trustStore)->snapshot(*physicalRoot);
    auto originalTrustDocument = fs.readFile(trustPath);
    if (!originalTrustDocument)
        return std::unexpected(originalTrustDocument.error());
    auto found = listEngineInstances(fs, directory, *physicalRoot);
    if (!found)
        return std::unexpected(found.error());
    const auto selected = input.stringAt("instanceId");
    const EngineInstanceRecord* record = nullptr;
    for (const auto& instance : *found)
        if (selected.empty() || instance.instanceId == selected) {
            if (record)
                return fail(ErrorCode::Busy, "Select an editor instance before sending commands");
            record = &instance;
        }
    if (!record)
        return fail(ErrorCode::NotFound, "No matching engine instance is advertised");
    engine::EngineEndpoint endpoint;
    endpoint.port = record->port;
    endpoint.projectPath = record->record.stringAt("projectPath");
    endpoint.instanceId = record->instanceId;
    endpoint.generation = record->record.stringAt("generation");
    endpoint.secret = record->record.stringAt("token");
    if (endpoint.instanceId.empty() || endpoint.generation.empty() || endpoint.secret.empty())
        return fail(ErrorCode::Unsupported, "The advertised engine does not support authenticated contract v2");
    const auto state = std::make_shared<HelperSessionState>();
    const auto projectKey = lang::documentKey(*physicalRoot);
    engine::EngineSessionOptions options;
    options.endpoint = std::move(endpoint);
    options.trustRevision = admission.revision;
    const auto freshAdmission =
        [&fs, trustPath = std::string(trustPath), root = *physicalRoot, original = std::move(*originalTrustDocument),
         expectedRevision = admission.revision](std::string_view project, std::uint64_t revision) {
            // The helper is a different process: its initial TrustStore cannot observe GUI mutations.
            // Re-read the persisted decision at each admission, including the final pre-send check.
            if (revision != expectedRevision || lang::documentKey(project) != lang::documentKey(root))
                return false;
            auto currentBytes = fs.readFile(trustPath);
            if (!currentBytes || *currentBytes != original)
                return false;
            auto fresh = project::TrustStore::open(fs, trustPath);
            return fresh && (*fresh)->isTrusted(root);
        };
    options.trusted = freshAdmission;
    const auto timeout = lang::engineMethodTimeout(method);
    if (timeout)
        options.requestTimeout = *timeout;
    options.admitCommand = [state](std::string_view command, const lang::Json& arguments,
                                   const engine::EngineIdentity& observed) -> Result<void> {
        std::scoped_lock lock(state->mutex);
        if (!state->policy || state->context.sessionId != key(observed))
            return fail(ErrorCode::PermissionDenied, "No authenticated command policy is available");
        return state->policy->admit(command, arguments, state->context);
    };
    engine::EngineSessionCallbacks callbacks;
    callbacks.connected = [state, projectKey](const engine::EngineIdentity& identity) {
        std::scoped_lock lock(state->mutex);
        auto schema = engine::parseSchemaDocument(identity.schema);
        if (!schema) {
            state->failure = schema.error();
        } else {
            state->policy = std::make_unique<engine::CommandPolicy>(std::move(*schema), projectKey, key(identity));
            auto& context = state->context;
            context.connected = context.paired = context.trusted = context.mutationAllowed = true;
            context.editorAvailable = identity.role == "editor";
            context.selectedProject = projectKey;
            context.sessionProject = lang::documentKey(identity.projectPath);
            context.selectedSession = context.sessionId = key(identity);
            context.capabilities = identity.capabilities;
            context.availableCommands = identity.availableCommands;
            state->connected = true;
        }
        state->changed.notify_all();
    };
    callbacks.state = [state](const engine::EngineState& observed) {
        std::scoped_lock lock(state->mutex);
        if (state->context.sessionId == key(observed.identity))
            state->context.state = observed.state;
    };
    callbacks.disconnected = [state](const engine::EngineDisconnect& disconnected) {
        std::scoped_lock lock(state->mutex);
        state->failure = disconnected.error;
        state->context.connected = false;
        state->changed.notify_all();
    };
    auto session = engine::EngineSession::create(std::move(options), std::move(callbacks));
    if (!session)
        return std::unexpected(session.error());
    {
        std::unique_lock lock(state->mutex);
        if (!state->changed.wait_for(lock, std::chrono::seconds(10),
                                     [&] { return state->connected || state->failure; }))
            return fail(ErrorCode::Timeout, "Engine pairing did not complete in time");
        if (state->failure)
            return std::unexpected(*state->failure);
    }
    auto requested = (*session)->request(std::string(method), *params, [state](Result<lang::Json> result) {
        std::scoped_lock lock(state->mutex);
        state->finished = true;
        if (result)
            state->result = std::move(*result);
        else
            state->failure = result.error();
        state->changed.notify_all();
    });
    if (!requested)
        return std::unexpected(requested.error());
    Result<lang::Json> result = fail(ErrorCode::Internal, "Engine request completed without a result");
    {
        std::unique_lock lock(state->mutex);
        if (timeout) {
            if (!state->changed.wait_for(lock, *timeout + std::chrono::seconds(1),
                                         [&] { return state->finished || state->failure; }))
                result = fail(ErrorCode::Timeout, "Engine command timed out; its execution is uncertain");
        } else {
            state->changed.wait(lock, [&] { return state->finished || state->failure; });
        }
        if (state->failure)
            result = std::unexpected(*state->failure);
        else if (state->result)
            result = std::move(*state->result);
    }
    (*session)->shutdown();
    if (!freshAdmission(*physicalRoot, admission.revision))
        return fail(ErrorCode::Untrusted, "Project trust changed while the engine command was running");
    return result;
}

} // namespace frost::app
