// SPDX-License-Identifier: Apache-2.0
// Bridges matching native DAP events to a local engine UI overlay and explicit DAP Stop.
#include "frost/io/path.hpp"
#include "frost/ui/debug_controller.hpp"

#include "engine_native_debug.hpp"

#include <utility>

namespace frost::app {

EngineNativeDebugAdapter::EngineNativeDebugAdapter(ui::DebugController& controller, TargetSupplier target,
                                                   Changed changed, QObject* parent)
    : QObject(parent), m_controller(&controller), m_target(std::move(target)), m_changed(std::move(changed)) {
    connect(&controller, &ui::DebugController::stateChanged, this, &EngineNativeDebugAdapter::refresh);
    connect(&controller, &QObject::destroyed, this, [this] { refresh(); });
    // Construction does not reenter the caller's partially initialized coordinator.
    m_observed = current();
}

EngineNativeDebugAdapter::~EngineNativeDebugAdapter() = default;

std::optional<EngineNativeDebugAdapter::Match> EngineNativeDebugAdapter::current() const {
    if (!m_controller || !m_target || !m_controller->nativeSession())
        return std::nullopt;
    auto target = m_target();
    if (!target || target->pid <= 0 || target->instanceId.empty() || target->serverGeneration.empty() ||
        !io::isAbsolutePath(target->projectRoot) || target->connectionGeneration == 0)
        return std::nullopt;
    const auto* session = m_controller->session();
    if (!session || session->processId() != target->pid)
        return std::nullopt;
    const auto state = session->state();
    if (state != debug::DebugState::Running && state != debug::DebugState::Stopped)
        return std::nullopt;
    return Match{std::move(*target), m_controller->sessionGeneration(), state == debug::DebugState::Stopped,
                 session->canTerminateDebuggee()};
}

void EngineNativeDebugAdapter::refresh() {
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

Result<void> EngineNativeDebugAdapter::terminate(Completion done) {
    const auto observed = current();
    if (!observed)
        return fail(ErrorCode::PermissionDenied, "No native debugger matches this authenticated engine instance");
    if (!observed->canTerminate)
        return fail(ErrorCode::Unsupported, "This native debugger cannot safely terminate the selected engine");
    QPointer<EngineNativeDebugAdapter> self(this);
    auto result = m_controller->terminateTarget(
        observed->target.pid, observed->debugGeneration,
        [self, target = observed->target, generation = observed->debugGeneration,
         done = std::move(done)](Result<void> response) mutable {
            if (!self || !self->m_controller || !self->m_target || self->m_target() != target ||
                self->m_controller->sessionGeneration() != generation)
                return;
            self->refresh();
            if (done)
                done(std::move(response));
        });
    refresh();
    return result;
}

} // namespace frost::app
