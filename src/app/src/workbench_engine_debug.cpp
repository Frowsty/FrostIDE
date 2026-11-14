// SPDX-License-Identifier: Apache-2.0
// Bridges matching native DAP events to a local engine UI overlay and explicit DAP Stop.
#include "frost/io/path.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"

#include "engine_integration_internal.hpp"
#include "engine_native_debug.hpp"

#include <utility>

namespace frost::app {

std::optional<EngineNativeDebugBinding> EngineIntegration::nativeDebugBinding() const {
    const auto& impl = *m_impl;
    if (!impl.identity || !impl.session || !impl.session->connected() ||
        !impl.admitted(impl.epoch, impl.root, impl.trustRevision) || !impl.nativeDebug)
        return std::nullopt;
    auto binding = impl.nativeDebug->binding();
    if (!binding || !impl.nativeTarget || binding->target != *impl.nativeTarget ||
        binding->target.sessionEpoch != impl.sessionEpoch || binding->target.trustRevision != impl.trustRevision ||
        binding->target.instanceId != impl.identity->instanceId ||
        binding->target.serverGeneration != impl.identity->generation ||
        binding->target.connectionGeneration != impl.identity->connectionGeneration)
        return std::nullopt;
    return binding;
}

EngineNativeDebugAdapter::EngineNativeDebugAdapter(ui::DebugSessionRegistry& registry, TargetSupplier target,
                                                   Changed changed, QObject* parent)
    : QObject(parent), m_registry(&registry), m_target(std::move(target)), m_changed(std::move(changed)) {
    connect(&registry, &ui::DebugSessionRegistry::sessionStateChanged, this, [this](ui::DebugSessionId) { refresh(); });
    connect(&registry, &ui::DebugSessionRegistry::sessionsChanged, this, &EngineNativeDebugAdapter::refresh);
    connect(&registry, &ui::DebugSessionRegistry::sessionRemoving, this, [this](ui::DebugSessionId) { refresh(); });
    connect(&registry, &QObject::destroyed, this, [this] { refresh(); });
    capture();
    m_observed = current();
}

EngineNativeDebugAdapter::EngineNativeDebugAdapter(ui::DebugController& controller, TargetSupplier target,
                                                   Changed changed, QObject* parent)
    : QObject(parent), m_legacyController(&controller), m_target(std::move(target)), m_changed(std::move(changed)) {
    connect(&controller, &ui::DebugController::stateChanged, this, &EngineNativeDebugAdapter::refresh);
    connect(&controller, &QObject::destroyed, this, [this] { refresh(); });
    // Construction does not reenter the caller's partially initialized coordinator.
    capture();
    m_observed = current();
}

EngineNativeDebugAdapter::~EngineNativeDebugAdapter() = default;

void EngineNativeDebugAdapter::capture() {
    auto target = m_target ? m_target() : std::nullopt;
    if (target != m_admittedTarget) {
        m_admittedTarget = target;
        m_binding.reset();
    }
    if (m_binding || !target)
        return;
    if (!target || target->pid <= 0 || target->instanceId.empty() || target->serverGeneration.empty() ||
        !io::isAbsolutePath(target->projectRoot) || target->connectionGeneration == 0 || target->sessionEpoch == 0 ||
        target->trustRevision == 0)
        return;
    ui::DebugController* controller = m_legacyController.data();
    std::uint64_t id = 0;
    if (m_registry) {
        const auto entry = m_registry->nativeForPid(target->pid);
        controller = entry.controller;
        id = entry.id;
    }
    if (!controller || !controller->nativeSession())
        return;
    const auto* session = controller->session();
    if (!session || session->processId() != target->pid ||
        (session->state() != debug::DebugState::Running && session->state() != debug::DebugState::Stopped))
        return;
    m_binding = EngineNativeDebugBinding{std::move(*target), id, controller->sessionGeneration(), controller};
}

std::optional<EngineNativeDebugAdapter::Match> EngineNativeDebugAdapter::current() const {
    if (!m_binding || !m_binding->controller || !m_target || m_target() != m_binding->target)
        return std::nullopt;
    auto* controller = m_binding->controller.data();
    if (m_registry) {
        const auto entry = m_registry->nativeForPid(m_binding->target.pid);
        if (entry.id != m_binding->registrySessionId || entry.controller != controller ||
            m_registry->controller(m_binding->registrySessionId) != controller)
            return std::nullopt;
    } else if (m_binding->registrySessionId != 0 || m_legacyController != controller) {
        return std::nullopt;
    }
    if (!controller->nativeSession() || controller->sessionGeneration() != m_binding->debugGeneration)
        return std::nullopt;
    const auto* session = controller->session();
    if (!session || session->processId() != m_binding->target.pid)
        return std::nullopt;
    const auto state = session->state();
    if (state != debug::DebugState::Running && state != debug::DebugState::Stopped)
        return std::nullopt;
    return Match{*m_binding, controller->adapterStoppedEvidence(), session->canTerminateDebuggee()};
}

void EngineNativeDebugAdapter::refresh() {
    capture();
    auto observed = current();
    if (observed == m_observed)
        return;
    m_observed = std::move(observed);
    // Keep a local callback copy in case the consumer replaces the adapter during notification.
    const auto changed = m_changed;
    if (changed)
        changed();
}

bool EngineNativeDebugAdapter::matched() const { return current().has_value(); }
bool EngineNativeDebugAdapter::paused() const {
    const auto observed = current();
    return observed && observed->paused;
}
bool EngineNativeDebugAdapter::canTerminate() const {
    const auto observed = current();
    return observed && observed->canTerminate;
}
std::optional<EngineNativeDebugBinding> EngineNativeDebugAdapter::binding() const {
    const auto observed = current();
    return observed ? std::optional(observed->binding) : std::nullopt;
}

Result<void> EngineNativeDebugAdapter::terminate(Completion done) {
    const auto observed = current();
    if (!observed)
        return fail(ErrorCode::PermissionDenied, "No native debugger matches this authenticated engine instance");
    if (!observed->canTerminate)
        return fail(ErrorCode::Unsupported, "This native debugger cannot safely terminate the selected engine");
    QPointer<EngineNativeDebugAdapter> self(this);
    auto result = observed->binding.controller->terminateTarget(
        observed->binding.target.pid, observed->binding.debugGeneration,
        [self, binding = observed->binding, done = std::move(done)](Result<void> response) mutable {
            if (!self)
                return;
            const auto current = self->binding();
            if (!current || *current != binding)
                return;
            self->refresh();
            if (done)
                done(std::move(response));
        });
    refresh();
    return result;
}

} // namespace frost::app
