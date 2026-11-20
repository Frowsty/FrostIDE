// SPDX-License-Identifier: Apache-2.0
// Trusted native generated-C++ IL2CPP profile; no managed scripting protocol is implied.
#pragma once
#include "frost/app/workbench.hpp"
#include "frost/debug/lldb_dap.hpp"
namespace frost::app {
struct UnityNativeProfile {
    debug::DebugTarget target;
    rpc::Json arguments;
    std::string generatedRoot;
};
[[nodiscard]] bool isUnityGeneratedSource(io::IFileSystem& fs, std::string_view root, std::string_view path);
// Pure preparation over supplied filesystem/trust state. Does not launch or verify native symbol
// identity: actual LLDB module/symbol results remain authoritative. Rejects aliases/outside roots.
[[nodiscard]] Result<UnityNativeProfile> prepareUnityNativeProfile(io::IFileSystem& fs,
    const project::TrustStore& trust, std::string_view root, const UnityIl2cppNativeSelection& selection);
} // namespace frost::app
