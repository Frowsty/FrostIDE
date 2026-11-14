// SPDX-License-Identifier: Apache-2.0
// Native LLDB launch ownership for one mixed engine package; no synthetic RunId or external adoption.
#include "engine_integration_internal.hpp"
#include "engine_native_host_internal.hpp"
#include "engine_run_debug.hpp"

#include <algorithm>
#include <limits>

namespace frost::app {
namespace {
ui::DebugController* nativeController(const std::shared_ptr<EngineNativeHostState>& host) {
    auto* c = host && host->registry ? host->registry->controller(host->session) : nullptr;
    return c && c->sessionGeneration() == host->generation && c->session() && c->nativeSession() &&
                   c->session()->targetMode() == debug::DebugTargetMode::Launch
               ? c
               : nullptr;
}
} // namespace
bool nativeEngineHostCurrent(const std::shared_ptr<EngineNativeHostState>& host) {
    auto* c = nativeController(host);
    if (!c || c->state() == debug::DebugState::Ended)
        return false;
    const auto pid = host->pid.load();
    return pid == 0 || c->session()->processId() == pid;
}
Result<void> stopNativeEngineHost(const std::shared_ptr<EngineNativeHostState>& host) {
    if (!host || host->session == 0)
        return {}; // Reserved preparation has not launched a host.
    auto* c = nativeController(host);
    if (!c)
        return fail(ErrorCode::Cancelled, "The exact native-owned engine controller ended");
    if (host->pid.load() > 0 && c->session()->processId() != host->pid.load())
        return fail(ErrorCode::PermissionDenied, "The native-owned engine process identity changed");
    c->stop();
    return {}; // Actual Launch-owned disconnect/termination; never an attach's host.
}
void EngineIntegration::Impl::stopOwnedHost() {
    if (ownedRun && workbench.m_runManager)
        workbench.m_runManager->stop(*ownedRun);
    if (nativeHost)
        (void)stopNativeEngineHost(nativeHost);
}
Result<EngineNativeHostLaunch> EngineIntegration::reserveNativeHost(const run::RunConfiguration& configuration) {
    auto& impl = *m_impl;
    if (configuration.engineDebugMode != run::EngineDebugMode::NativeAndLua)
        return fail(ErrorCode::InvalidArgument, "Choose explicit Native + Lua for a native engine host");
    if (auto valid = run::validateRunConfiguration(configuration); !valid)
        return std::unexpected(valid.error());
    if (impl.nativeHostReservation == std::numeric_limits<std::uint64_t>::max())
        return fail(ErrorCode::OutOfRange, "Native engine reservation identity exhausted");
    auto transient = engineDebugHostConfiguration(configuration);
    if (!transient)
        return std::unexpected(transient.error());
    auto prepared = impl.prepare(*transient);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto host = std::make_shared<EngineNativeHostState>();
    host->token = {++impl.nativeHostReservation};
    host->declared = configuration;
    host->admission = impl.runAdmission;
    impl.nativeHost = host;
    return EngineNativeHostLaunch{host->token,
                                  std::move(*prepared),
                                  impl.root,
                                  launchAdmissionFor(configuration),
                                  launchPlanFor(configuration),
                                  impl.workStop.get_token()};
}
Result<void> EngineIntegration::bindNativeHost(EngineNativeHostToken token, ui::DebugSessionRegistry& registry,
                                               ui::DebugSessionId session, std::uint64_t generation,
                                               ui::DebugGroupId group) {
    auto& impl = *m_impl;
    const auto host = impl.nativeHost;
    if (!host || host->token != token || !host->active.load() || !impl.runAdmission ||
        !impl.runAdmission->active.load())
        return fail(ErrorCode::Cancelled, "Native engine reservation ended before host binding");
    if (host->session != 0)
        return fail(ErrorCode::Busy, "Native engine reservation is already bound");
    auto* c = registry.controller(session);
    const auto entries = registry.sessions();
    const auto entry =
        std::ranges::find_if(entries, [&](const auto& e) { return e.id == session && e.group == group; });
    if (!c || generation == 0 || c->sessionGeneration() != generation || entry == entries.end() ||
        !c->nativeOsThreadIdsVerified() || !c->session() ||
        c->session()->targetMode() != debug::DebugTargetMode::Launch)
        return fail(ErrorCode::PermissionDenied,
                    "Native engine binding requires this group's actual built-in LLDB launch");
    host->registry = &registry;
    host->session = session;
    host->generation = generation;
    host->group = group;
    connect(&registry, &ui::DebugSessionRegistry::explicitStopRequested, this,
            [this, weak = std::weak_ptr(host), session, generation](ui::DebugSessionId requested) {
                const auto host = weak.lock();
                if (requested == session && host && m_impl->nativeHost == host && host->active.load() &&
                    host->generation == generation && host->registry && host->registry->controller(session) &&
                    host->registry->controller(session)->sessionGeneration() == generation)
                    m_impl->revokeCrashObservation();
            });
    return {};
}
Result<EngineNativePrivateOutput> EngineIntegration::nativeHostOutput(EngineNativeHostToken token) const {
    const auto host = m_impl->nativeHost;
    if (!host || host->token != token || !host->active.load())
        return fail(ErrorCode::Cancelled, "Native engine reservation ended");
    return nativeEngineHostOutput(host);
}
EngineNativePrivateOutput nativeEngineHostOutput(std::shared_ptr<EngineNativeHostState> host) {
    return [host = std::move(host)](std::string_view category, std::string_view bytes) {
        if (!host || !host->active.load() || host->discardRaw.load() || bytes.empty())
            return;
        // Never retain an adapter-controlled category string. Four fixed streams also bound the
        // GUI parser's partial-record map when a peer sends arbitrary categories without newlines.
        const std::string_view stream = category == "stdout"           ? "stdout"
                                        : category == "stderr"         ? "stderr"
                                        : category == "adapter-stderr" ? "adapter-stderr"
                                                                       : "other";
        const std::lock_guard lock(host->mutex);
        if (!host->active.load() || host->discardRaw.load() || host->overflow)
            return;
        if (host->output.size() >= EngineNativeHostState::maxOutputEvents ||
            bytes.size() > EngineNativeHostState::maxOutputBytes - stream.size() ||
            host->bytes > EngineNativeHostState::maxOutputBytes - stream.size() - bytes.size()) {
            host->overflow = true;
            return;
        }
        host->bytes += stream.size() + bytes.size();
        host->output.emplace_back(std::string(stream), std::string(bytes));
    };
}
Result<EngineNativeHostObservation> EngineIntegration::observeNativeHost(EngineNativeHostToken token) {
    auto& impl = *m_impl;
    const auto host = impl.nativeHost;
    if (!host || host->token != token || !host->active.load() || !host->admission->active.load())
        return fail(ErrorCode::Cancelled, "Native engine reservation ended");
    if (!impl.admitted(impl.epoch, impl.root, impl.trustRevision))
        return fail(ErrorCode::Untrusted, "Native engine admission changed");
    {
        const std::lock_guard lock(host->mutex);
        if (host->overflow)
            return fail(ErrorCode::Corrupt, "Native engine startup exceeded the private output limit");
    }
    if (!nativeEngineHostCurrent(host))
        return fail(ErrorCode::Cancelled, "Native-owned engine controller changed");
    auto* c = nativeController(host);
    const auto process = c->session()->processId();
    if (process && *process > 0) {
        if (static_cast<std::uint64_t>(*process) > std::numeric_limits<io::ProcessId>::max())
            return fail(ErrorCode::Corrupt, "Native engine PID is outside the OS process identifier range");
        if (host->pid.load() != 0 && host->pid.load() != *process)
            return fail(ErrorCode::PermissionDenied, "Native-owned engine PID changed");
        host->pid.store(*process);
        impl.ownedPid = static_cast<io::ProcessId>(*process);
        impl.ownedProcessStarted = true;
    }
    return EngineNativeHostObservation{process.value_or(0), c->configured(), c->adapterStoppedEvidence()};
}
void EngineIntegration::pauseNativeHostBootstrap(EngineNativeHostToken token) {
    const auto host = m_impl->nativeHost;
    if (!host || host->token != token || !nativeEngineHostCurrent(host))
        return;
    auto* c = nativeController(host);
    if (!c->adapterStoppedEvidence())
        return;
    // Frozen preauthentication leases are cancelled, never extended. A resumed host uses a fresh
    // HMAC challenge against its same private endpoint; already authenticated R6 pause handling stays.
    if (!m_impl->identity && m_impl->session) {
        m_impl->clearSession(false);
        m_impl->awaitingOwnedAuthentication = false;
        m_impl->awaitingOwnedEndpoint = false;
    }
}
void EngineIntegration::revokeNativeHost(EngineNativeHostToken token, bool preserveUnexpectedLoss) {
    auto& impl = *m_impl;
    const auto host = impl.nativeHost;
    if (!host || host->token != token)
        return;
    const auto crashLease = impl.crashSessions->snapshot();
    const bool preserve = preserveUnexpectedLoss && crashLease && crashLease->admitted() && !crashLease->live() &&
                          host->pid.load() > 0 && crashLease->identity().processId == host->pid.load();
    (void)stopNativeEngineHost(host);
    host->active.store(false);
    host->discardRaw.store(true);
    host->admission->active.store(false);
    impl.nativeHost.reset();
    impl.runAdmission.reset();
    impl.ownedPid = 0;
    impl.ownedProcessStarted = false;
    impl.ownedSecret.clear();
    impl.ownedOutput.clear();
    impl.awaitingOwnedEndpoint = false;
    impl.awaitingOwnedAuthentication = false;
    impl.clearSession(false, preserve);
    impl.updateControls();
}
Result<EngineLuaDebugOwner> EngineIntegration::captureNativeLuaDebugOwner(EngineNativeHostToken token) {
    auto& impl = *m_impl;
    const auto host = impl.nativeHost;
    if (!host || host->token != token || !host->active.load() || !host->admission->active.load())
        return fail(ErrorCode::Cancelled, "Native engine reservation ended");
    auto observed = observeNativeHost(token);
    if (!observed)
        return std::unexpected(observed.error());
    const auto process = observed->processId;
    if (process <= 0 || !observed->configured)
        return fail(ErrorCode::Busy, "Waiting for actual native process/configuration acknowledgment");
    if (observed->nativeStopped)
        return fail(ErrorCode::Busy, "Native engine startup is paused; continue it explicitly to prepare Lua");
    if (!impl.identity && !impl.awaitingOwnedAuthentication) {
        impl.awaitingOwnedAuthentication = true;
        if (host->endpoint) {
            impl.awaitingOwnedEndpoint = false;
            auto connected = impl.connectInstance(*host->endpoint);
            if (!connected)
                return std::unexpected(connected.error());
        } else
            impl.awaitingOwnedEndpoint = true;
    }
    std::deque<std::pair<std::string, std::string>> output;
    bool overflow = false;
    {
        const std::lock_guard lock(host->mutex);
        overflow = host->overflow;
        output.swap(host->output);
        host->bytes = 0;
    }
    if (overflow)
        return fail(ErrorCode::Corrupt, "Native engine startup exceeded the private output limit");
    if (impl.awaitingOwnedEndpoint) {
        for (auto& [category, bytes] : output) {
            auto& partial = host->partial[category];
            std::size_t retained = 0;
            for (const auto& [stream, buffered] : host->partial)
                retained += buffered.size();
            if (bytes.size() > EngineNativeHostState::maxOutputBytes - retained)
                return fail(ErrorCode::Corrupt, "Native engine startup record exceeded the private limit");
            partial += bytes;
            for (auto end = partial.find('\n'); end != std::string::npos; end = partial.find('\n')) {
                impl.ownedOutput = partial.substr(0, end + 1);
                partial.erase(0, end + 1);
                impl.endpointFromOutput();
                if (!impl.awaitingOwnedEndpoint) {
                    host->discardRaw.store(true);
                    host->partial.clear();
                    break;
                }
            }
            if (!impl.awaitingOwnedEndpoint)
                break;
        }
    }
    if (impl.awaitingOwnedEndpoint || impl.awaitingOwnedAuthentication || !impl.identity || !impl.session ||
        !impl.session->connected())
        return fail(ErrorCode::Busy, "Waiting for the native-owned engine's mutual authentication");
    if (impl.identity->processId != process)
        return fail(ErrorCode::PermissionDenied, "Authenticated engine PID differs from actual native DAP PID");
    auto owner = captureLuaDebugOwner();
    if (!owner)
        return std::unexpected(owner.error());
    const auto expected = owner->identity;
    auto current = std::move(owner->current);
    QPointer<const EngineIntegration> self(this);
    owner->current = [self, host, expected, current = std::move(current)] {
        return self && self->m_impl->nativeHost == host && host->active.load() && host->admission->active.load() &&
               nativeEngineHostCurrent(host) && host->pid.load() == expected.processId && current && current();
    };
    auto admit = std::move(owner->admit);
    owner->admit = [host, expected, admit = std::move(admit)](const debug::LuaDebugIdentity& identity,
                                                              std::string_view method) -> Result<void> {
        if (!host->active.load() || !host->admission->active.load() || host->pid.load() != expected.processId)
            return fail(ErrorCode::Cancelled, "Native engine owner ended");
        return admit(identity, method);
    };
    return owner;
}
Result<void> EngineIntegration::stopNativeHost(EngineNativeHostToken token) {
    const auto host = m_impl->nativeHost;
    if (!host || host->token != token)
        return fail(ErrorCode::Cancelled, "Native engine reservation ended");
    m_impl->revokeCrashObservation();
    return stopNativeEngineHost(host);
}
void EngineIntegration::nativeHostEnded(EngineNativeHostToken token, const debug::DebugOutcome& outcome) {
    auto& impl = *m_impl;
    const auto host = impl.nativeHost;
    if (!host || host->token != token)
        return;
    const auto crashLease = impl.crashSessions->snapshot();
    const bool preserveCrash =
        preserveEngineCrashObservation(crashLease, host->pid.load(), outcome.stopped, outcome.exitCode);
    const bool observedExit = preserveCrash && crashLease->live() && !outcome.stopped && outcome.exitCode.has_value();
    if (observedExit) {
        (void)impl.crashSessions->lost(crashLease->identity(), true);
        impl.observedCrashLoss(crashLease->identity());
    }
    host->active.store(false);
    host->discardRaw.store(true);
    host->admission->active.store(false);
    impl.nativeHost.reset();
    impl.runAdmission.reset();
    impl.ownedPid = 0;
    impl.ownedProcessStarted = false;
    impl.ownedSecret.clear();
    impl.ownedOutput.clear();
    impl.awaitingOwnedEndpoint = false;
    impl.awaitingOwnedAuthentication = false;
    impl.clearSession(false, preserveCrash);
    impl.updateControls();
}
} // namespace frost::app
