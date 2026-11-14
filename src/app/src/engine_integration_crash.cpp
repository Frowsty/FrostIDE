// SPDX-License-Identifier: Apache-2.0
// Cached authenticated crash-source capture and historical physical-project admission.
#include "frost/lang/uri.hpp"

#include "engine_crash_observer.hpp"
#include "engine_integration_internal.hpp"

namespace frost::app {

void EngineIntegration::Impl::revokeCrashObservation() {
    crashSessions->revoke();
    crashSessions = std::make_shared<EngineCrashSessionHolder>();
    emit owner.crashSourceRevoked();
}

void EngineIntegration::Impl::observedCrashLoss(const engine::EngineIdentity& observed) {
    const auto lease = crashSessions->snapshot();
    if (!lease || !lease->admitted())
        return;
    const auto& identity = lease->identity();
    if (identity.instanceId != observed.instanceId || identity.generation != observed.generation ||
        identity.connectionGeneration != observed.connectionGeneration ||
        identity.projectPath != observed.projectPath || identity.processId != observed.processId ||
        identity.crashArtifactSource != observed.crashArtifactSource)
        return;
    emit owner.crashSourceUnexpectedLoss(observed);
}

Result<EngineCrashObservationSource> EngineIntegration::captureCrashObservationSource() const {
    const auto& impl = *m_impl;
    const auto lease = impl.crashSessions->snapshot();
    if (!impl.identity || !lease || !lease->live() || !impl.currentIdentity(lease->identity()))
        return fail(ErrorCode::Cancelled, "Pair a live authenticated engine before capturing its crash source");
    if (!lease->identity().crashArtifactSource)
        return fail(ErrorCode::Unsupported, "This authenticated engine does not advertise local crash observation");
    if (!impl.admitted(impl.epoch, impl.root, impl.trustRevision) || impl.physicalRoot.empty() ||
        lang::documentKey(lease->identity().projectPath) != impl.projectKey)
        return fail(ErrorCode::Untrusted, "Crash observation requires the current trusted physical project");
    auto* trust = &impl.workbench.m_services.trust;
    auto* fs = &impl.workbench.m_services.fs;
    const auto root = impl.root;
    const auto key = impl.projectKey;
    const auto physicalRoot = impl.physicalRoot;
    const auto revision = impl.trustRevision;
    const auto physical = impl.physicalAdmission;
    EngineCrashObservationSource source;
    source.identity = lease->identity();
    source.live = [lease] { return lease->live(); };
    source.admission.projectRoot = physicalRoot;
    source.admission.current = [lease, physical, trust, root, revision]() -> Result<void> {
        const auto trusted = trust->snapshot(root);
        if (!lease->admitted() || !physical->load() || !trusted.trusted || trusted.revision != revision)
            return fail(ErrorCode::PermissionDenied, "The captured crash source or its project trust changed");
        return {};
    };
    source.admission.validatePhysical = [fs, root, key, physical,
                                         current = source.admission.current]() -> Result<void> {
        if (auto valid = current(); !valid)
            return valid;
        if (!retainEngineProjectIdentity(*fs, root, key, physical))
            return fail(ErrorCode::PermissionDenied, "The captured crash source physical project changed");
        return current();
    };
    return source;
}

} // namespace frost::app
