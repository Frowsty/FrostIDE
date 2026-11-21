// SPDX-License-Identifier: Apache-2.0
// Independent native oracle: uses no FrostIDE policy implementation or descriptor parser.
#include <cstdio>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>

#include <cerrno>

#include <unistd.h>
#endif
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    std::ifstream allowed(argv[1]);
    std::string text;
    std::getline(allowed, text);
    if (text != "public-fixture")
        return 10;
    allowed.close(); // The write refusal must come from policy, not an existing read handle.
    std::ifstream secret(argv[2]);
    if (secret.is_open())
        return 11;
    std::ofstream write(argv[1], std::ios::trunc);
    if (write.is_open())
        return 12;
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return 13;
    DWORD container = 0, size = sizeof(container);
    if (!GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &size) || !container) {
        CloseHandle(token);
        return 14;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenCapabilities, nullptr, 0, &needed);
    std::string storage(needed, '\0');
    if (!GetTokenInformation(token, TokenCapabilities, storage.data(), needed, &needed)) {
        CloseHandle(token);
        return 15;
    }
    const auto* capabilities = reinterpret_cast<const TOKEN_GROUPS*>(storage.data());
    CloseHandle(token);
    // This profile is capability-free: no Internet/private-network/COM/registry capability.
    if (capabilities->GroupCount != 0)
        return 16;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0) == 0)
        return 19;
#else
    const int network = socket(AF_INET, SOCK_STREAM, 0);
    if (network >= 0) {
        close(network);
        return 17;
    }
    if (errno != EPERM && errno != EACCES)
        return 18;
#endif
    std::puts("read=allowed secret=denied write=denied network=denied");
    return 0;
}
