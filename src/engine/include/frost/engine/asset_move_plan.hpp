// SPDX-License-Identifier: Apache-2.0
// Typed read-only engine asset move proposals; bytes remain owned by native REF1.
#pragma once
#include "frost/engine/handle.hpp"

#include <string>
#include <vector>
namespace frost::engine {
struct AssetMoveEdit {
    std::string path, before, after;
};
struct AssetMove {
    std::string from, to;
};
struct AssetMoveGuard {
    std::string path, hash;
    std::uint64_t size{};
};
struct AssetMovePlan {
    std::string lease, receipt, oldPath, path;
    EngineHandle handle;
    std::vector<AssetMoveEdit> files;
    std::vector<AssetMove> moves;
    std::vector<AssetMoveGuard> guards;
};
// Pure bounded parser. Paths are portable project-relative identities, never filesystem authority.
// Rejects ambiguous aliases, duplicate/overlapping participants and incomplete move guards.
[[nodiscard]] Result<AssetMovePlan> parseAssetMovePlan(const rpc::Json& reply, std::string_view source,
                                                       std::string_view target, EngineHandle handle);
// Producer's FNV1a64 raw-byte fingerprint (decimal). Exact bytes are checked again by its lease.
[[nodiscard]] std::string assetMoveHash(std::string_view bytes);
} // namespace frost::engine
