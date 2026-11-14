// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/debug/lldb_dap.hpp"

namespace frost::debug {
// GDB is user-installed (GPL), never downloaded or searched on PATH. Requires GDB >=16,
// built with Python/DAP support. The initialize handshake reports missing DAP support.
[[nodiscard]] Result<void> validateGdbDapVersion(std::string_view banner);
[[nodiscard]] Result<std::string> findGdbDap(io::IFileSystem& fs, std::string configured);
[[nodiscard]] io::ProcessSpec gdbDapProcessSpec(std::string executable);
[[nodiscard]] AdapterFactory gdbDapAdapterFactory(io::IProcessLauncher& launcher, std::string executable,
                                                DapProcessOutput output = {});
// inheritedEnvironment is a snapshot supplied by the caller; explicit target values win.
// GDB's env replaces the entire inferior environment, unlike LLDB's env array.
[[nodiscard]] Result<rpc::Json> gdbDapLaunchArguments(
    const DebugTarget& target, const std::map<std::string, std::string>& inheritedEnvironment);
[[nodiscard]] Result<std::unique_ptr<DebugSession>> startGdbDebugSession(
    const project::TrustStore& trust, const DebugTarget& target, AdapterFactory factory,
    const std::map<std::string, std::string>& inheritedEnvironment,
    std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks,
    std::optional<NativeDebugPrivacy> privacy = {});
[[nodiscard]] Result<std::unique_ptr<DebugSession>> attachGdbDebugSession(
    const project::TrustStore& trust, const DebugTarget& target, std::int64_t pid, AdapterFactory factory,
    std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks);
} // namespace frost::debug

