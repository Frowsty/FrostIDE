// SPDX-License-Identifier: Apache-2.0
//
// Death tests deliberately crash or abort. On Windows the debug CRT and Windows
// Error Reporting would show modal dialogs that hang an unattended test run;
// call disableCrashDialogs() first thing inside every death-test statement.
#pragma once

#ifdef _WIN32
#include <windows.h>

#include <stdlib.h>
#endif

namespace frost::test {

inline void disableCrashDialogs() {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
}

} // namespace frost::test
