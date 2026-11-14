// SPDX-License-Identifier: Apache-2.0
// R7: authenticate the exact RUN-11 owned host before private Lua DAP preparation.
#include "engine_integration_internal.hpp"
#include <QPointer>

namespace frost::app {
void EngineIntegration::explicitRunStop(run::RunId id) {
    if (m_impl->ownedRun == id)
        m_impl->revokeCrashObservation();
}
Result<EngineLuaDebugOwner> EngineIntegration::captureOwnedLuaDebugOwner(run::RunId id) const {
    const auto& impl = *m_impl;
    if (id == 0 || impl.ownedRun != id || !impl.runAdmission || !impl.runAdmission->active.load())
        return fail(ErrorCode::Cancelled, "The exact owned engine run is no longer current");
    if (!impl.admitted(impl.epoch, impl.root, impl.trustRevision))
        return fail(ErrorCode::Untrusted, "The owned engine Debug admission changed");
    if (!impl.ownedProcessStarted || impl.awaitingOwnedEndpoint || impl.awaitingOwnedAuthentication || !impl.session ||
        !impl.session->connected() || !impl.identity)
        return fail(ErrorCode::Busy, "Waiting for the owned engine's authenticated session");
    auto owner = captureLuaDebugOwner();
    if (!owner)
        return std::unexpected(owner.error());
    const auto reservation = impl.runAdmission;
    auto current = std::move(owner->current);
    QPointer<const EngineIntegration> self(this);
    owner->current = [self, id, reservation, current = std::move(current)] {
        return self && self->m_impl->ownedRun == id && self->m_impl->runAdmission == reservation &&
               reservation->active.load() && current && current();
    };
    auto admit = std::move(owner->admit);
    owner->admit = [reservation, admit = std::move(admit)](const debug::LuaDebugIdentity& identity,
                                                           std::string_view method) -> Result<void> {
        if (!reservation->active.load())
            return fail(ErrorCode::Cancelled, "The owned engine run ended");
        return admit(identity, method);
    };
    return owner;
}
} // namespace frost::app
