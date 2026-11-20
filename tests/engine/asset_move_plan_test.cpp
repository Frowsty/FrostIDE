// SPDX-License-Identifier: Apache-2.0
// Authored FEC-9b producer wire and fail-closed move receipt cases; execution deferred.
#include "frost/engine/asset_move_plan.hpp"

#include <gtest/gtest.h>
namespace frost::engine {
namespace {
rpc::Json receipt() {
    auto guards = rpc::Json::array();
    guards.push(rpc::Json::object().set("path", "Assets/A.lua").set("size", 3).set("hash", assetMoveHash("abc")));
    guards.push(rpc::Json::object().set("path", "Assets/A.lua.fmeta").set("size", 2).set("hash", assetMoveHash("{}")));
    guards.push(rpc::Json::object().set("path", "Game.frostproj").set("size", 3).set("hash", assetMoveHash("old")));
    auto moves = rpc::Json::array();
    moves.push(rpc::Json::object().set("from", "Assets/A.lua").set("to", "Assets/B.lua"));
    moves.push(rpc::Json::object().set("from", "Assets/A.lua.fmeta").set("to", "Assets/B.lua.fmeta"));
    auto files = rpc::Json::array();
    files.push(rpc::Json::object().set("path", "Game.frostproj").set("before", "old").set("after", "new"));
    return rpc::Json::object()
        .set("version", 1)
        .set("lease", "session-lease")
        .set("receipt", "retained-operation")
        .set("oldPath", "Assets/A.lua")
        .set("path", "Assets/B.lua")
        .set("handle", toJson({18446744073709551615ULL}))
        .set("guards", std::move(guards))
        .set("moves", std::move(moves))
        .set("files", std::move(files));
}
} // namespace
TEST(AssetMovePlan, KeepsExactImagesAndFullUnsignedIdentity) {
    auto parsed = parseAssetMovePlan(receipt(), "Assets/A.lua", "Assets/B.lua", {18446744073709551615ULL});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->handle.value, 18446744073709551615ULL);
    ASSERT_EQ(parsed->files.size(), 1);
    EXPECT_EQ(parsed->files.front().before, "old");
    EXPECT_EQ(parsed->files.front().after, "new");
    EXPECT_EQ(parsed->moves.size(), 2);
}
TEST(AssetMovePlan, RefusesMissingGuardAndWrongHandle) {
    auto value = receipt();
    value.set("guards", rpc::Json::array());
    EXPECT_FALSE(parseAssetMovePlan(value, "Assets/A.lua", "Assets/B.lua", {18446744073709551615ULL}));
    EXPECT_FALSE(parseAssetMovePlan(receipt(), "Assets/A.lua", "Assets/B.lua", {7}));
}
TEST(AssetMovePlan, RefusesAliasTraversalAndDuplicateParticipants) {
    auto value = receipt();
    auto files = rpc::Json::array();
    files.push(rpc::Json::object().set("path", "Assets/../Game.frostproj").set("before", "a").set("after", "b"));
    value.set("files", files);
    EXPECT_FALSE(parseAssetMovePlan(value, "Assets/A.lua", "Assets/B.lua", {18446744073709551615ULL}));
    files = rpc::Json::array();
    files.push(rpc::Json::object().set("path", "Assets/A.lua").set("before", "a").set("after", "b"));
    value.set("files", files);
    EXPECT_FALSE(parseAssetMovePlan(value, "Assets/A.lua", "Assets/B.lua", {18446744073709551615ULL}));
}
TEST(AssetMovePlan, FingerprintsRawBytesIncludingNul) {
    EXPECT_EQ(assetMoveHash(""), "14695981039346656037");
    EXPECT_NE(assetMoveHash(std::string_view("a\0b", 3)), assetMoveHash("a"));
}
} // namespace frost::engine
