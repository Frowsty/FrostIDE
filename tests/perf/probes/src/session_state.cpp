// SPDX-License-Identifier: Apache-2.0
//
// The portable half of the session-state precheck: the policy in
// evaluateSession() and precheck() (see frost/perf/session_state.hpp).

#include "frost/perf/session_state.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace frost::perf {

namespace {

constexpr std::uint32_t kNoConsoleSession = 0xFFFFFFFFU;

bool equalsIgnoringCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

} // namespace

SessionVerdict evaluateSession(const SessionFacts& facts) {
    SessionVerdict verdict;
    auto& problems = verdict.problems;

    // 1. Unlocked: the user's desktop has input and WTS agrees.
    if (!facts.inputDesktopOpened) {
        problems.emplace_back("the input desktop cannot be opened (the workstation is locked or a secure "
                              "desktop such as UAC has input)");
    } else if (!equalsIgnoringCase(facts.inputDesktopName, "Default")) {
        problems.push_back(
            std::format("the input desktop is '{}', not the user's Default desktop", facts.inputDesktopName));
    }
    if (facts.lock == LockState::Locked) {
        problems.emplace_back("WTS reports the session locked");
    } else if (facts.lock == LockState::Unknown) {
        problems.emplace_back("WTS does not report the session lock state");
    }

    // 2. The active console session (session 0 has no DWM; a remote session has no local display).
    if (facts.processSession == 0) {
        problems.emplace_back("the process runs in session 0 (a service), which has no desktop composition");
    }
    if (facts.activeConsoleSession == kNoConsoleSession) {
        problems.emplace_back("there is no active console session");
    } else if (facts.processSession != facts.activeConsoleSession) {
        problems.push_back(std::format("the process runs in session {}, not the active console session {}",
                                       facts.processSession, facts.activeConsoleSession));
    }

    // 3. A composed, powered display.
    const bool attached =
        std::ranges::any_of(facts.outputs, [](const OutputFacts& output) { return output.attachedToDesktop; });
    if (!attached) {
        problems.emplace_back("DXGI reports no output attached to the desktop");
    }
    if (facts.consoleDisplay == DisplayPower::Off) {
        problems.emplace_back("the console display is off");
    } else if (facts.consoleDisplay == DisplayPower::Unknown) {
        problems.emplace_back("the console display power state is unknown");
    }

    verdict.valid = problems.empty();
    return verdict;
}

SessionVerdict precheck(const ISessionState& state) {
    auto facts = state.facts();
    if (!facts) {
        SessionVerdict verdict;
        verdict.problems.push_back("session facts unavailable: " + facts.error().describe());
        return verdict;
    }
    return evaluateSession(*facts);
}

const char* toString(LockState state) noexcept {
    switch (state) {
    case LockState::Locked: return "locked";
    case LockState::Unlocked: return "unlocked";
    case LockState::Unknown: break;
    }
    return "unknown";
}

const char* toString(DisplayPower power) noexcept {
    switch (power) {
    case DisplayPower::Off: return "off";
    case DisplayPower::On: return "on";
    case DisplayPower::Dimmed: return "dimmed";
    case DisplayPower::Unknown: break;
    }
    return "unknown";
}

} // namespace frost::perf
