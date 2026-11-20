// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/json_schema.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/text/text_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
namespace frost::lang {
namespace {
#ifdef _WIN32
const std::string root = "C:/offline/";
#else
const std::string root = "/offline/";
#endif
text::Snapshot snapshot(std::string source) { return text::TextBuffer::fromUtf8(std::move(source)).value().snapshot(); }
Result<Json> complete(JsonSchemaService& service, const std::string& path, const text::Snapshot& text, ByteOffset at) {
    service.updateDocument(path, text);
    return sched::syncWait(
        service.request("textDocument/completion",
                        textDocumentPosition(path, toLspPosition(text, at, PositionEncoding::Utf16)).value()));
}
TEST(JsonSchema, NestedPresetErrorsUseExactCapturedValueRanges) {
    JsonSchemaService service;
    const auto path = root + "CMakePresets.json";
    const auto text = snapshot(R"({"version":"6","configurePresets":[{"name":false,"binaryDir":"build"}]})");
    service.updateDocument(path, text);
    const auto errors = service.diagnostics(path);
    ASSERT_EQ(errors.size(), 2U);
    ASSERT_TRUE(errors[0].range);
    ASSERT_TRUE(errors[1].range);
    EXPECT_EQ(text.text(*errors[0].range), "\"6\"");
    EXPECT_EQ(text.text(*errors[1].range), "false");
    EXPECT_EQ(errors[0].source, "frost-json-schema");
}
TEST(JsonSchema, IncompleteNestedObjectCompletesPropertiesOffline) {
    JsonSchemaService service;
    const auto path = root + "CMakePresets.json";
    const auto text = snapshot(R"({"version":6,"configurePresets":[{)");
    const auto result = complete(service, path, text, text.size());
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->isArray());
    EXPECT_TRUE(
        std::ranges::any_of(result->items(), [](const auto& item) { return item.stringAt("label") == "binaryDir"; }));
    EXPECT_TRUE(std::ranges::none_of(result->items(),
                                     [](const auto& item) { return item.stringAt("label") == "configurePresets"; }));
}
TEST(JsonSchema, LuaRuntimeEnumsReplaceWholeLiteralAndJsoncCommentsStayInert) {
    JsonSchemaService service;
    const auto path = root + ".luarc.json";
    const auto text = snapshot("{/* docs */\"runtime.version\":\"Lua 5.\",}");
    const auto result = complete(service, path, text, ByteOffset{text.size().value() - 3});
    ASSERT_TRUE(result);
    EXPECT_TRUE(
        std::ranges::any_of(result->items(), [](const auto& item) { return item.stringAt("label") == "\"Lua 5.4\""; }));
    service.updateDocument(path, snapshot("{/*docs*/\"runtime.version\":\"Lua 5.4\",}"));
    EXPECT_TRUE(service.diagnostics(path).empty());
}
TEST(JsonSchema, EngineProfilesValidateKnownFieldsWithoutInventingUnknownComponentErrors) {
    JsonSchemaService service;
    const auto path = root + "Game.frostproj";
    service.updateDocument(path, snapshot(R"({"General":{"Name":42},"CustomFutureEngineSetting":{"anything":true}})"));
    EXPECT_EQ(service.diagnostics(path).size(), 1U);
    service.updateDocument(path,
                           snapshot(R"({"General":{"Name":"Game"},"CustomFutureEngineSetting":{"anything":true}})"));
    EXPECT_TRUE(service.diagnostics(path).empty());
    service.removeDocument(path);
    EXPECT_TRUE(service.diagnostics(path).empty());
    EXPECT_FALSE(hasBundledJsonSchema(root + "unrelated.json"));
    service.updateDocument(root + "asset.fmeta", snapshot(R"({"Handle":13957500353814115645,"Type":"Scene"})"));
    EXPECT_TRUE(service.diagnostics(root + "asset.fmeta").empty());
}
TEST(JsonSchema, InvalidPositionAndCancellationRefuseWithoutClampingIntoAnotherValue) {
    JsonSchemaService service;
    const auto path = root + "CMakePresets.json";
    service.updateDocument(path, snapshot("{}"));
    auto params = textDocumentPosition(path, LspPosition{999, 0}).value();
    EXPECT_FALSE(sched::syncWait(service.request("textDocument/completion", params)));
    std::stop_source cancelled;
    cancelled.request_stop();
    auto result = sched::syncWait(service.request("textDocument/completion", params, cancelled.get_token()));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
}
Json actualSchemas() {
    return rpc::parseJson(R"({"protocol":"frostengine.document-schemas","version":1,"schemas":[{"extension":".fmat","serializer":"MaterialSerializer","schema":{"type":"object","properties":{"Program":{"type":"string","description":"Actual serializer program field"},"Enabled":{"type":"boolean"}}}}]})").value();
}
TEST(JsonSchema, SelectedEngineSchemasStayWithinTheirProjectAndAreWithdrawn) {
    JsonSchemaService service;
    const auto project=root+"Game";
    ASSERT_TRUE(service.installEngineSchemas(project,actualSchemas()));
    const auto generation=service.schemaGeneration();
    EXPECT_TRUE(service.handles(project+"/Assets/a.fmat"));
    EXPECT_FALSE(service.handles(project+"Sibling/Assets/a.fmat"));
    EXPECT_FALSE(service.handles(project+"/../Elsewhere/a.fmat"));
    const auto path=project+"/Assets/a.fmat";
    service.updateDocument(path,snapshot(R"({"Program":42,"FutureProperty":true})"));
    auto diagnostics=service.diagnostics(path);ASSERT_EQ(diagnostics.size(),1U);
    EXPECT_EQ(diagnostics[0].source,"frost-engine-schema-shape");
    service.clearEngineSchemas();EXPECT_GT(service.schemaGeneration(),generation);
    EXPECT_FALSE(service.handles(path));EXPECT_TRUE(service.diagnostics(path).empty());
}
TEST(JsonSchema, MalformedNestedEngineSchemaKeywordsNeverReplaceAdmittedShapes) {
    JsonSchemaService service;const auto project=root+"Game";
    ASSERT_TRUE(service.installEngineSchemas(project,actualSchemas()));const auto generation=service.schemaGeneration();
    for(const auto* bad:{R"({"type":42})",R"({"minimum":"zero"})",R"({"required":[false]})",R"({"properties":[]})",R"({"items":false})",R"({"enum":42})"}) {
        const auto schema=rpc::parseJson(std::string("{\"type\":\"object\",\"properties\":{\"Nested\":")+bad+"}}").value();
        auto entry=Json::object().set("extension",".fmat").set("serializer","actual").set("schema",schema);
        auto reply=Json::object().set("protocol","frostengine.document-schemas").set("version",1).set("schemas",Json::array({entry}));
        EXPECT_FALSE(service.installEngineSchemas(project,reply))<<bad;
        EXPECT_EQ(service.schemaGeneration(),generation);EXPECT_TRUE(service.handles(project+"/a.fmat"));
    }
}
TEST(JsonSchema, ActualEngineKeysCompleteAndInvalidReplacementKeepsPriorSchema) {
    JsonSchemaService service;const auto project=root+"Game";const auto path=project+"/Assets/a.fmat";
    ASSERT_TRUE(service.installEngineSchemas(project,actualSchemas()));
    const auto text=snapshot("{");auto result=complete(service,path,text,text.size());ASSERT_TRUE(result);
    EXPECT_TRUE(std::ranges::any_of(result->items(),[](const auto& item){return item.stringAt("label")=="Program";}));
    auto invalid=actualSchemas();invalid.set("version",2);
    EXPECT_FALSE(service.installEngineSchemas(project,invalid));EXPECT_TRUE(service.handles(path));
}
Json actualTableSchema() {
    return rpc::parseJson(R"({"protocol":"frostengine.table-schema","version":1,"path":"Assets/Weapons.ftable","reference":"Assets/Weapon.fschema","schemaInfo":{"path":"Assets/Weapon.fschema","resolveError":"","columns":[{"name":"Damage","type":"Int","declaredIn":"Assets/BaseWeapon.fschema","default":1,"definition":{"Name":"Damage","Type":"Int","Description":"Actual damage documentation","Min":0,"Max":100}},{"name":"Mode","type":"Enum","declaredIn":"Assets/Weapon.fschema","default":"Melee","definition":{"Name":"Mode","Type":"Enum","Values":["Melee","Magic"]}}]}})").value();
}
TEST(JsonSchema, ActualInheritedTableColumnsCompleteAndHoverWithDefaultsAndBounds) {
    JsonSchemaService service;const auto project=root+"Game";const auto path=project+"/Assets/Weapons.ftable";
    ASSERT_TRUE(service.installEngineSchemas(project,actualSchemas()));
    const auto text=snapshot(R"({"Schema":"Assets/Weapon.fschema","Rows":[{"Damage":101}]})");
    service.updateDocument(path,text);ASSERT_TRUE(service.installEngineTableSchema(path,text,actualTableSchema()));
    auto diagnostics=service.diagnostics(path);ASSERT_EQ(diagnostics.size(),1U);
    EXPECT_NE(diagnostics[0].message.find("maximum"),std::string::npos);
    auto hover=sched::syncWait(service.request("textDocument/hover",textDocumentPosition(path,toLspPosition(text,ByteOffset{text.text().find("Damage")+2},PositionEncoding::Utf16)).value()));
    ASSERT_TRUE(hover);const auto docs=hover->find("contents")->stringAt("value");
    EXPECT_NE(docs.find("Actual damage documentation"),std::string::npos);EXPECT_NE(docs.find("Default: 1"),std::string::npos);
    EXPECT_NE(docs.find("Maximum: 100"),std::string::npos);EXPECT_NE(docs.find("Assets/BaseWeapon.fschema"),std::string::npos);
    const auto incomplete=snapshot(R"({"Schema":"Assets/Weapon.fschema","Rows":[{)");
    auto result=complete(service,path,incomplete,incomplete.size());ASSERT_TRUE(result);
    EXPECT_TRUE(std::ranges::any_of(result->items(),[](const auto& item){return item.stringAt("label")=="Damage";}));
}
TEST(JsonSchema, ChangedTableSchemaReferenceWithdrawsOldColumnsAndMalformedExportCannotReplace) {
    JsonSchemaService service;const auto project=root+"Game";const auto path=project+"/Assets/Weapons.ftable";
    ASSERT_TRUE(service.installEngineSchemas(project,actualSchemas()));
    const auto text=snapshot(R"({"Schema":"Assets/Weapon.fschema","Rows":[]})");service.updateDocument(path,text);
    ASSERT_TRUE(service.installEngineTableSchema(path,text,actualTableSchema()));
    auto wrong=actualTableSchema();wrong.set("reference","Assets/Other.fschema");EXPECT_FALSE(service.installEngineTableSchema(path,text,wrong));
    const auto changed=snapshot(R"({"Schema":"Assets/Other.fschema","Rows":[{)");
    auto result=complete(service,path,changed,changed.size());ASSERT_TRUE(result);
    EXPECT_TRUE(std::ranges::none_of(result->items(),[](const auto& item){return item.stringAt("label")=="Damage";}));
}
TEST(JsonSchema, ReflectedAlternateEncodingsStayValidAndFlagNamesCompleteInsideArrayUnion) {
    JsonSchemaService service;const auto project=root+"Game";const auto path=project+"/a.fscene";
    const auto schema=rpc::parseJson(R"({"protocol":"frostengine.document-schemas","version":1,"schemas":[{"extension":".fscene","serializer":"actual","schema":{"type":"object","properties":{"Asset":{"type":["object","integer","string","null"],"x-engine-reference":"asset","x-engine-assetTypes":["Texture2D"]},"Target":{"type":["integer","string","null"],"x-engine-reference":"entity"},"Mode":{"type":["integer","string"],"x-engine-enumNames":["On","Off"]},"Flags":{"type":["array","string","integer"],"items":{"type":["string","integer"],"x-engine-enumNames":["First","Second"]}}}}]})").value();
    ASSERT_TRUE(service.installEngineSchemas(project,schema));
    service.updateDocument(path,snapshot(R"({"Asset":"Assets/Texture.png","Target":null,"Mode":1,"Flags":3})"));
    EXPECT_TRUE(service.diagnostics(path).empty());
    const auto text=snapshot(R"({"Flags":[""]})");
    auto result=complete(service,path,text,ByteOffset{text.text().find("\"\"")+1});ASSERT_TRUE(result);
    EXPECT_TRUE(std::ranges::any_of(result->items(),[](const auto& item){return item.stringAt("label")=="\"First\"";}));
}
TEST(JsonSchema, ActualVectorInputShapeAllowsEngineSpecialFloatStrings) {
    JsonSchemaService service;const auto project=root+"Game";const auto path=project+"/a.fscene";
    const auto schema=rpc::parseJson(R"({"protocol":"frostengine.document-schemas","version":1,"schemas":[{"extension":".fscene","serializer":"actual","schema":{"type":"object","properties":{"Vector":{"type":"array","items":{"type":["number","string"]}}}}}]})").value();
    ASSERT_TRUE(service.installEngineSchemas(project,schema));
    service.updateDocument(path,snapshot(R"({"Vector":["nan","inf","-inf",0]})"));EXPECT_TRUE(service.diagnostics(path).empty());
}
Json referenceSchemas() {
    return rpc::parseJson(R"({"protocol":"frostengine.document-schemas","version":1,"schemas":[{"extension":".fscene","serializer":"actual","schema":{"type":"object","properties":{"Texture":{"type":["integer","string","object","null"],"x-engine-reference":"asset","x-engine-assetTypes":["Texture2D"]},"Target":{"type":["integer","string","null"],"x-engine-reference":"entity"}}}}]})").value();
}
TEST(JsonSchema, TypedActualAssetCompletionAndExactU64DefinitionRevokeWithIndex) {
    JsonSchemaService service;const auto project=root+"Game";const auto path=project+"/a.fscene";
    ASSERT_TRUE(service.installEngineSchemas(project,referenceSchemas()));
    JsonAssetReferenceProvider provider;
    provider.resolve=[](const Json& value)->std::optional<JsonIndexedAsset> {
        if(value.kind()==Json::Kind::Integer&&value.asUnsigned()==13957500353814115645ULL)
            return JsonIndexedAsset{13957500353814115645ULL,"Assets/Texture.png","Texture2D"};return {};
    };
    provider.complete=[](const std::vector<std::string>& types,std::string_view prefix,std::size_t limit) {
        EXPECT_EQ(types,std::vector<std::string>{"Texture2D"});EXPECT_TRUE(prefix.empty());EXPECT_EQ(limit,5001U);
        return std::vector<JsonIndexedAsset>{{13957500353814115645ULL,"Assets/Texture.png","Texture2D"},{42,"Assets/Wrong.fmat","Material"}};
    };
    service.setAssetReferences(project,provider);
    const auto text=snapshot(R"({"Texture":13957500353814115645})");service.updateDocument(path,text);
    auto params=textDocumentPosition(path,toLspPosition(text,ByteOffset{14},PositionEncoding::Utf16)).value();
    auto definition=sched::syncWait(service.request("textDocument/definition",params));ASSERT_TRUE(definition);ASSERT_EQ(definition->size(),1U);
    EXPECT_EQ(definition->items()[0].stringAt("uri"),pathToUri(project+"/Assets/Texture.png").value());
    const auto empty=snapshot(R"({"Texture":""})");auto completion=complete(service,path,empty,ByteOffset{12});ASSERT_TRUE(completion);
    const auto* items=completion->find("items");ASSERT_NE(items,nullptr);ASSERT_EQ(items->size(),1U);EXPECT_EQ(items->items()[0].stringAt("label"),"Assets/Texture.png");
    service.setAssetReferences({},{});service.updateDocument(path,text);
    definition=sched::syncWait(service.request("textDocument/definition",params));ASSERT_TRUE(definition);EXPECT_EQ(definition->size(),0U);
}
TEST(JsonSchema, CapturedEntityReferencesNavigateExactIdAndRefuseDuplicateTargets) {
    JsonSchemaService service;const auto project=root+"Game";const auto path=project+"/a.fscene";
    ASSERT_TRUE(service.installEngineSchemas(project,referenceSchemas()));
    const auto text=snapshot(R"({"Target":"13957500353814115645","Entities":[{"ID":13957500353814115645,"Components":{"Tag":{"Tag":"Enemy"}}}]})");
    service.updateDocument(path,text);auto params=textDocumentPosition(path,toLspPosition(text,ByteOffset{15},PositionEncoding::Utf16)).value();
    auto definition=sched::syncWait(service.request("textDocument/definition",params));ASSERT_TRUE(definition);ASSERT_EQ(definition->size(),1U);
    const auto range=parseRange(*definition->items()[0].find("range"));ASSERT_TRUE(range);
    EXPECT_EQ(fromLspPosition(text,range->start,PositionEncoding::Utf16)->value(),text.text().rfind("13957500353814115645"));
    auto completion=complete(service,path,text,ByteOffset{15});ASSERT_TRUE(completion);const auto* items=completion->find("items");ASSERT_NE(items,nullptr);ASSERT_EQ(items->size(),1U);
    EXPECT_EQ(items->items()[0].find("textEdit")->stringAt("newText"),"13957500353814115645");
    service.updateDocument(path,snapshot(R"({"Target":7,"Entities":[{"ID":7},{"ID":7}]})"));
    params=textDocumentPosition(path,LspPosition{0,10}).value();definition=sched::syncWait(service.request("textDocument/definition",params));ASSERT_TRUE(definition);EXPECT_EQ(definition->size(),0U);
}
TEST(JsonSchema, InheritedColumnDefinitionUsesActualDeclaredFileAndCapturedOpenOverride) {
    JsonSchemaService service;const auto project=root+"Game";const auto path=project+"/Assets/Weapons.ftable";
    ASSERT_TRUE(service.installEngineSchemas(project,actualSchemas()));const auto table=snapshot(R"({"Schema":"Assets/Weapon.fschema","Rows":[{"Damage":1}]})");
    service.updateDocument(path,table);ASSERT_TRUE(service.installEngineTableSchema(path,table,actualTableSchema()));
    const auto declaration=snapshot("{\n\"Columns\":[{\"Name\":\"Other\"},{\"Name\":\"Damage\",\"Type\":\"Int\"}]\n}");int reads=0;
    service.setDeclarationReader([&](std::string_view selectedRoot,std::string_view file)->Result<text::Snapshot> {
        ++reads;EXPECT_EQ(documentKey(selectedRoot),documentKey(project));EXPECT_EQ(documentKey(file),documentKey(project+"/Assets/BaseWeapon.fschema"));return declaration;
    });
    auto params=textDocumentPosition(path,toLspPosition(table,ByteOffset{table.text().find("Damage")+2},PositionEncoding::Utf16)).value();
    auto result=sched::syncWait(service.request("textDocument/definition",params));ASSERT_TRUE(result);ASSERT_EQ(result->size(),1U);EXPECT_EQ(reads,1);
    auto range=parseRange(*result->items()[0].find("range"));ASSERT_TRUE(range);
    EXPECT_EQ(fromLspPosition(declaration,range->start,PositionEncoding::Utf16)->value(),declaration.text().find("\"Damage\""));
    const auto open=snapshot("{\n\n\"Columns\":[{\"Name\":\"Damage\",\"Type\":\"Int\"}]}" );
    service.updateDocument(project+"/Assets/BaseWeapon.fschema",open);
    result=sched::syncWait(service.request("textDocument/definition",params));ASSERT_TRUE(result);ASSERT_EQ(result->size(),1U);EXPECT_EQ(reads,1);
    range=parseRange(*result->items()[0].find("range"));ASSERT_TRUE(range);EXPECT_EQ(range->start.line,2U);
}
TEST(JsonSchema, DeclarationReadCannotPublishAfterSelectedSchemaGenerationChanges) {
    JsonSchemaService service;const auto project=root+"Game";const auto path=project+"/Assets/Weapons.ftable";
    ASSERT_TRUE(service.installEngineSchemas(project,actualSchemas()));const auto table=snapshot(R"({"Schema":"Assets/Weapon.fschema","Rows":[{"Damage":1}]})");
    service.updateDocument(path,table);ASSERT_TRUE(service.installEngineTableSchema(path,table,actualTableSchema()));
    service.setDeclarationReader([&](std::string_view,std::string_view)->Result<text::Snapshot> {service.clearEngineSchemas();return snapshot(R"({"Columns":[{"Name":"Damage"}]})");});
    auto params=textDocumentPosition(path,toLspPosition(table,ByteOffset{table.text().find("Damage")+2},PositionEncoding::Utf16)).value();
    auto result=sched::syncWait(service.request("textDocument/definition",params));ASSERT_FALSE(result);EXPECT_EQ(result.error().code(),ErrorCode::Cancelled);
}
} // namespace
} // namespace frost::lang
