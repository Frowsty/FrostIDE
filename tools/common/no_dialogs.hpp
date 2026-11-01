// SPDX-License-Identifier: Apache-2.0
//
// The crash-dialog guard of FrostIDE's build-time tools (frost-themec, frost-icon-baker): a
// tool that runs inside a build must never show UI, so on Windows it turns off the critical-
// error and fault dialogs, the abort() message box and the debug CRT's report windows (which
// then go to stderr). Call disableToolDialogs() first thing in main(). Header-only; no-op
// elsewhere.
#pragma once

#ifdef _WIN32
#include <windows.h>

#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace frost::tools {

inline void disableToolDialogs() noexcept {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // In release CRTs these are no-op macros.
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        static_cast<void>(type);
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
}

} // namespace frost::tools
