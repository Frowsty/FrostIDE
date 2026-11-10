// SPDX-License-Identifier: Apache-2.0
//
// The planted binary of the malicious-repository test (FND-8, trust_malicious_repo.py): the test
// copies this program into the fixture under the names an attacker would plant (git, cmake, cmd,
// pwsh, bash, clangd, clang-tidy, cl, ninja, MSBuild, lua-language-server, FrostEditor, hooks). When
// anything starts one of the copies, it appends a line to "<its own image path>.ran" (the image path
// from the OS, never argv[0]) holding its arguments, and exits with 0, so the test sees every run
// even of a process that bypassed the spawn log. It reads nothing and starts nothing.
//
// Windows builds use the static CRT (no DLL beside a copy is needed); see the CMakeLists.txt entry.

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>

#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
// UTF-8 of `text` (unpaired surrogates become U+FFFD).
std::string toUtf8(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 1), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    out.resize(out.size() - 1);
    return out;
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // GetModuleFileNameW with a buffer that grows until the path fits (long paths included).
    std::wstring image(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
        if (length == 0)
            return 3;
        if (length < image.size()) {
            image.resize(length);
            break;
        }
        image.resize(image.size() * 2);
    }
    const std::wstring marker = image + L".ran";
    FILE* out = nullptr;
    if (_wfopen_s(&out, marker.c_str(), L"ab") != 0)
        out = nullptr;
    const std::string imageText = toUtf8(image.c_str());
#else
    char buffer[PATH_MAX + 1] = {};
    const ssize_t length = readlink("/proc/self/exe", buffer, PATH_MAX);
    if (length <= 0)
        return 3;
    const std::string imageText(buffer, static_cast<std::size_t>(length));
    FILE* out = std::fopen((imageText + ".ran").c_str(), "ab");
#endif
    if (out == nullptr)
        return 4;
    std::string line = "ran " + imageText;
    for (int i = 1; i < argc; ++i)
        line += std::string(" ") + argv[i];
    line += "\n";
    std::fwrite(line.data(), 1, line.size(), out);
    std::fclose(out);
    return 0;
}
