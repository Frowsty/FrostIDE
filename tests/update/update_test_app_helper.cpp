// SPDX-License-Identifier: Apache-2.0
// Standalone helper executable for SafeUpdater real-process health-check and rollback tests.
// Reads a trailing `FROST_HELPER_META:version=<v>;selftest_exit=<n>;run_exit=<m>` marker appended
// to its own binary image so each test payload has distinct bytes, SHA-256 digest, and behavior.

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

// Test payload metadata contains decimal exit codes. Refuse malformed metadata instead of
// silently reporting a successful health check through atoi's zero fallback.
int exitCode(std::string_view text) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        std::fputs("updater helper: invalid exit-code metadata\n", stderr);
        std::exit(65);
    }
    return value;
}

std::string readSelfTail(const char* argv0) {
    std::ifstream in;
#ifdef _WIN32
    wchar_t modulePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        in.open(modulePath, std::ios::binary | std::ios::ate);
    }
#else
    char linkBuf[4096]{};
    const ssize_t linkLen = ::readlink("/proc/self/exe", linkBuf, sizeof(linkBuf) - 1);
    if (linkLen > 0) {
        linkBuf[linkLen] = '\0';
        in.open(linkBuf, std::ios::binary | std::ios::ate);
    }
#endif
    if (!in.is_open() && argv0 != nullptr) {
        in.open(argv0, std::ios::binary | std::ios::ate);
    }
    if (!in.is_open()) {
        return {};
    }
    const auto endPos = in.tellg();
    if (endPos <= 0) {
        return {};
    }
    const std::streamoff tailSize = std::min<std::streamoff>(endPos, 512);
    in.seekg(endPos - tailSize, std::ios::beg);
    std::string buf(static_cast<std::size_t>(tailSize), '\0');
    in.read(buf.data(), tailSize);
    return buf;
}

std::string extractField(std::string_view meta, std::string_view key, std::string_view defaultVal) {
    const std::string pattern = std::string(key) + "=";
    const auto pos = meta.find(pattern);
    if (pos == std::string_view::npos) {
        return std::string(defaultVal);
    }
    const auto valStart = pos + pattern.size();
    const auto valEnd = meta.find_first_of(";\r\n", valStart);
    return std::string(
        meta.substr(valStart, valEnd == std::string_view::npos ? meta.size() - valStart : valEnd - valStart));
}

} // namespace

int main(int argc, char** argv) {
    const std::string tail = readSelfTail(argc > 0 ? argv[0] : nullptr);
    constexpr std::string_view kMarker = "FROST_HELPER_META:";
    std::string_view meta;
    if (const auto pos = tail.rfind(kMarker); pos != std::string::npos) {
        meta = std::string_view(tail).substr(pos + kMarker.size());
    }

    const std::string version = extractField(meta, "version", "0.0.0");
    const int selftestExit = exitCode(extractField(meta, "selftest_exit", "0"));
    const int runExit = exitCode(extractField(meta, "run_exit", "0"));

    bool isSelftest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--selftest") {
            isSelftest = true;
        }
    }

    if (isSelftest) {
        if (selftestExit == 0) {
            std::fprintf(stdout, "FrostIDE %s selftest ok\n", version.c_str());
            std::fflush(stdout);
        } else {
            std::fprintf(stderr, "FrostIDE %s selftest failed (code %d)\n", version.c_str(), selftestExit);
            std::fflush(stderr);
        }
        return selftestExit;
    }

    std::fprintf(stdout, "FrostIDE %s running\n", version.c_str());
    std::fflush(stdout);
    return runExit;
}
