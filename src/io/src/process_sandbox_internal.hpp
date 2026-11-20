// SPDX-License-Identifier: Apache-2.0
// Shares internal sandbox launch declarations with platform backends.
#pragma once
#include "frost/io/process_sandbox.hpp"
#ifdef _WIN32
#include <windows.h>
namespace frost::io::detail {
struct WindowsSandboxAttributes {
    PSID appSid = nullptr;
    SECURITY_CAPABILITIES capabilities{};
    SID_AND_ATTRIBUTES network{};
    alignas(DWORD) unsigned char networkSid[SECURITY_MAX_SID_SIZE]{};
    DWORD allPackagesPolicy = 1; // PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT
    std::vector<HANDLE> pinnedPaths;
    ~WindowsSandboxAttributes() { for (auto handle : pinnedPaths) CloseHandle(handle); if (appSid) FreeSid(appSid); }
};
[[nodiscard]] Result<std::unique_ptr<WindowsSandboxAttributes>> prepareWindowsSandboxAttributes(const ProcessSpec& spec);
}
#else
namespace frost::io::detail {
[[nodiscard]] Result<ProcessSpec> prepareLinuxSandboxLaunch(const ProcessSpec& spec);
// Owned helper only: applies irreversible native restrictions to the current helper process.
[[nodiscard]] Result<void> enforceLinuxSandbox(const ProcessSandboxProfile& profile);
}
#endif
