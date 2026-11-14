// SPDX-License-Identifier: Apache-2.0
// HMAC session admission, typed push publication, schema policy and stale-reply rejection.
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/toolwindows/engine_console.hpp"
#include "frost/ui/toolwindows/engine_view.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "engine_integration_internal.hpp"
#include "engine_native_host_internal.hpp"
#include <QComboBox>
#include <QMetaObject>
#include <QPushButton>

#include <algorithm>
#include <climits>
#include <utility>

namespace frost::app {
namespace {
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string sessionKey(const engine::EngineIdentity& value) { return value.instanceId + "/" + value.generation; }

sched::Task<void> retireEngineSession(std::unique_ptr<engine::EngineSession> session) {
    session->shutdown();
    co_return;
}

Result<engine::EngineEndpoint> endpointOf(const EngineInstanceRecord& record) {
    const auto generation = record.record.stringAt("generation");
    const auto secret = record.record.stringAt("token");
    if (record.instanceId.empty() || generation.empty() || secret.empty())
        return fail(ErrorCode::Unsupported, "This editor does not advertise authenticated contract v2 pairing");
    // The signed wire path must stay byte-identical to the engine's discovery document. Its
    // physical projectRoot was independently checked; native separators must not replace it.
    return engine::EngineEndpoint{record.port, std::string(record.record.stringAt("projectPath")), record.instanceId,
                                  std::string(generation), std::string(secret)};
}

} // namespace

void EngineIntegration::Impl::retireSession() {
    if (!session)
        return;
    tasks.push_back(workbench.m_pool->spawn(sched::Lane::Background, retireEngineSession(std::move(session))));
}

void EngineIntegration::Impl::clearSession(bool clearOutput, bool preserveCrashObservation) {
    clearAssetUsages();
    if (!preserveCrashObservation)
        revokeCrashObservation();
    ++sessionEpoch;
    eventQueue->active.store(false);
    pushSink = {};
    eventQueue = std::make_shared<EngineEventQueue>();
    policy->active.store(false);
    retireSession();
    policy = std::make_shared<EnginePolicyState>();
    identity.reset();
    state.reset();
    nativeTarget.reset();
    retainedNativeBinding.reset();
    nativeBoundBinding.reset();
    selectedInstance.clear();
    selectedServerGeneration.clear();
    lastRequest = 0;
    lastCompletedRequest = 0;
    requests.clear();
    lastReply = lang::Json{};
    lastError.clear();
    eventHistory.clear();
    eventHistoryBytes = 0;
    eventHistoryDropped = 0;
    diagnosticBytes = 0;
    diagnosticSizes.clear();
    lastEventSequence = 0;
    diagnostics.clear();
    if (workbench.m_problems)
        workbench.m_problems->model()->clear(ui::DiagnosticSource::Engine);
    if (console && clearOutput)
        console->clear();
    if (indicator)
        indicator->setState(ui::EngineConnectionState::Disconnected);
    if (nativeDebug)
        nativeDebug->refresh();
    updateControls();
}

bool EngineIntegration::Impl::currentIdentity(const engine::EngineIdentity& observed) const {
    return identity && observed.instanceId == identity->instanceId && observed.generation == identity->generation &&
           observed.connectionGeneration == identity->connectionGeneration &&
           lang::documentKey(observed.projectPath) == projectKey;
}

Result<void> EngineIntegration::pair(std::string_view instanceId) {
    auto& impl = *m_impl;
    if (impl.ownedRun || impl.nativeHost)
        return fail(ErrorCode::Busy, "Stop the owned engine host before pairing another editor");
    if (instanceId.empty())
        return fail(ErrorCode::InvalidArgument, "Select an editor instance to pair");
    const auto found = std::ranges::find(impl.instances, instanceId, &EngineInstanceRecord::instanceId);
    if (found == impl.instances.end())
        return fail(ErrorCode::NotFound, "The selected editor is absent from the current discovery listing");
    return impl.connectInstance(*found);
}

Result<void> EngineIntegration::Impl::connectInstance(const EngineInstanceRecord& instance) {
    if (!admitted(epoch, root, trustRevision))
        return fail(ErrorCode::Untrusted, "Project trust changed before pairing");
    if (lang::documentKey(instance.projectRoot) != projectKey)
        return fail(ErrorCode::PermissionDenied, "The selected editor has a different project open");
    auto endpoint = endpointOf(instance);
    if (!endpoint)
        return std::unexpected(endpoint.error());
    clearSession(false);
    selectedInstance = instance.instanceId;
    selectedServerGeneration = endpoint->generation;
    const auto currentEpoch = epoch;
    const auto expectedRoot = root;
    const auto revision = trustRevision;
    const auto sharedPolicy = policy;
    const auto crashHolder = crashSessions;
    const auto rootKey = projectKey;
    const auto admission = physicalAdmission;
    auto* trust = &workbench.m_services.trust;
    auto* fs = &workbench.m_services.fs;
    const auto directory = (ownedRun || nativeHost) ? ownedConfigDirectory + "/Automation" : automationDirectory;
    QPointer<EngineIntegration> self(&owner);
    engine::EngineSessionOptions options;
    options.endpoint = std::move(*endpoint);
    options.trustRevision = revision;
    options.trusted = [trust, sharedPolicy, admission, expectedRoot, rootKey](std::string_view project,
                                                                              std::uint64_t expectedRevision) {
        const auto current = trust->snapshot(expectedRoot);
        return admission->load() && sharedPolicy->active.load() && current.trusted &&
               current.revision == expectedRevision && lang::documentKey(project) == rootKey;
    };
    options.admitCommand = [sharedPolicy, fs, expectedRoot, rootKey,
                            admission](std::string_view method, const lang::Json& params,
                                       const engine::EngineIdentity& live) -> Result<void> {
        if (!retainEngineProjectIdentity(*fs, expectedRoot, rootKey, admission))
            return fail(ErrorCode::PermissionDenied, "The selected physical project changed before command dispatch");
        std::scoped_lock lock(sharedPolicy->mutex);
        const auto& context = sharedPolicy->context;
        if (!sharedPolicy->active.load() || !sharedPolicy->policy || context.sessionId != sessionKey(live) ||
            context.sessionProject != lang::documentKey(live.projectPath))
            return fail(ErrorCode::PermissionDenied,
                        "The command policy does not belong to this authenticated session");
        auto observation = context;
        if (sharedPolicy->nativePaused.load())
            observation.state = "DebuggerPaused";
        return sharedPolicy->policy->admit(method, params, observation);
    };
    const auto selected = selectedInstance;
    options.rediscover = [fs, trust, sharedPolicy, directory, expectedRoot, rootKey, admission, revision,
                          selected]() -> Result<engine::EngineEndpoint> {
        const auto current = trust->snapshot(expectedRoot);
        if (!sharedPolicy->active.load() || !current.trusted || current.revision != revision)
            return fail(ErrorCode::Untrusted, "Project trust changed while the editor was disconnected");
        if (!retainEngineProjectIdentity(*fs, expectedRoot, rootKey, admission))
            return fail(ErrorCode::PermissionDenied, "The selected physical project changed while disconnected");
        auto discovered = listEngineInstances(*fs, directory, expectedRoot);
        if (!discovered)
            return std::unexpected(discovered.error());
        const auto found = std::ranges::find(*discovered, selected, &EngineInstanceRecord::instanceId);
        if (found == discovered->end())
            return fail(ErrorCode::NotFound, "The selected editor instance is no longer advertised");
        return endpointOf(*found);
    };
    const auto post = [self, fs, rootKey, admission, currentEpoch, currentSessionEpoch = sessionEpoch, expectedRoot,
                       revision](auto value, auto apply) {
        if (!self || !retainEngineProjectIdentity(*fs, expectedRoot, rootKey, admission))
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, currentEpoch, currentSessionEpoch, expectedRoot, revision, value = std::move(value),
             apply = std::move(apply)]() mutable {
                if (!self || self->m_impl->sessionEpoch != currentSessionEpoch ||
                    !self->m_impl->admitted(currentEpoch, expectedRoot, revision))
                    return;
                apply(*self->m_impl, value);
            },
            Qt::QueuedConnection);
    };
    const auto push = [self, queue = eventQueue, currentEpoch, currentSessionEpoch = sessionEpoch, expectedRoot,
                       revision](EnginePush value) {
        if (!queue->active.load())
            return;
        const std::size_t bytes =
            std::visit([](const auto& event) { return event.data.dump().size() * 2U + 1024U; }, value);
        bool wake = false;
        {
            std::scoped_lock lock(queue->mutex);
            if (!queue->active.load())
                return;
            if (bytes > 1024U * 1024U) {
                ++queue->dropped;
            } else {
                while (!queue->pending.empty() &&
                       (queue->pending.size() >= 512 || queue->bytes + bytes > 4U * 1024U * 1024U)) {
                    queue->bytes -= queue->pending.front().bytes;
                    if (std::holds_alternative<engine::EngineState>(queue->pending.front().value)) {
                        if (queue->coalescedState)
                            ++queue->dropped;
                        queue->coalescedState = std::move(queue->pending.front());
                    } else {
                        ++queue->dropped;
                    }
                    queue->pending.pop_front();
                }
                queue->bytes += bytes;
                queue->pending.push_back({std::move(value), bytes, ++queue->nextOrder});
            }
            if (!queue->scheduled) {
                queue->scheduled = true;
                wake = true;
            }
        }
        if (wake && self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, queue, currentEpoch, currentSessionEpoch, expectedRoot, revision] {
                    if (self && self->m_impl->eventQueue == queue &&
                        self->m_impl->sessionEpoch == currentSessionEpoch &&
                        self->m_impl->admitted(currentEpoch, expectedRoot, revision))
                        self->m_impl->drainPushEvents();
                },
                Qt::QueuedConnection);
    };
    pushSink = push;
    engine::EngineSessionCallbacks callbacks;
    const auto nativeLease = nativeHost;
    callbacks.connected = [post, sharedPolicy, rootKey, crashHolder,
                           nativeLease](const engine::EngineIdentity& observed) {
        std::string schemaError;
        if (nativeLease && (!nativeLease->active.load() || nativeLease->pid.load() <= 0 ||
                            observed.processId != nativeLease->pid.load()))
            schemaError = "Authenticated engine PID differs from its exact native DAP launch";
        {
            std::scoped_lock lock(sharedPolicy->mutex);
            sharedPolicy->policy.reset();
            sharedPolicy->context = {};
            auto schema = engine::parseSchemaDocument(observed.schema);
            if (schema && schemaError.empty()) {
                sharedPolicy->policy =
                    std::make_unique<engine::CommandPolicy>(std::move(*schema), rootKey, sessionKey(observed));
                auto& context = sharedPolicy->context;
                context.connected = true;
                context.paired = true;
                context.trusted = true;
                // Every non-bootstrap request comes from an explicit toolbar/console/run action.
                context.mutationAllowed = true;
                context.editorAvailable = observed.role == "editor";
                context.selectedProject = rootKey;
                context.sessionProject = lang::documentKey(observed.projectPath);
                context.selectedSession = sessionKey(observed);
                context.sessionId = sessionKey(observed);
                context.capabilities = observed.capabilities;
                context.availableCommands = observed.availableCommands;
            } else if (!schema) {
                schemaError = schema.error().message();
            }
        }
        if (schemaError.empty())
            crashHolder->authenticated(observed);
        post(observed, [schemaError = std::move(schemaError), nativeLease](Impl& impl,
                                                                           const engine::EngineIdentity& value) {
            if (nativeLease && (impl.nativeHost != nativeLease || !nativeLease->active.load()))
                return;
            if (impl.selectedInstance != value.instanceId)
                return;
            if (!schemaError.empty()) {
                impl.stopOwnedHost();
                impl.clearSession(false);
                impl.lastError = schemaError;
                impl.setStatus(qs(schemaError));
                return;
            }
            impl.identity = value;
            const auto processId = value.processId > 0 ? value.processId : static_cast<std::int64_t>(impl.ownedPid);
            impl.nativeTarget.reset();
            impl.retainedNativeBinding.reset();
            impl.nativeBoundBinding.reset();
            if (processId > 0)
                impl.nativeTarget = EngineNativeDebugTarget{
                    processId,         value.instanceId,  value.generation, impl.root, value.connectionGeneration,
                    impl.sessionEpoch, impl.trustRevision};
            impl.state.reset();
            impl.eventHistory.clear();
            impl.eventHistoryBytes = 0;
            impl.eventHistoryDropped = 0;
            impl.lastEventSequence = 0;
            impl.selectedServerGeneration = value.generation;
            impl.lastError.clear();
            impl.awaitingOwnedAuthentication = false;
            if (impl.startupWatch)
                impl.startupWatch->stop();
            impl.indicator->setState(ui::EngineConnectionState::Connected);
            impl.setStatus(QStringLiteral("Authenticated with engine %1").arg(qs(value.instanceId)));
            if (impl.nativeDebug)
                impl.nativeDebug->refresh();
            impl.updateControls();
            emit impl.owner.crashSourceObserved();
        });
    };
    callbacks.state = [push, sharedPolicy](const engine::EngineState& observed) {
        {
            std::scoped_lock lock(sharedPolicy->mutex);
            if (sharedPolicy->context.sessionId == sessionKey(observed.identity))
                sharedPolicy->context.state = observed.state;
        }
        push(observed);
    };
    callbacks.log = [push](const engine::EngineLogEntry& value) { push(value); };
    callbacks.diagnostic = [push](const engine::EngineDiagnostic& value) { push(value); };
    callbacks.disconnected = [post, sharedPolicy, crashHolder](const engine::EngineDisconnect& observed) {
        {
            std::scoped_lock lock(sharedPolicy->mutex);
            sharedPolicy->context.connected = false;
            sharedPolicy->context.state.clear();
            sharedPolicy->policy.reset();
        }
        const bool unexpected =
            observed.cause == engine::EngineDisconnectCause::Unexpected &&
            (observed.error.code() == ErrorCode::IoError || observed.error.code() == ErrorCode::Timeout);
        (void)crashHolder->lost(observed.identity, unexpected);
        post(observed, [](Impl& impl, const engine::EngineDisconnect& value) {
            if (impl.identity && !impl.currentIdentity(value.identity))
                return;
            const bool unexpected =
                value.cause == engine::EngineDisconnectCause::Unexpected &&
                (value.error.code() == ErrorCode::IoError || value.error.code() == ErrorCode::Timeout);
            if (unexpected)
                impl.observedCrashLoss(value.identity);
            else
                impl.revokeCrashObservation();
            const auto* registry = impl.workbench.m_debugSessions.get();
            const auto* binding = impl.nativeBoundBinding ? &*impl.nativeBoundBinding : nullptr;
            const auto* controller = binding && registry ? registry->controller(binding->registrySessionId) : nullptr;
            const auto unique =
                registry && binding ? registry->nativeForPid(binding->target.pid) : ui::DebugSessionEntry{};
            const auto* native = controller && controller->nativeSession() ? controller->session() : nullptr;
            if (impl.identity && impl.nativeTarget && binding && binding->target == *impl.nativeTarget && controller &&
                controller == binding->controller && unique.id == binding->registrySessionId &&
                unique.controller == controller && controller->sessionGeneration() == binding->debugGeneration &&
                native && controller->adapterStoppedEvidence() && native->processId() == impl.nativeTarget->pid) {
                impl.retainedNativeBinding = *binding;
            } else if (!impl.retainedNativeBinding || !registry ||
                       registry->controller(impl.retainedNativeBinding->registrySessionId) !=
                           impl.retainedNativeBinding->controller ||
                       !impl.retainedNativeBinding->controller ||
                       impl.retainedNativeBinding->controller->sessionGeneration() !=
                           impl.retainedNativeBinding->debugGeneration) {
                impl.nativeTarget.reset();
                impl.retainedNativeBinding.reset();
            }
            impl.identity.reset();
            impl.state.reset();
            impl.requests.clear();
            impl.lastError = value.error.message();
            const auto code = value.error.code();
            const auto indicatorState = code == ErrorCode::Unsupported ? ui::EngineConnectionState::IncompatibleContract
                                                                       : ui::EngineConnectionState::Disconnected;
            impl.indicator->setState(indicatorState, qs(value.error.message()));
            impl.setStatus(qs(value.error.message()) +
                           QStringLiteral(" Refresh and select Pair to choose a replacement editor."));
            impl.observeEvent("disconnected", lang::Json::object().set("reason", value.error.message()));
            if (!value.reconnecting)
                impl.stopOwnedHost();
            impl.updateControls();
        });
    };
    indicator->setState(ui::EngineConnectionState::Connecting);
    setStatus(QStringLiteral("Authenticating the selected editor..."));
    auto created = engine::EngineSession::create(std::move(options), std::move(callbacks));
    if (!created) {
        clearSession(false);
        return std::unexpected(created.error());
    }
    session = std::move(*created);
    updateControls();
    return {};
}

Result<std::uint64_t> EngineIntegration::Impl::send(std::string method, lang::Json params,
                                                    std::function<void(Result<lang::Json>)> completed) {
    if (!admitted(epoch, root, trustRevision))
        return fail(ErrorCode::Untrusted, "Project trust changed before the engine command");
    if (!session || !identity || !session->connected())
        return fail(ErrorCode::Busy, "Pair an editor before sending commands");
    if (nativeDebug && nativeDebug->paused() && method == "play.stop")
        return fail(ErrorCode::Unsupported, "Stop a native debugger through its Debug Stop action");
    {
        std::scoped_lock lock(policy->mutex);
        auto observation = policy->context;
        if (policy->nativePaused.load())
            observation.state = "DebuggerPaused";
        auto allowed =
            policy->policy
                ? policy->policy->admit(method, params, observation)
                : Result<void>(fail(ErrorCode::PermissionDenied, "No authenticated command policy is available"));
        if (!allowed) {
            lastError = allowed.error().message();
            return std::unexpected(allowed.error());
        }
    }
    auto observed = *identity;
    observed.schema = lang::Json{};
    std::vector<std::string>{}.swap(observed.availableCommands);
    const auto currentEpoch = epoch;
    const auto expectedRoot = root;
    const auto revision = trustRevision;
    auto* fs = &workbench.m_services.fs;
    const auto originalKey = projectKey;
    const auto admission = physicalAdmission;
    auto* exactSession = session.get();
    QPointer<EngineIntegration> self(&owner);
    auto requestId = std::make_shared<std::uint64_t>(0);
    auto started = session->request(
        method, std::move(params),
        [self, fs, originalKey, admission, observed, currentEpoch, expectedRoot, revision, exactSession, requestId,
         method, completed = std::move(completed)](Result<lang::Json> result) mutable {
            if (!self || !retainEngineProjectIdentity(*fs, expectedRoot, originalKey, admission))
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, observed, currentEpoch, expectedRoot, revision, exactSession, requestId, method,
                 result = std::move(result), completed = std::move(completed)]() mutable {
                    if (!self)
                        return;
                    auto& impl = *self->m_impl;
                    if (!impl.admitted(currentEpoch, expectedRoot, revision) || impl.session.get() != exactSession ||
                        !impl.currentIdentity(observed))
                        return;
                    impl.requests.erase(*requestId);
                    impl.lastCompletedRequest = *requestId;
                    if (impl.lastRequest == *requestId) {
                        impl.lastReply = result ? *result : lang::Json{};
                        impl.lastError = result ? std::string{} : result.error().message();
                    }
                    impl.updateControls();
                    EngineCommandReply reply;
                    reply.identity = observed;
                    reply.identity.schema = lang::Json{};
                    reply.identity.availableCommands.clear();
                    reply.method = method;
                    reply.success = result.has_value();
                    reply.message = result ? result->dump() : result.error().message();
                    reply.data = result ? *result : lang::Json::object().set("message", result.error().message());
                    if (impl.pushSink)
                        impl.pushSink(std::move(reply));
                    impl.setStatus(result ? QStringLiteral("Command completed") : qs(result.error().message()));
                    // Other engine operations may change content; retain no complete usage
                    // snapshot across them. External edits have no content event in contract v2,
                    // so retained counts are explicitly labelled query snapshots, not live values.
                    if (result && method != "assets.find-references")
                        impl.clearAssetUsages();
                    // Command replies never change the toolbar's play/paused/frame state.
                    if (completed)
                        completed(std::move(result));
                },
                Qt::QueuedConnection);
        });
    if (!started) {
        lastError = started.error().message();
        return std::unexpected(started.error());
    }
    *requestId = *started;
    lastRequest = *started;
    requests.emplace(*started, method);
    updateControls();
    lastReply = lang::Json{};
    lastError.clear();
    return started;
}

lang::Json EngineIntegration::snapshot() const {
    const auto& impl = *m_impl;
    const bool current = impl.admitted(impl.epoch, impl.root, impl.trustRevision);
    auto out = lang::Json::object()
                   .set("available", impl.available)
                   .set("physicalRootValid", impl.physicalAdmission->load())
                   .set("discoveryBusy", impl.discoveryBusy)
                   .set("selectedInstanceId", impl.selectedInstance)
                   .set("connected", current && impl.session && impl.identity && impl.session->connected())
                   .set("processId", current && impl.session && impl.session->connected() && impl.identity
                                         ? impl.identity->processId
                                         : std::int64_t{0})
                   .set("state", current && impl.state ? lang::Json(impl.state->state) : lang::Json{})
                   .set("playing", current && impl.state ? lang::Json(impl.state->playing) : lang::Json{})
                   .set("paused", current && impl.state ? lang::Json(impl.state->paused) : lang::Json{})
                   .set("frame", current && impl.state ? lang::Json(impl.state->frame) : lang::Json{})
                   .set("playGeneration", current && impl.state ? lang::Json(impl.state->playGeneration) : lang::Json{})
                   .set("operationId", current && impl.state ? lang::Json(impl.state->operationId) : lang::Json{})
                   .set("phase", current && impl.state ? lang::Json(impl.state->phase) : lang::Json{})
                   .set("operationError", current && impl.state ? lang::Json(impl.state->operationError) : lang::Json{})
                   .set("eventSequence", impl.lastEventSequence)
                   .set("lastRequestId", impl.lastRequest)
                   .set("lastCompletedRequestId", impl.lastCompletedRequest)
                   .set("pendingRequestCount", static_cast<std::uint64_t>(impl.requests.size()))
                   .set("ownedRunId", impl.ownedRun.value_or(0))
                   .set("ownedPid", static_cast<std::uint64_t>(impl.ownedPid))
                   .set("launchReserved", impl.runAdmission && impl.runAdmission->active.load())
                   .set("awaitingAuthentication", impl.awaitingOwnedAuthentication)
                   .set("role", impl.identity ? impl.identity->role : std::string{})
                   .set("nativeDebuggerMatched", impl.nativeDebug && impl.nativeDebug->matched())
                   .set("debuggerPaused", impl.nativeDebug && impl.nativeDebug->matched()
                                              ? lang::Json(impl.nativeDebug->paused())
                                              : lang::Json{})
                   .set("lastReply", impl.lastReply)
                   .set("lastError", impl.lastError)
                   .set("sentCommandCount", impl.session ? impl.session->sentCommandCount() : std::uint64_t{0});
    auto instances = lang::Json::array();
    for (const auto& instance : impl.instances)
        if (current)
            instances.push(lang::Json::object()
                               .set("instanceId", instance.instanceId)
                               .set("projectPath", instance.projectRoot)
                               .set("port", static_cast<std::uint64_t>(instance.port)));
    auto events = lang::Json::array();
    for (const auto& event : impl.eventHistory)
        if (current)
            events.push(event.first);
    out.set("instances", std::move(instances)).set("events", std::move(events));
    {
        std::scoped_lock lock(impl.eventQueue->mutex);
        out.set("queuedEventBytes", static_cast<std::uint64_t>(impl.eventQueue->bytes))
            .set("droppedEventCount", impl.eventQueue->dropped);
    }
    out.set("eventHistoryDropped", impl.eventHistoryDropped)
        .set("consoleEntryCount",
             impl.console ? static_cast<std::uint64_t>(impl.console->retainedEntries()) : std::uint64_t{0});
    out.set("connectionGeneration", impl.identity ? impl.identity->connectionGeneration : std::uint64_t{0})
        .set("serverGeneration", impl.identity ? impl.identity->generation : std::string{})
        .set("heartbeatIntervalMs", impl.identity ? impl.identity->heartbeatIntervalMs : std::uint64_t{0})
        .set("heartbeatTimeoutMs", impl.identity ? impl.identity->heartbeatTimeoutMs : std::uint64_t{0});
    auto content = lang::Json::object().set("available", current && impl.projectContent.has_value());
    if (current && impl.projectContent) {
        auto assets = lang::Json::array();
        for (const auto& asset : impl.projectContent->assets)
            assets.push(lang::Json::object()
                            .set("path", asset.path)
                            .set("type", asset.type)
                            .set("handle", std::to_string(asset.handle.value)));
        content.set("assets", std::move(assets));
        content.set("assetCount", static_cast<std::uint64_t>(impl.projectContent->assets.size()))
            .set("sceneCount", static_cast<std::uint64_t>(impl.projectContent->sceneCount()))
            .set("projectFile", impl.projectContent->projectFile);
    }
    out.set("engineProject", std::move(content));
    return out;
}

} // namespace frost::app
