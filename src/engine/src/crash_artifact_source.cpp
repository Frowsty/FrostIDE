// SPDX-License-Identifier: Apache-2.0
// Strict parsing of authenticated crash-source metadata.
#include "frost/engine/crash_artifact_source.hpp"

#include "frost/io/path.hpp"

#include <algorithm>

namespace frost::engine {
Result<CrashArtifactSource> parseCrashArtifactSource(const rpc::Json& value, std::string_view instanceId,
                                                     std::string_view generation, std::string_view projectPath,
                                                     std::int64_t processId) {
    const auto hex = [](std::string_view input) {
        return input.size() == 32 &&
               std::ranges::all_of(input, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    const auto integer = [&](std::string_view key) -> std::optional<std::int64_t> {
        const auto* item = value.find(key);
        return item && item->kind() == rpc::Json::Kind::Integer ? item->asInteger() : std::nullopt;
    };
    const auto pid = integer("processId"), start = integer("processStartUnixMs");
    const auto root = value.stringAt("rootPath"), role = value.stringAt("processRole");
    if (!value.isObject() || integer("protocolVersion") != 1 || !hex(value.stringAt("sourceId")) || !hex(instanceId) ||
        !hex(generation) || value.stringAt("instanceId") != instanceId || value.stringAt("generation") != generation ||
        value.stringAt("projectPath") != projectPath || projectPath.empty() || projectPath.size() > 32768 ||
        projectPath.find_first_of("\r\n") != std::string_view::npos ||
        projectPath.find('\0') != std::string_view::npos || !io::isAbsolutePath(projectPath) || processId <= 0 ||
        !pid || *pid != processId || !start || *start <= 0 || root.size() > 32768 || !io::isAbsolutePath(root) ||
        root.find_first_of("\r\n") != std::string_view::npos || role.empty() || role.size() > 64 ||
        !std::ranges::all_of(role, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        }))
        return fail(ErrorCode::Corrupt, "invalid authenticated crash artifact source");
    // Authenticated source facts authorize local recovery, never an SMB/device lookup.
    auto localRoot = root;
#ifdef _WIN32
    if (root.size() >= 7 && root.starts_with("\\\\?\\") &&
        ((root[4] >= 'A' && root[4] <= 'Z') || (root[4] >= 'a' && root[4] <= 'z')) && root[5] == ':' && root[6] == '\\')
        localRoot.remove_prefix(4);
#endif
    if (localRoot.starts_with("\\\\") || localRoot.starts_with("//") || localRoot.starts_with("\\/") ||
        localRoot.starts_with("/\\"))
        return fail(ErrorCode::Corrupt, "crash sources must use local paths");
    auto normalized = io::normalizePath(localRoot);
    auto project = io::normalizePath(projectPath);
    if (!normalized || !project)
        return fail(ErrorCode::Corrupt, "invalid crash source path");
    if (normalized->starts_with("\\\\") || normalized->starts_with("//"))
        return fail(ErrorCode::Corrupt, "crash sources must use local paths");
    CrashArtifactSource source;
    source.sourceId = value.stringAt("sourceId");
    source.rootPath = *normalized;
    source.processRole = role;
    source.instanceId = instanceId;
    source.generation = generation;
    source.projectPath = projectPath;
    source.processId = *pid;
    source.processStartUnixMs = *start;
    return source;
}
} // namespace frost::engine
