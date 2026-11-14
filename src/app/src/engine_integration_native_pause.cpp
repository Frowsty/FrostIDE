// SPDX-License-Identifier: Apache-2.0
// Publish only exact registry/native DAP evidence to an authenticated R6 heartbeat lease.
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"

#include "engine_integration_internal.hpp"

namespace frost::app {
void EngineIntegration::updateNativeDebuggerPause() {
    auto& impl = *m_impl;
    const auto clear = [&] {
        const bool changed = impl.policy->nativePaused.exchange(false);
        impl.nativePauseLease.reset(); // Revocation never grants a continuation grace period.
        impl.nativePauseBinding.reset();
        if (changed)
            emit nativeDebuggerEvidenceChanged();
    };
    if (!impl.identity || !impl.session || !impl.session->connected() || !impl.nativeDebug ||
        !impl.admitted(impl.epoch, impl.root, impl.trustRevision)) {
        clear();
        return;
    }
    auto binding = impl.nativeDebug->binding();
    if (!binding || !binding->controller || !binding->controller->nativeSession() ||
        binding->target.pid != impl.identity->processId || binding->target.instanceId != impl.identity->instanceId ||
        binding->target.serverGeneration != impl.identity->generation ||
        binding->target.connectionGeneration != impl.identity->connectionGeneration ||
        binding->target.sessionEpoch != impl.sessionEpoch || binding->target.trustRevision != impl.trustRevision ||
        binding->target.projectRoot != impl.root ||
        binding->controller->sessionGeneration() != binding->debugGeneration) {
        clear();
        return;
    }
    const auto* registry = impl.workbench.m_debugSessions.get();
    const auto unique = registry ? registry->nativeForPid(binding->target.pid) : ui::DebugSessionEntry{};
    if (!registry || unique.id != binding->registrySessionId || unique.controller != binding->controller) {
        clear();
        return;
    }
    if (impl.nativePauseBinding != binding) {
        clear();
        auto lease = impl.session->bindNativeDebuggerPause(
            *impl.identity, {binding->registrySessionId, binding->debugGeneration, binding->target.pid});
        if (!lease)
            return; // No permission/identity fallback; the ordinary heartbeat rules remain.
        impl.nativePauseLease.emplace(std::move(*lease));
        impl.nativePauseBinding = binding;
    }
    const bool actualStopped = binding->controller->adapterStoppedEvidence();
    const bool previousStopped = impl.policy->nativePaused.exchange(actualStopped);
    if (previousStopped != actualStopped)
        emit nativeDebuggerEvidenceChanged();
    const auto sequence = binding->controller->adapterEvidenceSequence();
    if (!impl.nativePauseLease || sequence == 0)
        return; // No actual stopped/continued observation yet.
    const auto event = binding->controller->adapterStoppedEvidence() ? engine::NativeAdapterEvent::Stopped
                                                                     : engine::NativeAdapterEvent::Continued;
    // Outgoing resume requests do not alter either evidence sequence or actual stopped evidence.
    (void)impl.nativePauseLease->observe(
        {{binding->registrySessionId, binding->debugGeneration, binding->target.pid}, sequence, event});
}
} // namespace frost::app
