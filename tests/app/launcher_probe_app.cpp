// SPDX-License-Identifier: Apache-2.0
//
// A stand-in for frost_app.dll, for FrostProcess.LauncherSetsTheErrorModeForItsLoads in
// tests/app/frost_process_test.cpp. frost.exe's launcher loads it from a copied tree under the
// name frost_app.dll: its DllMain records the process error mode while the launcher's
// LoadLibraryExW runs (the mode in which the loader decides whether to raise a hard-error box),
// and its frost_main prints that mode, the mode at the call and the hardening bits it got:
//   "launcher probe: error mode at load 0x8003, at entry 0x2, hardening bits 0x1f"
// It is built like the launcher (static release CRT, no sanitizer, no manifest), so it imports
// KERNEL32.dll alone and loads beside a copied frost.exe without any other DLL, and its load
// cannot fail in a way that raises a hard error itself.

#include <cstdio>

#include <windows.h>

namespace {

// Set once by DllMain on the launcher's main thread; read by frost_main on the same thread.
constinit UINT errorModeAtLoad = 0;

} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        errorModeAtLoad = GetErrorMode();
    }
    return TRUE;
}

// frost::app::AppEntryPoint's signature (frost/app/startup_core.hpp).
extern "C" __declspec(dllexport) int frost_main(unsigned hardeningBits) {
    char line[128];
    const int length = std::snprintf(line, sizeof line,
                                     "launcher probe: error mode at load 0x%x, at entry 0x%x, hardening bits 0x%x\n",
                                     errorModeAtLoad, GetErrorMode(), hardeningBits);
    DWORD written = 0;
    if (length > 0) {
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, static_cast<DWORD>(length), &written, nullptr);
    }
    return 0;
}
