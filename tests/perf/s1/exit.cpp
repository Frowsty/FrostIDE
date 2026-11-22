// SPDX-License-Identifier: Apache-2.0
//
// B20 Windows probe: close a verified project window and time the launched process handle.

#include "frost/perf/win/timing.hpp"
#include "frost/rpc/json.hpp"

#include "child_process.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <windows.h>

#include <shellapi.h>

namespace {

namespace fs = std::filesystem;
using frost::perf::win::qpcNow;
using frost::perf::win::qpcRate;

std::string utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
}

std::wstring localAppData() {
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0)
        return {};
    std::wstring value(size, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size);
    if (copied == 0 || copied >= size)
        return {};
    value.resize(copied);
    return value;
}

bool projectCommitted(const fs::path& index, const fs::path& root) {
    std::ifstream stream(index, std::ios::binary);
    if (!stream)
        return false;
    std::string bytes(std::istreambuf_iterator<char>(stream), {});
    const auto document = frost::rpc::parseJsonStrict(bytes);
    const auto* last = document ? document->find("lastProject") : nullptr;
    const auto* string = last ? last->asString() : nullptr;
    if (!string)
        return false;
    const fs::path path(std::u8string(string->begin(), string->end()));
    const std::wstring observed = path.lexically_normal().make_preferred().wstring();
    const std::wstring expected = root.lexically_normal().make_preferred().wstring();
    return CompareStringOrdinal(observed.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

HWND projectWindow(DWORD pid, const std::wstring& title) {
    struct Search {
        DWORD pid;
        const std::wstring& title;
        HWND match = nullptr;
        bool ambiguous = false;
    } search{pid, title};
    EnumWindows(
        [](HWND window, LPARAM state) -> BOOL {
            auto& searchState = *reinterpret_cast<Search*>(state);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != searchState.pid || !IsWindowVisible(window))
                return TRUE;
            // A visible owned dialog could intercept the normal close path.
            if (GetWindow(window, GW_OWNER) != nullptr) {
                searchState.ambiguous = true;
                return FALSE;
            }
            wchar_t text[256]{};
            GetWindowTextW(window, text, static_cast<int>(std::size(text)));
            if (searchState.title != text || !IsWindowEnabled(window))
                return TRUE;
            if (searchState.match) {
                searchState.ambiguous = true;
                return FALSE;
            }
            searchState.match = window;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.ambiguous ? nullptr : search.match;
}

int fail(const char* reason) {
    std::fprintf(stderr, "s1_exit: %s\n", reason);
    return 3;
}

int run(const fs::path& exe, const fs::path& project) {
    const fs::path profile(localAppData());
    const fs::path index = profile / L"FrostIDE/data/sessions/index.json";
    if (!profile.is_absolute() || !exe.is_absolute() || !project.is_absolute() || fs::exists(index))
        return fail("expected absolute paths and a fresh private session index");
    frost::s1::LaunchOptions options;
    options.executable = exe.wstring();
    options.arguments = L"--project \"" + project.wstring() + L"\"";
    auto process = frost::s1::ChildProcess::launch(options);
    if (!process)
        return fail("could not start FrostIDE");
    const auto rate = qpcRate();
    const auto deadline = qpcNow() + rate.fromMs(60'000.0);
    const auto title = project.filename().wstring() + L" - FrostIDE";
    HWND window = nullptr;
    while (qpcNow() < deadline && !process->wait(0)) {
        if (projectCommitted(index, project)) {
            window = projectWindow(process->pid(), title);
            if (window)
                break;
        }
        Sleep(20);
    }
    if (!window)
        return fail("the launched process did not open the expected project window and session");
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != process->pid() || process->wait(0))
        return fail("the project window or process was replaced");

    // Budget: B20. Measure the normal close request through the original process handle.
    const auto requested = qpcNow();
    if (!PostMessageW(window, WM_CLOSE, 0, 0))
        return fail("could not post the normal window close request");
    const auto status = process->wait(10'000);
    const auto observed = qpcNow();
    if (!status || *status != 0)
        return fail("FrostIDE did not exit naturally after the close request");

    const auto report = frost::rpc::Json::object()
                            .set("tool", "s1_exit")
                            .set("pid", process->pid())
                            .set("workspace", utf8(project))
                            .set("sessionIndex", utf8(index))
                            .set("exit", *status)
                            .set("exitMs", rate.toMs(observed - requested));
    std::puts(report.dump().c_str());
    return 0;
}

} // namespace

int main() {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args || count != 3) {
        LocalFree(args);
        return fail("usage: s1_exit <frost.exe> <R3 llvm root>");
    }
    const fs::path exe(args[1]);
    const fs::path project(args[2]);
    LocalFree(args);
    try {
        return run(exe, project);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
