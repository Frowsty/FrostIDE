// SPDX-License-Identifier: Apache-2.0
// Checks scene merge source identity, conflict choices, and absent values.
#include "frost/engine/scene_merge.hpp"
#include "frost/io/spawn_log.hpp"

#include <gtest/gtest.h>
namespace frost::engine {
namespace {
SceneMergeRequest request() {
    return {
        .images = {"Assets/Scenes/A.fscene", R"({"Entities":[]})", R"({"Entities":[]})", R"({"Entities":[]})", true},
        .choices = {},
        .candidate = {}};
}
rpc::Json reply(const SceneMergeRequest& r) {
    return rpc::Json::object()
        .set("protocol", "frostengine.scene-merge")
        .set("version", 1)
        .set("path", r.images.path)
        .set("mode", "semantic")
        .set("candidateSha256", "")
        .set("sources", rpc::Json::object()
                            .set("baseExists", r.images.baseExists)
                            .set("base", r.images.baseExists ? io::sha256Hex(r.images.base) : "")
                            .set("ours", io::sha256Hex(r.images.ours))
                            .set("theirs", io::sha256Hex(r.images.theirs)))
        .set("result", r.images.ours)
        .set("conflicts", rpc::Json::array())
        .set("unresolved", 0)
        .set("clean", true)
        .set("validation", rpc::Json::object().set("valid", true).set("issues", rpc::Json::array()))
        .set("inputIssues", rpc::Json::array());
}
TEST(SceneMerge, RefusesWrongSourceIdentityValidityAndEntityAmbiguity) {
    auto r = request();
    auto actual = reply(r);
    ASSERT_TRUE(parseSceneMergeReport(actual, r));
    auto changed = r;
    changed.images.ours += ' ';
    EXPECT_FALSE(parseSceneMergeReport(actual, changed));
    actual.set("clean", false);
    EXPECT_FALSE(parseSceneMergeReport(actual, r));
    actual = reply(r);
    actual.set("result", R"({"Entities":[{"ID":13957500353814115645},{"ID":"13957500353814115645"}]})");
    EXPECT_FALSE(parseSceneMergeReport(actual, r));
    actual.set("result", R"({"Entities":[{"ID":13957500353814115645}]})");
    EXPECT_TRUE(parseSceneMergeReport(actual, r));
}
TEST(SceneMerge, ActualConflictSidesKeepAbsentDifferentFromNullAndChoicesAreCorrelated) {
    auto r = request();
    auto actual = reply(r);
    auto conflict = rpc::Json::object()
                        .set("path", "/Entities/@1/Future")
                        .set("kind", "delete-edit")
                        .set("choice", "")
                        .set("basePresent", true)
                        .set("base", nullptr)
                        .set("oursPresent", false)
                        .set("theirsPresent", true)
                        .set("theirs", 42);
    actual.set("conflicts", rpc::Json::array({conflict})).set("unresolved", 1).set("clean", false);
    auto parsed = parseSceneMergeReport(actual, r);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->conflicts.size(), 1U);
    EXPECT_TRUE(parsed->conflicts[0].base);
    EXPECT_FALSE(parsed->conflicts[0].ours);
    r.choices.push_back({"/Entities/@1/Future", "theirs"});
    EXPECT_FALSE(parseSceneMergeReport(actual, r));
    conflict.set("choice", "theirs");
    actual.set("conflicts", rpc::Json::array({conflict})).set("unresolved", 0).set("clean", true);
    EXPECT_TRUE(parseSceneMergeReport(actual, r));
    conflict.set("oursPresent", true);
    actual.set("conflicts", rpc::Json::array({conflict}));
    EXPECT_FALSE(parseSceneMergeReport(actual, r));
}
TEST(SceneMerge, ExplicitAbsentBaseAndRequestBoundaries) {
    auto r = request();
    r.images.baseExists = false;
    r.images.base.clear();
    EXPECT_TRUE(parseSceneMergeReport(reply(r), r));
    r.images.base = "{}";
    EXPECT_FALSE(sceneMergeParams(r));
    r = request();
    r.images.path = "Assets/../outside.fscene";
    EXPECT_FALSE(sceneMergeParams(r));
    r = request();
    r.images.ours.assign(1024 * 1024 + 1, ' ');
    EXPECT_FALSE(sceneMergeParams(r));
}
} // namespace
} // namespace frost::engine
