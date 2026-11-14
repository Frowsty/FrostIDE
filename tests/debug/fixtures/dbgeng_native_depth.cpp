// SPDX-License-Identifier: Apache-2.0
// Real native debugger depth fixture; output reports values read by the program after debugger writes.
#include <cstdio>
#include <cstring>
#include <cwchar>

#include <windows.h>
volatile long nativeValue = 7;
struct Pair {
    long member;
    long other;
};
extern "C" __declspec(noinline) void FrostNativeDepthCheckpoint(int iteration, Pair* first, Pair* second) {
    const auto value = nativeValue; // DBGENG_DEPTH_WRITE_MARKER
    wchar_t text[256];
    std::swprintf(text, 256, L"native-depth iteration=%d value=%ld first=%ld second=%ld\n", iteration, value,
                  first->member, second->member);
    OutputDebugStringW(text);
}
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wcscmp(argv[1], L"--attach") == 0) {
        std::printf("ready %lu\n", GetCurrentProcessId());
        std::fflush(stdout);
        if (argc > 2) {
            const auto ready =
                CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (ready == INVALID_HANDLE_VALUE)
                return 71;
            const char bytes[] = "ready";
            DWORD written = 0;
            const auto ok = WriteFile(ready, bytes, 5, &written, nullptr);
            CloseHandle(ready);
            if (!ok || written != 5)
                return 72;
        }
        while (nativeValue == 7)
            Sleep(10);
    }
    Pair first{11, 12}, second{21, 22};
    for (int iteration = 0; iteration < 4; ++iteration) {
        FrostNativeDepthCheckpoint(iteration, &first, &second);
        InterlockedIncrement(&nativeValue);
    }
    return 0;
}
