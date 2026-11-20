// SPDX-License-Identifier: Apache-2.0
// Engine ComponentCommands.cpp and Command::Describe, with additive expected-before admission.
#include "frost/engine/serialized_properties.hpp"
namespace frost::engine {
bool supportsExpectedComponentSet(const rpc::Json& description) {
    if (description.stringAt("name") != "component.set")
        return false;
    auto* params = description.find("params");
    if (!params || !params->isArray())
        return false;
    for (const auto& param : params->items())
        if (param.stringAt("name") == "expected" && param.stringAt("type") == "Json")
            return true;
    return false;
}
Result<SerializedComponent> parseSerializedComponent(const AssetIndex& index, std::string_view scene,
                                                     std::string_view component, const rpc::Json& reply,
                                                     std::optional<EngineHandle> expectedEntity) {
    const auto* asset = index.find(scene);
    auto entity = engineHandleAt(reply, "entity");
    auto* fields = reply.find("fields");
    if (!asset || asset->type != "Scene" || reply.stringAt("scene") != asset->path ||
        reply.stringAt("component") != component || !entity || !entity->value ||
        (expectedEntity && *entity != *expectedEntity) || !fields || !fields->isObject())
        return fail(ErrorCode::Corrupt, "Engine serialized component reply changed its target or fields shape");
    return SerializedComponent{asset->path, std::string(component), *entity, *fields};
}
Result<rpc::Json> serializedComponentEdit(const SerializedComponent& before, const rpc::Json& after) {
    if (!before.fields.isObject() || !after.isObject() || !before.entity.value || before.scene.empty() ||
        before.component.empty())
        return fail(ErrorCode::InvalidArgument, "Serialized property edit requires actual Before/After objects");
    for (const auto& name : before.fields.keys())
        if (!after.find(name))
            return fail(ErrorCode::Unsupported, "Deleting serialized fields is not an engine reset operation");
    auto patch = rpc::Json::object();
    for (std::size_t i = 0; i < after.size(); ++i) {
        const auto& name = after.keys()[i];
        const auto* old = before.fields.find(name);
        if (!old || *old != after.items()[i])
            patch.set(name, after.items()[i]);
    }
    if (patch.size() == 0)
        return fail(ErrorCode::InvalidArgument, "No serialized field changed");
    return rpc::Json::object()
        .set("scene", before.scene)
        .set("entity", toJson(before.entity))
        .set("component", before.component)
        .set("expected", before.fields)
        .set("fields", std::move(patch));
}
} // namespace frost::engine
