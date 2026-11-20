// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/unreal_reflection.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
namespace frost::lang {
TEST(UnrealReflection, NavigatesOnlyActualTopLevelReflectionMacroCallbackLocations) {
    const auto root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/Synthetic" : "/Synthetic";
    const auto source = io::joinPath(root, "Source/Oracle.h").value();
    const auto header = io::joinPath(root, "PublicMetadata.h").value();
    auto location = [](const std::string& path, std::uint64_t offset) { return Json::object().set("path", path).set("offset", offset); };
    auto event = Json::object().set("name", "UCLASS").set("nested", false).set("at", location(source, 120)).set("definition", location(header, 42));
    CppPreprocessorResult result;
    result.macros = Json::array({event, Json(event).set("name", "IGNORED"), Json(event).set("nested", true)});
    auto macros = unrealReflectionMacros(result);
    ASSERT_TRUE(macros);
    ASSERT_EQ(macros->size(), 1U);
    EXPECT_EQ(macros->front().path, source);
    EXPECT_EQ(macros->front().offset, 120U);
    EXPECT_EQ(macros->front().definitionPath, header);
    EXPECT_EQ(macros->front().definitionOffset, 42U);
    result.macros = Json::array({Json(event).set("at", location("relative.h", 10))});
    EXPECT_FALSE(unrealReflectionMacros(result));
}
} // namespace frost::lang
