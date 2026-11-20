// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/unreal_reflection.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
#include <algorithm>
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

TEST(UnrealReflection, SpecifierCompletionIsSyntaxOnlyAndExcludesCommentsStringsAndNestedValues) {
    auto complete=[](std::string_view source){return unrealSpecifierCompletions(source,source.size());};
    auto list=complete("UPROPERTY(Blue"); ASSERT_TRUE(list);
    EXPECT_TRUE(std::ranges::any_of(list->items,[](const auto& item){return item.label=="BlueprintReadOnly"&&item.resolved;}));
    auto classConfig=complete("UCLASS(Conf"),propertyConfig=complete("UPROPERTY(Conf");
    ASSERT_TRUE(classConfig);ASSERT_TRUE(propertyConfig);
    const auto classItem=std::ranges::find(classConfig->items,std::string("Config"),&CompletionItem::label);
    const auto propertyItem=std::ranges::find(propertyConfig->items,std::string("Config"),&CompletionItem::label);
    ASSERT_NE(classItem,classConfig->items.end());ASSERT_NE(propertyItem,propertyConfig->items.end());
    EXPECT_EQ(classItem->insertText,"Config=${1}");EXPECT_TRUE(classItem->snippet);
    EXPECT_EQ(propertyItem->insertText,"Config");EXPECT_FALSE(propertyItem->snippet);
    EXPECT_FALSE(complete("// UPROPERTY(Blue"));
    EXPECT_FALSE(complete("/* UCLASS(Blue"));
    EXPECT_FALSE(complete("const char* text=\"UPROPERTY(Blue"));
    EXPECT_FALSE(complete("auto text=R\"tag(UPROPERTY(Blue"));
    EXPECT_FALSE(complete("UPROPERTY(meta=(DisplayName=\"Blue"));
    EXPECT_FALSE(complete("UPROPERTY(Category=SomeValue"));
    EXPECT_TRUE(complete("UPROPERTY(meta=(DisplayName=\"X\"), Blue"));
}
TEST(UnrealReflection, InspectionsUseActualArgumentEvidenceAndIgnoreNestedMetadataAndUnknownSpecifiers) {
    const auto root=io::kNativePathStyle==io::PathStyle::Windows?"C:/Synthetic":"/Synthetic";
    const auto path=io::joinPath(root,"Oracle.h").value();
    auto row=Json::object().set("name","UPROPERTY").set("nested",false)
        .set("at",Json::object().set("path",path).set("line",8).set("column",1));
    CppPreprocessorResult result;
    result.macros=Json::array({row});
    auto missing=inspectUnrealReflection(result); ASSERT_FALSE(missing); EXPECT_EQ(missing.error().code(),ErrorCode::Unsupported);
    result.macros=Json::array({Json(row).set("argumentTokens",Json::array({Json::array({"BlueprintReadOnly",",","BlueprintReadWrite",",","meta","=","(","VisibleAnywhere",",","EditAnywhere",")"})}))});
    auto issues=inspectUnrealReflection(result); ASSERT_TRUE(issues); ASSERT_EQ(issues->size(),1U);
    EXPECT_EQ(issues->front().path,path); EXPECT_EQ(issues->front().line,8U);
    EXPECT_NE(issues->front().message.find("BlueprintReadOnly"),std::string::npos);
    result.macros=Json::array({Json(row).set("argumentTokens",Json::array({Json::array({"FutureSpecifier",",","BlueprintReadOnly"})}))});
    issues=inspectUnrealReflection(result); ASSERT_TRUE(issues); EXPECT_TRUE(issues->empty());
}
} // namespace frost::lang
