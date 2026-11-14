// SPDX-License-Identifier: Apache-2.0
// Exact authenticated engine identity binding to an existing native DAP session, without polling.
#pragma once

#include "frost/core/error.hpp"

#include <QObject>
#include <QPointer>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace frost::ui {
class DebugController;
class DebugSessionRegistry;
} // namespace frost::ui

namespace frost::app {

// The supplier must obtain PID from the authenticated server.info response (or the exact owned
// process handle), never from an unverified discovery PID. Every field belongs to the current
// trusted selected instance; nullopt revokes the binding on disconnect, project/trust change or
// instance replacement. It must recheck the captured trust revision before returning a value.
struct EngineNativeDebugTarget {
    std::int64_t pid = 0;
    std::string instanceId, serverGeneration, projectRoot;
    std::uint64_t connectionGeneration = 0, sessionEpoch = 0, trustRevision = 0;
    friend bool operator==(const EngineNativeDebugTarget&, const EngineNativeDebugTarget&) = default;
};

struct EngineNativeDebugBinding {
    EngineNativeDebugTarget target;
    std::uint64_t registrySessionId = 0, debugGeneration = 0;
    QPointer<ui::DebugController> controller;
    friend bool operator==(const EngineNativeDebugBinding&, const EngineNativeDebugBinding&) = default;
};

// Parent-owned QObject, GUI thread only. Captured supplier services must outlive this adapter.
// Observes actual native DAP state/process events; never changes engine push state or tab selection.
// PID alone never grants a binding: the full current authenticated target and native DAP generation
// are checked again for every getter and termination request. A proof pins one unique registry ID,
// controller pointer and generation; its replacement cannot inherit that proof, even with equal PID
// and local generation. A new full authenticated target is required to bind another controller.
// Operations are O(registry sessions + identity text), with at most 128 registry sessions (DBG-10).
class EngineNativeDebugAdapter final : public QObject {
public:
    using TargetSupplier = std::function<std::optional<EngineNativeDebugTarget>()>;
    using Changed = std::function<void()>;
    using Completion = std::function<void(Result<void>)>;
    EngineNativeDebugAdapter(ui::DebugSessionRegistry& registry, TargetSupplier target, Changed changed,
                             QObject* parent = nullptr);
    // Compatibility for isolated single-controller hosts. Product workbenches use the registry.
    EngineNativeDebugAdapter(ui::DebugController& controller, TargetSupplier target, Changed changed,
                             QObject* parent = nullptr);
    ~EngineNativeDebugAdapter() override;
    // Call when pairing/trust/project identity changes; DAP changes call this automatically.
    void refresh();
    [[nodiscard]] bool matched() const;
    [[nodiscard]] bool paused() const;
    [[nodiscard]] bool canTerminate() const;
    [[nodiscard]] std::optional<EngineNativeDebugBinding> binding() const;
    // Uses the matching controller's explicit DAP termination API. Refusal is returned immediately;
    // an admitted request's actual response is delivered via done, scoped to the same identities.
    // No timeout or transport failure is interpreted as a pause or successful termination.
    [[nodiscard]] Result<void> terminate(Completion done = {});

private:
    struct Match {
        EngineNativeDebugBinding binding;
        bool paused = false, canTerminate = false;
        friend bool operator==(const Match&, const Match&) = default;
    };
    void capture();
    [[nodiscard]] std::optional<Match> current() const;
    QPointer<ui::DebugSessionRegistry> m_registry;
    QPointer<ui::DebugController> m_legacyController;
    TargetSupplier m_target;
    Changed m_changed;
    std::optional<EngineNativeDebugTarget> m_admittedTarget;
    std::optional<EngineNativeDebugBinding> m_binding;
    std::optional<Match> m_observed;
};

} // namespace frost::app
