// SPDX-License-Identifier: Apache-2.0
// CoreCLR DAP launch request construction for an already evaluated managed target.
#pragma once

#include "frost/core/error.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/json.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace frost::project {
class TrustStore;
}

namespace frost::debug {

struct DebugTarget;

// All paths are absolute. The caller owns the trust, project generation, build and source checks.
struct CoreClrTarget {
    std::string dotnet{};
    std::string assembly{};
    std::string workingDirectory{};
    std::vector<std::string> arguments{};
    std::vector<std::pair<std::string, std::string>> environment{};
};

/* Builds a CoreCLR launch request without starting a process. The host and assembly paths and
 * working directory must be absolute; invalid paths return InvalidArgument. Environment names
 * must be nonempty and contain neither '=' nor NUL; all arguments and values must be NUL-free.
 * The result owns its data. Independent calls are thread-safe and cost O(total input size).
 */
[[nodiscard]] Result<rpc::Json> coreClrLaunchArguments(const CoreClrTarget& target);

// Captures an existing local process by PID and absolute image path. The returned worker-safe
// admission retains its Windows process handle, or checks Linux /proc start time and image.
// It must outlive the session; failed reads, exit and identity changes fail closed. O(path size).
// No runtime-kind inference is made: the actual CoreCLR adapter decides whether it can attach.
[[nodiscard]] Result<std::function<Result<void>()>> coreClrProcessAdmission(std::uint32_t pid, std::string executable);

// Attaches netcoredbg to a real local process. The caller supplies captured process admission;
// trust and identity are checked before adapter creation and again by the DAP writer. Stop only
// disconnects and never terminates the attached host. trust must outlive the returned session.
// Optional releaseAuthorization ends request-only trust after final writer admission (success or
// failure). Its capture must also release on destruction if dispatch never happens.
// InvalidArgument rejects bad PID/missing admission; Untrusted rejects the target's roots/cwd.
[[nodiscard]] Result<std::unique_ptr<DebugSession>>
attachCoreClrSession(const project::TrustStore& trust, const DebugTarget& target, std::int64_t pid,
                     AdapterFactory factory, std::function<Result<void>()> processAdmission,
                     std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks,
                     std::function<void()> releaseAuthorization = {});

// Adapter subprocess with stdio pipes and process-tree ownership. `executable` must be absolute;
// the launcher must outlive every session using the returned factory. Thread safety follows launcher.
[[nodiscard]] AdapterFactory coreClrAdapterFactory(io::IProcessLauncher& launcher, std::string executable);

/* Starts a managed debug session after validating target paths and checking every project trust
 * root. An empty root list is invalid; trust must already cover evaluation and build separately.
 * Returns InvalidArgument for malformed input or Untrusted without starting an adapter. The
 * trust must outlive the returned session; trust is rechecked by the DAP writer. The session owns
 * its connection; the factory and callbacks follow DebugSession::start's contract.
 */
[[nodiscard]] Result<std::unique_ptr<DebugSession>>
startCoreClrSession(const project::TrustStore& trust, const CoreClrTarget& target,
                    const std::vector<std::string>& trustRoots, AdapterFactory factory,
                    std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks);

} // namespace frost::debug
