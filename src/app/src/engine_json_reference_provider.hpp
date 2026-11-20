// SPDX-License-Identifier: Apache-2.0
// Worker-safe JSON reference queries backed exclusively by the retained actual engine index.
#pragma once
#include "frost/engine/asset_index.hpp"
#include "frost/lang/json_engine_references.hpp"
#include <memory>
namespace frost::app {
[[nodiscard]] lang::JsonAssetReferenceProvider makeEngineJsonAssetProvider(std::shared_ptr<const engine::AssetIndex>);
}
