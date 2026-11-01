// SPDX-License-Identifier: Apache-2.0
//
// SystemSessionState: the Windows ISessionState that reads the real session
// facts (frost/perf/session_state.hpp) from the OS:
//   input desktop   OpenInputDesktop + GetUserObjectInformationW(UOI_NAME);
//   lock state      WTSQuerySessionInformationW(WTSSessionInfoEx) SessionFlags;
//   console session ProcessIdToSessionId versus WTSGetActiveConsoleSessionId;
//   display power   the GUID_CONSOLE_DISPLAY_STATE power setting, whose current
//                   value Windows delivers right after registration;
//   outputs         DXGI outputs (AttachedToDesktop) and the current mode's refresh rate.
#pragma once

#include "frost/perf/session_state.hpp"

namespace frost::perf::win {

class SystemSessionState final : public ISessionState {
public:
    SystemSessionState() = default;

    // Thread-safe. Takes up to about 0.5 s when the power-setting callback is slow.
    // Errors: IoError when DXGI cannot be initialised; every other check reports
    // Unknown/false in the facts instead of failing.
    [[nodiscard]] Result<SessionFacts> facts() const override;
};

} // namespace frost::perf::win
