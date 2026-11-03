// SPDX-License-Identifier: Apache-2.0
// Capture immutable source bytes bound to a PDB verifier's exact-read hashes.
#pragma once

#include "managed_debug_preparation.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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

// Immutable verifier-bound bytes for a Mono module. The runtime's loaded image and PDB must be
// compared byte for byte with these snapshots before any source binding is accepted.
struct MonoSourcePoint {
    std::uint32_t methodToken = 0;
    std::uint32_t sourceIndex = 0;
    std::int32_t line = 0;
    std::int32_t ilOffset = 0;
};

struct ManagedModuleSnapshot {
    ManagedSourceSnapshot source;
    std::string assemblyBytes;
    std::string pdbBytes;
};

struct MonoSourceMap {
    std::vector<std::string> sources;
    std::vector<MonoSourcePoint> points;
};

// Encode helper stdin from already admitted immutable bytes. The caller must establish physical
// trust and bind the document order to the same PE/PDB inspection and current session generation.
[[nodiscard]] Result<std::string> monoDocumentsInput(std::string_view assembly, std::string_view pdb);
[[nodiscard]] Result<std::string> monoMapInput(const ManagedModuleSnapshot& snapshot,
                                               const std::vector<std::string>& documentOrder);

// Parse the bounded verifier output only if it names exactly the prepared module and captured sources.
[[nodiscard]] Result<MonoSourceMap> parseMonoSourceMap(std::string_view json, const PreparedManagedDebug& prepared,
                                                       const ManagedModuleSnapshot& snapshot);

// Inspect each path component without following links; a missing or unreadable component fails closed.
[[nodiscard]] bool managedPathHasNoLinks(io::IFileSystem& fs, std::string_view path);

// Refuses aliases, untrusted paths, changes after the helper read, and oversized files.
// The returned source bytes are immutable evidence for presentation, not a filesystem lock.
[[nodiscard]] Result<ManagedSourceSnapshot> captureManagedSources(io::IFileSystem& fs, const project::TrustStore& trust,
                                                                  std::string_view root,
                                                                  const PreparedManagedDebug& prepared,
                                                                  const std::function<bool()>& cancelled = {});

// This initial Mono slice is limited by the 1 MiB wire-packet cap; larger modules fail closed.
[[nodiscard]] Result<ManagedModuleSnapshot> captureManagedModule(io::IFileSystem& fs, const project::TrustStore& trust,
                                                                 std::string_view root,
                                                                 const PreparedManagedDebug& prepared,
                                                                 const std::function<bool()>& cancelled = {});

} // namespace frost::app
