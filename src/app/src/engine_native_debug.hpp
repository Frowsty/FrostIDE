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
}

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

// Parent-owned QObject, GUI thread only. Controller and captured supplier services must outlive
// this adapter. Observes actual native DAP state/process events; never changes engine push state.
// PID alone never grants a binding: the full current authenticated target and native DAP generation
// are checked again for every getter and termination request. All operations O(identity text size).
class EngineNativeDebugAdapter final : public QObject {
public:
    using TargetSupplier = std::function<std::optional<EngineNativeDebugTarget>()>;
    using Changed = std::function<void()>;
    using Completion = std::function<void(Result<void>)>;
    EngineNativeDebugAdapter(ui::DebugController& controller, TargetSupplier target, Changed changed,
                             QObject* parent = nullptr);
    ~EngineNativeDebugAdapter() override;
    // Call when pairing/trust/project identity changes; DAP changes call this automatically.
    void refresh();
    [[nodiscard]] bool matched() const;
    [[nodiscard]] bool paused() const;
    [[nodiscard]] bool canTerminate() const;
    // Uses the matching controller's explicit DAP termination API. Refusal is returned immediately;
    // an admitted request's actual response is delivered via done, scoped to the same identities.
    // No timeout or transport failure is interpreted as a pause or successful termination.
    [[nodiscard]] Result<void> terminate(Completion done = {});

private:
    struct Match {
        EngineNativeDebugTarget target;
        std::uint64_t debugGeneration = 0;
        bool paused = false, canTerminate = false;
        friend bool operator==(const Match&, const Match&) = default;
    };
    [[nodiscard]] std::optional<Match> current() const;
    QPointer<ui::DebugController> m_controller;
    TargetSupplier m_target;
    Changed m_changed;
    std::optional<Match> m_observed;
};

} // namespace frost::app
