// SPDX-License-Identifier: Apache-2.0
// FER-14/FEC-11 measured authenticated stats.get data and explicit instrumentation source receipts.
#pragma once
#include "frost/engine/session.hpp"
namespace frost::engine {
struct EngineSourcePerformance {
    std::string name, file, function, sourceSha256; // Build receipt; empty disables inline attribution.
    std::uint32_t line = 0;
    std::uint64_t calls = 0;
    double msPerFrame = 0, callsPerFrame = 0, averageMs = 0, maxMs = 0;
};
struct EnginePerformanceSample {
    EngineIdentity identity;
    rpc::Json measured; // Original real report, including null/unavailable renderer values.
    std::vector<EngineSourcePerformance> sourceScopes; // No name-to-function guesses.
    bool sourceProvenanceAvailable = false;
};
// Pure O(source bytes), bounded to 2 MiB. Missing receipts, changed bytes or unavailable
// hashing refuse attribution. Caller must separately retain current trust/session identity.
[[nodiscard]] bool matchesEnginePerformanceSource(const EngineSourcePerformance&, std::string_view source);
// Pure O(report bytes), bounded to 2 MiB/1000 source scopes; malformed metrics fail closed.
// The producer supplies its exact authenticated identity; app publication must still revalidate it.
[[nodiscard]] Result<EnginePerformanceSample> parseEnginePerformance(const EngineIdentity& identity,
                                                                     const rpc::Json& report);
} // namespace frost::engine
