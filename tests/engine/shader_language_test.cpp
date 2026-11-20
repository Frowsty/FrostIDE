// SPDX-License-Identifier: Apache-2.0
// Exact native language DTO projection, UTF8 byte coordinates and forged-range refusals.
#include "frost/engine/shader_language.hpp"
#include "frost/io/spawn_log.hpp"

#include <gtest/gtest.h>
namespace {
using frost::rpc::Json;
TEST(ShaderLanguage, CompletionUsesExactCapturedByteRangeAndRejectsInventedInsertion) {
    const std::string source = "// \xC3\xA5\nfloat actual;\nvoid main(){act;}\n";
    const std::string path = "Assets/Shaders/Main.glsl";
    std::vector<frost::engine::ShaderLanguageSource> files{{path, "C:/fixture/Main.glsl", source}};
    auto body = Json::object()
                    .set("protocol", "frostengine.shader-language")
                    .set("version", 1)
                    .set("queryPath", path)
                    .set("sourceSha256", frost::io::sha256Hex(source))
                    .set("method", "completion")
                    .set("diagnostics", Json::array())
                    .set("items", Json::array({Json::object()
                                                   .set("label", "actual")
                                                   .set("insertText", "actual")
                                                   .set("kind", "variable")
                                                   .set("detail", "global highp float")
                                                   .set("offset", source.find("act;"))
                                                   .set("length", 3)}));
    auto projected = frost::engine::projectShaderLanguage(body, "completion", path, files);
    ASSERT_TRUE(projected);
    const auto& item = projected->response.find("items")->items().front();
    EXPECT_EQ(item.find("textEdit")->stringAt("newText"), "actual");
    EXPECT_EQ(item.find("textEdit")->find("range")->find("start")->unsignedAt("line"), 2U);
    auto invalid = body;
    invalid.set("items", Json::array({Json::object()
                                          .set("label", "actual")
                                          .set("insertText", "invented();")
                                          .set("kind", "variable")
                                          .set("detail", "")
                                          .set("offset", 0)
                                          .set("length", 0)}));
    EXPECT_FALSE(frost::engine::projectShaderLanguage(invalid, "completion", path, files));
    EXPECT_FALSE(frost::engine::projectShaderLanguage(body, "completion", path, {}));
}
TEST(ShaderLanguage, DefinitionCannotNavigateToUncapturedSourceOrOutsideItsBytes) {
    const std::string path = "Assets/Shaders/Main.glsl", source = "float value;\n";
    std::vector<frost::engine::ShaderLanguageSource> files{{path, "C:/fixture/Main.glsl", source}};
    auto body =
        Json::object()
            .set("protocol", "frostengine.shader-language")
            .set("version", 1)
            .set("queryPath", path)
            .set("sourceSha256", frost::io::sha256Hex(source))
            .set("method", "definition")
            .set("diagnostics", Json::array())
            .set(
                "locations",
                Json::array(
                    {Json::object().set("path", "Assets/Shaders/Uncaptured.glslh").set("offset", 0).set("length", 1)}));
    EXPECT_FALSE(frost::engine::projectShaderLanguage(body, "definition", path, files));
    body.set("locations",
             Json::array({Json::object().set("path", path).set("offset", source.size() + 1).set("length", 0)}));
    EXPECT_FALSE(frost::engine::projectShaderLanguage(body, "definition", path, files));
}
} // namespace
