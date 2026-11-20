// SPDX-License-Identifier: Apache-2.0
// Actual serialized-component protocol shapes; no engine execution is claimed by these specimens.
#include "frost/engine/serialized_properties.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>
namespace frost::engine {
namespace {
rpc::Json fields() {
    return rpc::Json::object()
        .set("Translation", rpc::Json::array({1, 2, 0}))
        .set("ReadOnlyId", toJson(EngineHandle{18446744073709551614ULL}));
}
Result<AssetIndex> index() {
    ProjectContent c;
    c.directory = io::kNativePathStyle == io::PathStyle::Windows ? "C:/game" : "/game";
    c.projectFile = c.directory + "/Game.frostproj";
    c.assets = {{EngineHandle{1}, "Assets/Main.fscene", "Scene"}};
    return AssetIndex::create(std::move(c));
}
TEST(SerializedProperties, ChangedFieldPatchRetainsWholeActualBeforeAndExactEntity) {
    SerializedComponent before{"Assets/Main.fscene", "Transform", EngineHandle{18446744073709551614ULL}, fields()};
    auto after = fields();
    after.set("Translation", rpc::Json::array({5, 6, 0}));
    auto edit = serializedComponentEdit(before, after);
    ASSERT_TRUE(edit);
    EXPECT_EQ(engineHandleAt(*edit, "entity"), before.entity);
    ASSERT_NE(edit->find("expected"), nullptr);
    EXPECT_EQ(*edit->find("expected"), before.fields);
    ASSERT_NE(edit->find("fields"), nullptr);
    EXPECT_EQ(edit->find("fields")->size(), 1u);
    EXPECT_EQ(edit->find("fields")->find("ReadOnlyId"), nullptr);
    EXPECT_FALSE(serializedComponentEdit(before, fields()));
    EXPECT_FALSE(serializedComponentEdit(before, rpc::Json::object().set("Translation", rpc::Json::array({0, 0, 0}))));
}
TEST(SerializedProperties, MissingExpectedSupportIsReadOnlyAndChangedReplyIdentityIsRefused) {
    EXPECT_FALSE(supportsExpectedComponentSet(
        rpc::Json::object().set("name", "component.set").set("params", rpc::Json::array())));
    EXPECT_TRUE(supportsExpectedComponentSet(
        rpc::Json::object()
            .set("name", "component.set")
            .set("params", rpc::Json::array({rpc::Json::object().set("name", "expected").set("type", "Json")}))));
    auto actual = index();
    ASSERT_TRUE(actual);
    auto reply = rpc::Json::object()
                     .set("scene", "Assets/Main.fscene")
                     .set("entity", toJson(EngineHandle{9007199254740993ULL}))
                     .set("component", "Transform")
                     .set("fields", fields());
    auto read = parseSerializedComponent(*actual, "Assets/Main.fscene", "Transform", reply);
    ASSERT_TRUE(read);
    EXPECT_EQ(read->entity.value, 9007199254740993ULL);
    EXPECT_FALSE(parseSerializedComponent(*actual, "Assets/Main.fscene", "Transform", reply, EngineHandle{2}));
    reply.set("entity", 9007199254740992.0);
    EXPECT_FALSE(parseSerializedComponent(*actual, "Assets/Main.fscene", "Transform", reply));
}
} // namespace
} // namespace frost::engine
