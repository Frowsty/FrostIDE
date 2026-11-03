// SPDX-License-Identifier: Apache-2.0
// Capture immutable source bytes bound to a PDB verifier's exact-read hashes.
#pragma once

#include "managed_debug_preparation.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace frost::io {
class IFileSystem;
}
namespace frost::project {
class TrustStore;
}

namespace frost::app {

struct ManagedSourceSnapshot {
    std::string moduleId;
    std::map<std::string, std::string> sources;
};

// Inspect each path component without following links; a missing or unreadable component fails closed.
[[nodiscard]] bool managedPathHasNoLinks(io::IFileSystem& fs, std::string_view path);

// Refuses aliases, untrusted paths, changes after the helper read, and oversized files.
// The returned source bytes are immutable evidence for presentation, not a filesystem lock.
[[nodiscard]] Result<ManagedSourceSnapshot> captureManagedSources(io::IFileSystem& fs, const project::TrustStore& trust,
                                                                  std::string_view root,
                                                                  const PreparedManagedDebug& prepared,
                                                                  const std::function<bool()>& cancelled = {});

} // namespace frost::app
