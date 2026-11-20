// SPDX-License-Identifier: Apache-2.0
// Exact engine-reported handle/path endpoints; lookup never substitutes filesystem discoveries.
#include "engine_json_reference_provider.hpp"

#include "frost/lang/uri.hpp"

#include "engine_integration_internal.hpp"
#include "language_bridge.hpp"

#include <algorithm>
namespace frost::app {
lang::JsonAssetReferenceProvider makeEngineJsonAssetProvider(std::shared_ptr<const engine::AssetIndex> index) {
    if (!index)
        return {};
    const auto target = [](const engine::ProjectAsset& asset) {
        return lang::JsonIndexedAsset{asset.handle.value, asset.path, asset.type};
    };
    lang::JsonAssetReferenceProvider provider;
    provider.resolve = [index, target](const lang::Json& value) -> std::optional<lang::JsonIndexedAsset> {
        const auto byHandle = [&](const lang::Json& handle) -> const engine::ProjectAsset* {
            const auto parsed = engine::engineHandleFrom(handle);
            return parsed ? index->find(*parsed) : nullptr;
        };
        const auto byPath = [&](std::string_view path) -> const engine::ProjectAsset* {
            if (path.empty())
                return nullptr;
            if (const auto* actual = index->find(path))
                return actual;
            // Engine asset references accept the Assets/-relative spelling too. This is
            // only a second key lookup in the actual listing, never a constructed endpoint.
            if (!path.starts_with("Assets/") && path.find(':') == path.npos && !path.starts_with('/') &&
                !path.starts_with('\\'))
                return index->find("Assets/" + std::string(path));
            return nullptr;
        };
        const engine::ProjectAsset* actual = nullptr;
        if (value.isObject()) {
            if (const auto* handle = value.find("Handle"))
                actual = byHandle(*handle);
            if (!actual)
                actual = byPath(value.stringAt("Path"));
        } else {
            actual = byHandle(value);
            if (!actual && value.asString())
                actual = byPath(*value.asString());
        }
        return actual ? std::optional(target(*actual)) : std::nullopt;
    };
    provider.complete = [index, target](const std::vector<std::string>& types, std::string_view prefix,
                                        std::size_t limit) {
        std::vector<lang::JsonIndexedAsset> found;
        const auto key = lang::documentKey(prefix);
        for (const auto& asset : index->content().assets) {
            if (!types.empty() && std::ranges::find(types, asset.type) == types.end())
                continue;
            if (!key.empty() && lang::documentKey(asset.path).find(key) == std::string::npos)
                continue;
            if (found.size() == limit)
                break;
            found.push_back(target(asset));
        }
        return found;
    };
    return provider;
}
void EngineIntegration::Impl::publishDocumentAssets() {
    if (!workbench.m_bridge)
        return;
    workbench.m_bridge->setJsonAssetReferences(assetIndex ? root : std::string{},
                                               makeEngineJsonAssetProvider(assetIndex));
}
} // namespace frost::app
