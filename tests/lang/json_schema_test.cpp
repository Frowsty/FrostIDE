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
} // namespace
} // namespace frost::lang
