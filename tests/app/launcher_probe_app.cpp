// SPDX-License-Identifier: Apache-2.0
//
// A stand-in for frost_app.dll, for the launcher tests in tests/app/frost_process_test.cpp
// (LauncherSetsTheErrorModeForItsLoads, RestartPassesStatusAndStandardStreamsThrough and the
// restart's cost measurement). frost.exe's launcher loads it from a copied tree under the name
// frost_app.dll: its DllMain records the process error mode while the launcher's LoadLibraryExW
// runs (the mode in which the loader decides whether to raise a hard-error box), and its
// frost_main prints that mode, the mode at the call, the hardening bits it got and the image
// path the launcher was restarted from (ADR 0012), if any, on stdout, and whether the restart
// marker FROST_RESTARTED_FROM is still in its environment (the launcher must have removed it):
//   "launcher probe: error mode at load 0x8003, at entry 0x2, hardening bits 0x1f, restarted
//    from <path or "nothing">, image <this process's image path>, marker unset"
// With FROST_PROBE_ECHO=1 in its environment it then copies stdin to stdout until EOF, writes
// "launcher probe: stderr\n" to stderr and exits with the status FROST_PROBE_EXIT_CODE gives
// (hexadecimal with 0x, else decimal), so a test can follow each stream and the status through
// a restart. It is built like the launcher (static release CRT, no sanitizer, no manifest), so
// it imports KERNEL32.dll alone and loads beside a copied frost.exe without any other DLL, and
// its load cannot fail in a way that raises a hard error itself.

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

#include <windows.h>

namespace {

// Set once by DllMain on the launcher's main thread; read by frost_main on the same thread.
constinit UINT errorModeAtLoad = 0;

// UTF-8 of `text` (unpaired surrogates become U+FFFD).
std::string toUtf8(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 1), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    }
    out.resize(out.size() - 1);
    return out;
}

void write(DWORD which, const std::string& bytes) {
    DWORD written = 0;
    WriteFile(GetStdHandle(which), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

// Environment variable `name`, or "" when unset.
std::wstring environment(const wchar_t* name) {
    wchar_t value[64] = {};
    const DWORD units = GetEnvironmentVariableW(name, value, 64);
    return units > 0 && units < 64 ? std::wstring(value, units) : std::wstring();
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        errorModeAtLoad = GetErrorMode();
    }
    return TRUE;
}

// frost::app::AppEntryPoint's signature (frost/app/startup_core.hpp).
extern "C" __declspec(dllexport) int frost_main(unsigned hardeningBits, const wchar_t* restartedFrom) {
    wchar_t image[32768] = {};
    GetModuleFileNameW(nullptr, image, 32768);
    char line[160];
    const int length = std::snprintf(line, sizeof line,
                                     "launcher probe: error mode at load 0x%x, at entry 0x%x, hardening bits 0x%x, ",
                                     errorModeAtLoad, GetErrorMode(), hardeningBits);
    if (length > 0) {
        const bool marker = GetEnvironmentVariableW(L"FROST_RESTARTED_FROM", nullptr, 0) != 0;
        write(STD_OUTPUT_HANDLE, std::string(line, static_cast<std::size_t>(length)) + "restarted from " +
                                     (restartedFrom != nullptr ? toUtf8(restartedFrom) : std::string("nothing")) +
                                     ", image " + toUtf8(image) + (marker ? ", marker set\n" : ", marker unset\n"));
    }
    if (environment(L"FROST_PROBE_ECHO") != L"1") {
        return 0;
    }
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof buffer, &read, nullptr) != FALSE && read > 0) {
        write(STD_OUTPUT_HANDLE, std::string(buffer, read));
    }
    write(STD_ERROR_HANDLE, "launcher probe: stderr\n");
    const std::wstring code = environment(L"FROST_PROBE_EXIT_CODE");
    return static_cast<int>(std::wcstoul(code.c_str(), nullptr, code.starts_with(L"0x") ? 16 : 10));
}
