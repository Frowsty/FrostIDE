// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/unity_assets.hpp"
#include <gtest/gtest.h>
using namespace frost::lang;
namespace {
const std::string guid = "0123456789abcdef0123456789abcdef";
TEST(UnityAssets, OriginalGuidNavigationAndByteOffsets) {
    const std::vector<UnityAssetSnapshot> files{
        {"Assets/é.cs.meta", "fileFormatVersion: 2\r\nguid: " + guid + "\r\n"},
        {"Assets/Scene.unity", "%YAML 1.1\r\n--- !u!114 &1\r\nMonoBehaviour:\r\n  m_Script: {fileID: 11500000, guid: " + guid + ", type: 3}\r\n"}};
    UnityAssetIndex index; index.reset(files);
    ASSERT_TRUE(index.resolve(guid)); EXPECT_EQ(index.resolve(guid)->path, "Assets/é.cs");
    const auto usages = index.usages(guid); ASSERT_EQ(usages.size(), 1);
    EXPECT_EQ(files[1].contents.substr(usages[0].offset, 32), guid);
    EXPECT_EQ(usages[0].fileId, "11500000");
}
TEST(UnityAssets, DuplicateGuidDoesNotGuessTargetAndResetDropsOldRows) {
    std::vector<UnityAssetSnapshot> files{{"Assets/A.meta", "guid: " + guid}, {"Assets/B.meta", "guid: " + guid}};
    UnityAssetIndex index; index.reset(files); EXPECT_FALSE(index.resolve(guid));
    index.reset({}); EXPECT_FALSE(index.resolve(guid)); EXPECT_TRUE(index.usages(guid).empty());
}
TEST(UnityAssets, TextAndQuotedScalarsAreNotSemanticUsages) {
    const std::string ref = "{fileID: 1, guid: " + guid + ", type: 3}";
    const std::vector<UnityAssetSnapshot> files{{"Assets/S.prefab", "%YAML 1.1\n# " + ref + "\n  text: '" + ref + "'\n  text: |\n    fake: " + ref + "\n"}, {"Assets/Readme.txt", ref}};
    UnityAssetIndex index; index.reset(files); EXPECT_TRUE(index.usages(guid).empty());
}
TEST(UnityAssets, MovePreservesOriginalMetaAndRefusesIncompleteOrOccupiedPair) {
    std::vector<UnityAssetSnapshot> files{{"Assets/A.cs", "// original\r\n"}, {"Assets/A.cs.meta", "guid: " + guid + "\r\nimporter: unchanged\r\n"}};
    const auto plan = planUnityAssetMove(files, "Assets/A.cs", "Assets/B.cs");
    ASSERT_TRUE(plan); ASSERT_EQ(plan->size(), 2); EXPECT_EQ((*plan)[1].expectedBytes, files[1].contents);
    EXPECT_EQ((*plan)[1].to, "Assets/B.cs.meta");
    EXPECT_FALSE(planUnityAssetMove(files, "Assets/A.cs.meta", "Assets/B.cs.meta"));
    files.push_back({"Assets/B.cs.meta", "foreign"}); EXPECT_FALSE(planUnityAssetMove(files, "Assets/A.cs", "Assets/B.cs"));
    files.resize(1); EXPECT_FALSE(planUnityAssetMove(files, "Assets/A.cs", "Assets/B.cs"));
}
}

namespace {
TEST(UnityAssets, ArrayItemsInlineCommentsAndQuotedContinuations) {
    const std::string reference = "{fileID: -123, guid: " + guid + ", type: 3}";
    const std::string yaml = "%YAML 1.1\r\n  array:\r\n  - " + reference + " # real\r\n  text: 'continued\r\n    fake: " + reference + "\r\n    end'\r\n  actual: " + reference + "\r\n";
    const std::vector<UnityAssetSnapshot> files{{"Assets/S.prefab", yaml}};
    UnityAssetIndex index; index.reset(files);
    ASSERT_EQ(index.usages(guid).size(), 2);
    for (const auto& usage : index.usages(guid)) EXPECT_EQ(yaml.substr(usage.offset, 32), guid);
}
TEST(UnityAssets, OversizedLineAndUnknownEncodingAreExplicitlyIncomplete) {
    const std::vector<UnityAssetSnapshot> files{{"Assets/S.prefab", "%YAML 1.1\n" + std::string(16385, '9')},
        {"Assets/B.asset", std::string("\xff\xfe\0x", 4)}};
    UnityAssetIndex index; index.reset(files);
    EXPECT_TRUE(index.usages(guid).empty()); EXPECT_EQ(index.diagnostics().size(), 2);
}
}
namespace {
TEST(UnityAssets, ComponentBoundariesArePortableAcrossNativeSeparators) {
    EXPECT_TRUE(isUnityAssetsPath("C:/Project", "c:\\project\\Assets\\A.cs", frost::io::PathStyle::Windows));
    EXPECT_TRUE(isUnityAssetsPath("C:/Project", "C:/Project/Assets", frost::io::PathStyle::Windows));
    EXPECT_FALSE(isUnityAssetsPath("C:/Project", "C:/Project/AssetsOther/A.cs", frost::io::PathStyle::Windows));
    EXPECT_TRUE(isUnityAssetsPath("/project", "/project/Assets/A.cs", frost::io::PathStyle::Posix));
    EXPECT_FALSE(isUnityAssetsPath("/project", "/project/AssetsOther/A.cs", frost::io::PathStyle::Posix));
}
}
