// SPDX-License-Identifier: Apache-2.0
// Convert immutable source-admission evidence into the Mono adapter's module representation.
#pragma once
#include "frost/debug/mono_dap.hpp"

#include "managed_source_snapshot.hpp"

namespace frost::app {
// Does not connect or establish process identity. The caller must keep admission current and
// recheck trust, selected process generation and dirty documents until the session ends.
[[nodiscard]] Result<std::shared_ptr<const debug::mono::VerifiedModule>>
makeVerifiedMonoModule(std::string assemblyPath, const ManagedModuleSnapshot& snapshot, const MonoSourceMap& map);
} // namespace frost::app
