// SPDX-License-Identifier: Apache-2.0
// Bounded owning projection of the pinned Lua grammar; no tree-sitter ABI leaves frost::syntax.
#pragma once
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/text/snapshot.hpp"

#include <chrono>
#include <limits>
#include <stop_token>
#include <string>
#include <vector>
namespace frost::syntax {
inline constexpr std::uint32_t kNoLuaNode = std::numeric_limits<std::uint32_t>::max();
struct LuaAstToken {
    std::string kind; // Exact anonymous grammar token, never a lexical guess.
    std::string field;
    text::ByteRange range;
};
struct LuaAstNode {
    std::string kind;
    std::string field; // Actual grammar field on the parent edge, empty when unnamed.
    text::ByteRange range;
    std::uint32_t parent = kNoLuaNode;
    std::vector<std::uint32_t> children; // Named grammar nodes, in source order.
    std::vector<LuaAstToken> tokens;     // Direct anonymous children, in source order.
};
struct LuaAstProjection {
    text::Snapshot snapshot;
    std::vector<LuaAstNode> nodes; // Root is 0; ids are stable for this immutable projection only.
};
struct LuaAstLimits {
    std::uint64_t maxBytes = 8 * 1024 * 1024;
    std::uint32_t maxNodes = 200000;
    std::uint32_t maxDepth = 256;
    std::uint32_t maxVisits = 1000000;
    IClock::Duration budget = std::chrono::milliseconds(250);
    std::stop_token stop;
};
// Worker-only one-shot parse of a captured immutable image through the SAME pinned grammar and
// cancellable ParseJob as editor syntax. Owns the projection; no registry/tree lifetime escapes.
[[nodiscard]] Result<LuaAstProjection> projectLuaSnapshot(text::Snapshot, const LuaAstLimits& = {});
} // namespace frost::syntax
