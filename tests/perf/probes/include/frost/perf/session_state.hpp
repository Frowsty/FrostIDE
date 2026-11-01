// SPDX-License-Identifier: Apache-2.0
//
// The session-state precheck of PLAN.md section 6.3. A perf, S1 or on-screen UI
// run is meaningful only while the user desktop is being composed: if the
// workstation is locked, the process is not in the active console session (RDP,
// a service in session 0) or the monitor is off, DWM and Desktop Duplication
// return invalid samples. Every such run calls precheck() before sampling and
// again at the end; a failed check marks the run invalid (neither pass nor fail).
//
// ISessionState is the seam: SystemSessionState (Windows) gathers SessionFacts
// from the OS, tests substitute fakes, and evaluateSession() holds all the
// policy, so the rules are unit-tested without a desktop.
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace frost::perf {

// WTS session lock state (WTSINFOEX SessionFlags).
enum class LockState : std::uint8_t { Unknown, Locked, Unlocked };

// Console display power (GUID_CONSOLE_DISPLAY_STATE power setting).
enum class DisplayPower : std::uint8_t { Unknown, Off, On, Dimmed };

// One DXGI output (monitor connection).
struct OutputFacts {
    std::string deviceName;         // UTF-8, e.g. "\\.\DISPLAY1".
    bool attachedToDesktop = false; // DXGI_OUTPUT_DESC::AttachedToDesktop.
    int left = 0;                   // Desktop coordinates in physical pixels.
    int top = 0;
    int width = 0;
    int height = 0;
    double refreshHz = 0; // Current display mode refresh rate; 0 if unknown.
};

// Raw observations; no policy.
struct SessionFacts {
    bool inputDesktopOpened = false; // OpenInputDesktop succeeded (it fails while the secure desktop has input).
    std::string inputDesktopName;    // UTF-8; "Default" is the user's desktop, "Winlogon" the lock screen.
    LockState lock = LockState::Unknown;
    std::uint32_t processSession = 0;       // ProcessIdToSessionId of this process.
    std::uint32_t activeConsoleSession = 0; // WTSGetActiveConsoleSessionId (0xFFFFFFFF: none).
    DisplayPower consoleDisplay = DisplayPower::Unknown;
    std::vector<OutputFacts> outputs;
};

// The precheck result. `valid` is true only when no problem was found.
struct SessionVerdict {
    bool valid = false;
    std::vector<std::string> problems; // One English sentence per failed check.
};

// Applies the three checks of PLAN.md section 6.3 to `facts`:
//   1. the input desktop is the user's "Default" desktop and WTS reports the session unlocked;
//   2. this process runs in the active console session (not session 0, not a remote session);
//   3. at least one DXGI output is attached to the desktop and the console display is on (or dimmed).
// Unknown states are problems: a run must prove its preconditions. Pure, O(outputs).
[[nodiscard]] SessionVerdict evaluateSession(const SessionFacts& facts);

// The source of SessionFacts. Implementations are thread-safe.
class ISessionState {
public:
    // Gathers the current facts. Errors mean the facts could not be read at all.
    [[nodiscard]] virtual Result<SessionFacts> facts() const = 0;

protected:
    ISessionState() = default;
    ISessionState(const ISessionState&) = default;
    ISessionState& operator=(const ISessionState&) = default;
    ~ISessionState() = default;
};

// Reads the facts and evaluates them. A read error is an invalid verdict whose
// problem is the error's description.
[[nodiscard]] SessionVerdict precheck(const ISessionState& state);

// Names for reports ("locked", "on", ...).
[[nodiscard]] const char* toString(LockState state) noexcept;
[[nodiscard]] const char* toString(DisplayPower power) noexcept;

} // namespace frost::perf
