// SPDX-License-Identifier: Apache-2.0
// Immutable actual-index queries injected without a lang-to-engine dependency or filesystem inference.
#pragma once
#include "frost/lang/json.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace frost::lang {
struct JsonIndexedAsset {
    std::uint64_t handle = 0;
    std::string path, type; // Actual engine-index Assets/... endpoint and engine type.
};
struct JsonAssetReferenceProvider {
    // Worker-safe closures own the immutable actual index. No process, filesystem scan,
    // fallback-file guess, or floating-point handle conversion is permitted here.
    std::function<std::optional<JsonIndexedAsset>(const Json&)> resolve;
    std::function<std::vector<JsonIndexedAsset>(const std::vector<std::string>& types, std::string_view prefix,
                                                std::size_t limit)>
        complete;
};
} // namespace frost::lang
