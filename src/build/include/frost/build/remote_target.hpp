// SPDX-License-Identifier: Apache-2.0
// CMK-16/RUN-15 explicit application-owned WSL/Linux-SSH endpoints and reversible path maps.
#pragma once
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"
#include "frost/io/path.hpp"
#include <functional>
#include <string>
#include <vector>
namespace frost::build {
enum class RemoteTransport { Wsl, Ssh };
struct RemotePathMapping {
    std::string localRoot, remoteRoot;
    friend bool operator==(const RemotePathMapping&,const RemotePathMapping&)=default;
};
struct RemoteTarget {
    std::string id, name;
    RemoteTransport transport=RemoteTransport::Wsl;
    std::string transportExecutable; // Explicit absolute local wsl.exe/ssh; never resolved from project/PATH.
    std::string distribution; // WSL only, selected by the user; no default distribution fallback.
    std::string host, user; // SSH Linux only, explicit host/user, system known-host verification required.
    unsigned port=22;
    std::string identityFile; // Optional explicit local absolute key. No password/secret is stored here.
    std::string remotePython="/usr/bin/python3"; // Linux Python >=3.8, isolated mode; no shell-selected interpreter.
    std::vector<RemotePathMapping> mappings;
    bool executionApproved=false; // Separate explicit application-scope endpoint approval, default deny.
    friend bool operator==(const RemoteTarget&,const RemoteTarget&)=default;
};
// Pure bounded validation. Mappings cannot overlap in either namespace: no ambiguous inverse.
[[nodiscard]] Result<void> validateRemoteTarget(const RemoteTarget& target);
[[nodiscard]] Result<std::string> mapRemoteToLocal(const RemoteTarget& target,std::string_view remote);
[[nodiscard]] Result<std::string> mapLocalToRemote(const RemoteTarget& target,std::string_view local);
// Application-owned JSON only, not a project-shared settings importer. Unknown/newer versions refuse.
[[nodiscard]] Result<std::vector<RemoteTarget>> parseRemoteTargets(std::string_view bytes);
[[nodiscard]] std::string remoteTargetsJson(const std::vector<RemoteTarget>& targets);
// Static bootstrap only: remote user argv/environment travel over the control pipe, never a shell.
// SSH uses strict known-host checks, BatchMode and -F none; no ProxyCommand/LocalCommand/config hooks.
[[nodiscard]] Result<io::ProcessSpec> remoteTransportSpec(const RemoteTarget& target,std::string_view guardScript);
// Workload spec is Linux/POSIX argv+cwd; environment applies ONLY to the remote child, never SSH/WSL.
[[nodiscard]] Result<void> validateRemoteWorkload(const io::ProcessSpec& workload);
} // namespace frost::build
