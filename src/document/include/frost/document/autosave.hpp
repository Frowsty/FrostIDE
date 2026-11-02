// SPDX-License-Identifier: Apache-2.0
//
// AutosaveController: the autosave and hot-exit journaling cadence of a SaveManager (ADR 0028),
// from its policies (save.autosave.*, save.hotExit): Save All when the IDE window loses focus,
// Save All once the user has been idle for the idle delay, and the journal of unsaved buffers
// shortly after the last input. Passive and clock-driven, so tests run it in virtual time.
/*
 * The shell calls noteActivity() for every input event, frameDeactivated() when the main window
 * loses focus, and poll() from a timer set for nextDeadline() (and after noteActivity()). An idle
 * save runs once per idle period: after it, the next one needs new activity first. The journal runs
 * kJournalDelay after the last activity, also once per period. Policies are read from the manager
 * at every call, so settings changes apply at once.
 *
 * Thread safety: the owner thread of the SaveManager. Lifetime: the manager and the clock outlive it.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/document/save_manager.hpp"

#include <chrono>
#include <optional>

namespace frost::document {

class AutosaveController {
public:
    // How long after the last input unsaved buffers are journaled for hot exit.
    static constexpr IClock::Duration kJournalDelay = std::chrono::seconds(1);

    // What one poll() did.
    struct PollResult {
        std::optional<SaveAllResult> autosave; // An idle Save All ran.
        std::optional<JournalReport> journal;  // The hot-exit journal was written.
    };

    // Starts with an idle period beginning now.
    AutosaveController(SaveManager& manager, const IClock& clock) noexcept;

    // An input event: restarts the idle period. O(1).
    void noteActivity() noexcept;
    // The IDE window lost focus: Save All (SaveReason::Autosave) when the policy asks for it, else nullopt.
    std::optional<SaveAllResult> frameDeactivated();
    // When poll() has work next (an idle save or the journal), or nullopt when nothing is pending. O(1).
    [[nodiscard]] std::optional<IClock::SteadyTime> nextDeadline() const noexcept;
    // Runs whatever is due at the clock's now.
    PollResult poll();

private:
    SaveManager* m_manager;
    const IClock* m_clock;
    IClock::SteadyTime m_lastActivity;
    bool m_idleSaveDone = false;
    bool m_journalDone = false;
};

} // namespace frost::document
