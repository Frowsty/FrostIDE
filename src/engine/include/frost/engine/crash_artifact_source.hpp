// SPDX-License-Identifier: Apache-2.0
// Authenticated live crash-source metadata, correlated with later local artifacts.
#pragma once

#include "frost/core/error.hpp"
#include "frost/rpc/json.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace frost::engine {
struct CrashArtifactSource {
    std::string sourceId, rootPath, processRole, instanceId, generation, projectPath;
    std::int64_t processId = 0, processStartUnixMs = 0;
    friend bool operator==(const CrashArtifactSource&, const CrashArtifactSource&) = default;
};
// Called only after mutual authentication and exact server.info identity validation. This binds
// expected local source facts; it does not authenticate mutable report/dump file contents.
[[nodiscard]] Result<CrashArtifactSource> parseCrashArtifactSource(const rpc::Json& value, std::string_view instanceId,
                                                                   std::string_view generation,
                                                                   std::string_view projectPath,
                                                                   std::int64_t processId);
} // namespace frost::engine
