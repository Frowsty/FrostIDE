// SPDX-License-Identifier: Apache-2.0
// FEC4/5 schema specimens; future real CLI comparison is separately authored, not implied here.
#include "frost/engine/asset_index.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>
namespace frost::engine {
namespace {
constexpr EngineHandle Target{18446744073709551614ULL}, Source{9007199254740993ULL};
ProjectContent content() {
    ProjectContent c;
    c.directory = io::kNativePathStyle == io::PathStyle::Windows ? "C:/game" : "/game";
    c.projectFile = c.directory + "/Game.frostproj";
    c.assets = {{Target, "Assets/Hero.png", "Texture2D"}, {Source, "Assets/Main.fscene", "Scene"}};
    return c;
}
rpc::Json target() {
    return rpc::Json::object().set("handle", toJson(Target)).set("path", "Assets/Hero.png").set("type", "Texture2D");
}
rpc::Json references() {
    return rpc::Json::object()
        .set("asset", "Assets/Hero.png")
        .set("targets", rpc::Json::array({target()}))
        .set("count", 1)
        .set("references", rpc::Json::array({rpc::Json::object()
                                                 .set("handle", toJson(Source))
                                                 .set("path", "Assets/Main.fscene")
                                                 .set("type", "Scene")
                                                 .set("count", 3)
                                                 .set("targets", rpc::Json::array({"Assets/Hero.png"}))
                                                 .set("unsaved", true)}))
        .set("unreadable", rpc::Json::array());
}
TEST(AssetIndex, ExactListingOrderAndUnsignedHandleIdentityRemainOwned) {
    auto c = content();
    auto index = AssetIndex::create(c);
    ASSERT_TRUE(index);
    c.assets.clear();
    ASSERT_NE(index->find(Target), nullptr);
    EXPECT_EQ(index->find(Target)->path, "Assets/Hero.png");
    EXPECT_EQ(index->find("Assets/Main.fscene")->handle, Source);
    EXPECT_EQ(index->content().assets.size(), 2u);
    auto duplicate = content();
    duplicate.assets.push_back(duplicate.assets.front());
    EXPECT_FALSE(AssetIndex::create(duplicate));
    auto unsafe = content();
    unsafe.assets[0].path = "Assets/../outside";
    EXPECT_FALSE(AssetIndex::create(unsafe));
}
TEST(AssetIndex, ActualOccurrenceCountsAreNotDocumentCountsAndUnsavedStateSurvives) {
    auto index = AssetIndex::create(content());
    ASSERT_TRUE(index);
    auto result = parseAssetUsages(*index, Target, references());
    ASSERT_TRUE(result);
    EXPECT_EQ(result->occurrences, 3u);
    ASSERT_EQ(result->references.size(), 1u);
    EXPECT_TRUE(result->references[0].unsaved);
    EXPECT_EQ(result->codeVisionCount(), 1u);
}
TEST(AssetIndex, UnreadableSearchHasNoInventedCompleteCodeVisionCount) {
    auto index = AssetIndex::create(content());
    ASSERT_TRUE(index);
    auto reply = references();
    reply.set("unreadable", rpc::Json::array({"Assets/Broken.fscene"}));
    auto result = parseAssetUsages(*index, Target, reply);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->codeVisionCount());
    EXPECT_EQ(result->occurrences, 3u);
}
TEST(AssetIndex, UnregisteredUnsavedSceneHasNoFabricatedRegistryIdentity) {
    auto index = AssetIndex::create(content());
    ASSERT_TRUE(index);
    auto reply = references();
    reply.set("references", rpc::Json::array({rpc::Json::object()
                                                  .set("path", "Assets/New.fscene")
                                                  .set("type", "Scene")
                                                  .set("count", 1)
                                                  .set("targets", rpc::Json::array({"Assets/Hero.png"}))
                                                  .set("unsaved", true)}));
    auto parsed = parseAssetUsages(*index, Target, reply);
    ASSERT_TRUE(parsed);
    EXPECT_FALSE(parsed->references.front().indexed);
    EXPECT_FALSE(parsed->references.front().handle);
    EXPECT_TRUE(parsed->references.front().unsaved);
    reply.set("references", rpc::Json::array({rpc::Json::object()
                                                  .set("path", "Assets/Main.fscene")
                                                  .set("type", "Scene")
                                                  .set("count", 1)
                                                  .set("targets", rpc::Json::array({"Assets/Hero.png"}))
                                                  .set("unsaved", true)}));
    EXPECT_FALSE(parseAssetUsages(*index, Target, reply)); // An indexed identity cannot silently lose its handle.
}
TEST(AssetIndex, StaleIdentityCountersAndEscapedNavigationAreRejected) {
    auto index = AssetIndex::create(content());
    ASSERT_TRUE(index);
    auto bad = references();
    bad.set("count", 2);
    EXPECT_FALSE(parseAssetUsages(*index, Target, bad));
    bad = references();
    bad.set("targets", rpc::Json::array({target().set("handle", toJson(Source))}));
    EXPECT_FALSE(parseAssetUsages(*index, Target, bad));
    bad = references();
    bad.set("unreadable", rpc::Json::array({"../outside"}));
    EXPECT_FALSE(parseAssetUsages(*index, Target, bad));
    EXPECT_FALSE(parseAssetUsages(*index, EngineHandle{2}, references()));
}
} // namespace
} // namespace frost::engine
