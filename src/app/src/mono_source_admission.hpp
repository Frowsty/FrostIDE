// SPDX-License-Identifier: Apache-2.0
// Worker-only two-phase source admission for one already-selected Mono module.
#pragma once
#include "managed_source_snapshot.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string_view>
namespace frost::io {
class IProcessLauncher;
}
namespace frost::app {
struct MonoAdmittedModule {
    std::shared_ptr<const ManagedModuleSnapshot> snapshot;
    std::shared_ptr<const MonoSourceMap> map;
};
// The caller owns generation/trust/dirty-document policy, requests stop immediately on revocation,
// and rechecks that policy before publication and every later source binding. Both helper paths
// must resolve to verified installed executables. This does not connect to a runtime or Editor.
[[nodiscard]] Result<MonoAdmittedModule> admitMonoModule(io::IProcessLauncher& launcher, std::string_view heldHelper,
                                                         std::string_view dotnet, std::string_view guard,
                                                         std::string_view root, std::string_view assemblyPath,
                                                         std::string_view pdbPath, std::stop_token stop,
                                                         const std::function<bool()>& current,
                                                         std::chrono::milliseconds timeout = std::chrono::seconds(60));
} // namespace frost::app
