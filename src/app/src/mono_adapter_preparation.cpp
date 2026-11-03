// SPDX-License-Identifier: Apache-2.0
// Conversion of admitted immutable Mono PE/PDB/source evidence for the debugger.
#include "mono_adapter_preparation.hpp"

#include <set>

namespace frost::app {
Result<std::shared_ptr<const debug::mono::VerifiedModule>>
makeVerifiedMonoModule(std::string assemblyPath, const ManagedModuleSnapshot& snapshot, const MonoSourceMap& map) {
    if (assemblyPath.empty() || snapshot.source.moduleId.empty() || snapshot.assemblyBytes.empty() ||
        snapshot.pdbBytes.empty() || map.sources.empty() || map.points.empty())
        return fail(ErrorCode::InvalidArgument, "incomplete Mono source admission");
    std::set<std::string> unique;
    for (const auto& path : map.sources)
        if (path.empty() || !snapshot.source.sources.contains(path) || !unique.insert(path).second)
            return fail(ErrorCode::Corrupt, "Mono source map does not match admitted sources");
    auto module = std::make_shared<debug::mono::VerifiedModule>();
    module->assemblyPath = std::move(assemblyPath);
    module->moduleId = snapshot.source.moduleId;
    module->image = snapshot.assemblyBytes;
    module->pdb = snapshot.pdbBytes;
    module->sources = map.sources;
    module->points.reserve(map.points.size());
    for (const auto& point : map.points) {
        if (point.sourceIndex >= map.sources.size() || point.line <= 0 || point.ilOffset < 0 ||
            (point.methodToken & 0xff000000U) != 0x06000000U || !(point.methodToken & 0x00ffffffU))
            return fail(ErrorCode::Corrupt, "invalid admitted Mono source point");
        module->points.push_back({point.methodToken, point.sourceIndex, point.line, point.ilOffset});
    }
    return std::shared_ptr<const debug::mono::VerifiedModule>(std::move(module));
}
} // namespace frost::app
