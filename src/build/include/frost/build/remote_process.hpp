// SPDX-License-Identifier: Apache-2.0
// R19 owned remote-control process adapter. Workload outputs and cleanup must be acknowledged remotely.
#pragma once
#include "frost/build/remote_target.hpp"
#include <chrono>
namespace frost::build {
struct RemoteExecutionOptions {
    // Worker-safe current root/endpoint/trust/stop admission; called before launch and while alive.
    // Services captured by the callback outlive the process. Missing callback refuses execution.
    std::function<Result<void>()> admit;
    std::chrono::milliseconds startupTimeout{15000};
    std::chrono::milliseconds stopTimeout{5000};
};
// A per-job, immutable endpoint launcher. Lifetime: base launcher outlives all children.
// Remote Linux Python >=3.9/proc/pidfd/subreaper support is checked by the actual guardian. No endpoint
// discovery, password dialog, implicit approval, local project hooks or remote shell argv expansion.
class RemoteProcessLauncher final : public io::IProcessLauncher {
public:
    RemoteProcessLauncher(io::IProcessLauncher& local,RemoteTarget target,RemoteExecutionOptions options)
        :m_local(local),m_target(std::move(target)),m_options(std::move(options)) {}
    [[nodiscard]] Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& workload) override;
    [[nodiscard]] Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec&,const io::AsyncContext&) override;
private:
    io::IProcessLauncher& m_local;
    RemoteTarget m_target;
    RemoteExecutionOptions m_options;
};
} // namespace frost::build
