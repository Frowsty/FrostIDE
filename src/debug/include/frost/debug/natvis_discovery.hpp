// SPDX-License-Identifier: Apache-2.0
// Bounded discovery of user, project and dependency visualizers; discovery never evaluates XML.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"

#include <string>
#include <vector>

namespace frost::debug {
struct NatvisDiscovery {
    // Explicit evaluated project items, user files and installed packs, in precedence order.
    std::vector<std::string> files;
    // Explicitly admitted project/user/dependency directories. Links are never traversed.
    std::vector<std::string> roots;
    // Known project/dependency source directories, inspected one level only (e.g. Frost.natvis).
    std::vector<std::string> directories;
    std::size_t maxDirectories = 4096;
    std::size_t maxEntries = 100000;
    std::size_t maxFiles = 64;
};
struct NatvisInventory {
    std::vector<std::string> files;
    std::vector<std::string> diagnostics;
};
// Synchronous worker operation, no shared state. Paths must be absolute; no implicit home/SDK scan.
// Physical identities deduplicate aliases. Missing optional roots/files produce diagnostics;
// malformed paths, inaccessible inputs and exceeded bounds fail rather than silently truncate.
[[nodiscard]] Result<NatvisInventory> discoverNatvis(io::IFileSystem&, const NatvisDiscovery&);
} // namespace frost::debug
