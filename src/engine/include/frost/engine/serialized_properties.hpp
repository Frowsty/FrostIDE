// SPDX-License-Identifier: Apache-2.0
// FEC-6 live reflected serialized component values, supplied/validated by the actual engine.
#pragma once
#include "frost/engine/asset_index.hpp"
namespace frost::engine {
struct SerializedComponent {
    std::string scene, component;
    EngineHandle entity;
    rpc::Json fields;
};
// Pure/thread-safe checks of real component.get/set and commands.describe replies. No defaults,
// guessed fields, lossy UUID conversion or frontend substitute for engine validation.
[[nodiscard]] bool supportsExpectedComponentSet(const rpc::Json& description);
[[nodiscard]] Result<SerializedComponent> parseSerializedComponent(const AssetIndex& index, std::string_view scene,
                                                                   std::string_view component, const rpc::Json& reply,
                                                                   std::optional<EngineHandle> expectedEntity = {});
// Keeps unchanged/read-only fields out of the patch, but includes the COMPLETE actual Before
// for atomic main-thread compare-before admission. Deleting fields is unsupported, not a reset.
[[nodiscard]] Result<rpc::Json> serializedComponentEdit(const SerializedComponent& before, const rpc::Json& after);
} // namespace frost::engine
