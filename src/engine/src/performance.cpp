// SPDX-License-Identifier: Apache-2.0
// Strict additive decoder for real stats.get output, preserving unavailable fields as null.
#include "frost/engine/performance.hpp"

#include "frost/io/path.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <sodium.h>
namespace frost::engine {
namespace {
bool bounded(const rpc::Json& value, std::size_t& nodes, std::size_t& bytes, unsigned depth = 0) {
    if (!nodes || depth > 16)
        return false;
    --nodes;
    if (value.isString()) {
        if (value.asString()->size() > bytes)
            return false;
        bytes -= value.asString()->size();
    }
    for (const auto& key : value.keys()) {
        if (key.size() > bytes)
            return false;
        bytes -= key.size();
    }
    for (const auto& item : value.items())
        if (!bounded(item, nodes, bytes, depth + 1))
            return false;
    return true;
}
} // namespace
bool matchesEnginePerformanceSource(const EngineSourcePerformance& scope, std::string_view source) {
    if (scope.sourceSha256.size() != 64 || source.size() > 2U * 1024U * 1024U || sodium_init() < 0)
        return false;
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(source.data()), source.size());
    std::array<char, 65> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), hash.data(), hash.size());
    return scope.sourceSha256 == encoded.data();
}
Result<EnginePerformanceSample> parseEnginePerformance(const EngineIdentity& identity, const rpc::Json& report) {
    if (identity.projectPath.empty() || identity.instanceId.empty() || identity.generation.empty() ||
        !identity.connectionGeneration)
        return fail(ErrorCode::InvalidArgument, "Performance needs an authenticated session identity");
    std::size_t nodes = 12000, bytes = 2U * 1024U * 1024U;
    if (!report.isObject() || !bounded(report, nodes, bytes) || !report.find("frame") ||
        !report.find("frame")->isObject())
        return fail(ErrorCode::Corrupt, "stats.get did not return a bounded measured frame report");
    const auto& frame = *report.find("frame");
    const auto* fps = frame.find("fps");
    if (!frame.unsignedAt("frames") || !fps || !fps->asDouble() || !std::isfinite(*fps->asDouble()) ||
        *fps->asDouble() < 0)
        return fail(ErrorCode::Corrupt, "The frame report lacks measured frame/FPS values");
    EnginePerformanceSample result{identity, report, {}, false};
    result.identity.schema = {};
    result.identity.availableCommands.clear();
    const auto* profiler = report.find("profiler");
    if (!profiler || profiler->stringAt("sourceProvenance") != "macro-source-location/v1")
        return result;
    const auto* scopes = profiler->find("sourceScopes");
    if (!scopes || !scopes->isArray() || scopes->size() > 1000)
        return fail(ErrorCode::Corrupt, "Invalid measured source scope list");
    result.sourceProvenanceAvailable = true;
    for (const auto& scope : scopes->items()) {
        EngineSourcePerformance entry;
        entry.name = scope.stringAt("name");
        entry.file = scope.stringAt("file");
        entry.function = scope.stringAt("function");
        const auto line = scope.unsignedAt("line");
        const auto calls = scope.unsignedAt("calls");
        if (entry.name.empty() || entry.function.empty() || entry.file.empty() ||
            entry.file.find('\0') != std::string::npos || !line || !*line ||
            *line > std::numeric_limits<std::uint32_t>::max() || !calls || !*calls)
            return fail(ErrorCode::Corrupt, "Measured source scope lacks actual source provenance");
        entry.sourceSha256 = scope.stringAt("sourceSha256");
        if (!entry.sourceSha256.empty() &&
            (entry.sourceSha256.size() != 64 || !std::ranges::all_of(entry.sourceSha256, [](char c) {
                 return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
             })))
            return fail(ErrorCode::Corrupt, "Invalid build-source SHA256 receipt");
        entry.line = static_cast<std::uint32_t>(*line);
        entry.calls = *calls;
        const auto metric = [&](std::string_view key, double& out) {
            const auto* value = scope.find(key);
            const auto number = value ? value->asDouble() : std::nullopt;
            if (!number || !std::isfinite(*number) || *number < 0)
                return false;
            out = *number;
            return true;
        };
        if (!metric("msPerFrame", entry.msPerFrame) || !metric("callsPerFrame", entry.callsPerFrame) ||
            !metric("averageMs", entry.averageMs) || !metric("maxMs", entry.maxMs))
            return fail(ErrorCode::Corrupt, "Invalid measured source scope timing");
        result.sourceScopes.push_back(std::move(entry));
    }
    return result;
}
} // namespace frost::engine
