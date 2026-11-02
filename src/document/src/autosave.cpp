// SPDX-License-Identifier: Apache-2.0
//
// AutosaveController (autosave.hpp, ADR 0028): frame-deactivation and idle autosave and the
// hot-exit journal cadence, driven by an IClock.

#include "frost/document/autosave.hpp"

#include <algorithm>

namespace frost::document {

AutosaveController::AutosaveController(SaveManager& manager, const IClock& clock) noexcept
    : m_manager(&manager), m_clock(&clock), m_lastActivity(clock.steadyNow()) {}

void AutosaveController::noteActivity() noexcept {
    m_lastActivity = m_clock->steadyNow();
    m_idleSaveDone = false;
    m_journalDone = false;
}

std::optional<SaveAllResult> AutosaveController::frameDeactivated() {
    if (!m_manager->policies().autosave.onFrameDeactivation) {
        return std::nullopt;
    }
    return m_manager->saveAll(SaveReason::Autosave);
}

std::optional<IClock::SteadyTime> AutosaveController::nextDeadline() const noexcept {
    const SavePolicies& policies = m_manager->policies();
    std::optional<IClock::SteadyTime> next;
    const auto consider = [&](IClock::SteadyTime at) { next = next ? std::min(*next, at) : at; };
    if (policies.autosave.onIdle && !m_idleSaveDone) {
        consider(m_lastActivity + policies.autosave.idleDelay);
    }
    if (policies.hotExit && !m_journalDone) {
        consider(m_lastActivity + kJournalDelay);
    }
    return next;
}

AutosaveController::PollResult AutosaveController::poll() {
    PollResult result;
    const SavePolicies& policies = m_manager->policies();
    const IClock::SteadyTime now = m_clock->steadyNow();
    if (policies.hotExit && !m_journalDone && now - m_lastActivity >= kJournalDelay) {
        m_journalDone = true;
        result.journal = m_manager->journalDirty();
    }
    if (policies.autosave.onIdle && !m_idleSaveDone && now - m_lastActivity >= policies.autosave.idleDelay) {
        m_idleSaveDone = true;
        if (m_manager->anyDirty()) {
            result.autosave = m_manager->saveAll(SaveReason::Autosave);
        }
    }
    return result;
}

} // namespace frost::document
