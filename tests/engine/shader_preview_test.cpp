// SPDX-License-Identifier: Apache-2.0
// Cross-process diagnostics and PNG receipt readers reject stale/malformed engine responses.
#include "frost/engine/shader_preview.hpp"
#include "frost/io/spawn_log.hpp"

#include <gtest/gtest.h>
namespace {
using frost::rpc::Json;
TEST(ShaderValidation, ExactSourceAndActualErrorLocationsRoundTrip) {
    const std::string source = "#type compute\nvoid main(){invalid;}\n";
    auto result = Json::object()
                      .set("protocol", "frostengine.shader-validation")
                      .set("version", 1)
                      .set("path", "Assets/Shaders/Broken.glsl")
                      .set("sourceSha256", frost::io::sha256Hex(source))
                      .set("variant", "base")
                      .set("compiler", "pinned engine glslang")
                      .set("valid", false)
                      .set("errors", 1)
                      .set("issues", Json::array({Json::object()
                                                      .set("path", "Assets/Shaders/Include/Broken.glslh")
                                                      .set("line", 7)
                                                      .set("column", 0)
                                                      .set("severity", "error")
                                                      .set("message", "undeclared identifier")}))
                      .set("inputs", Json::array());
    auto parsed = frost::engine::parseShaderValidation(result, "Assets/Shaders/Broken.glsl", source);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->issues.size(), 1U);
    EXPECT_EQ(parsed->issues[0].line, 7U);
    EXPECT_FALSE(parsed->valid);
    EXPECT_FALSE(frost::engine::parseShaderValidation(result, "Assets/Shaders/Broken.glsl", source + "edited"));
    result.set("valid", true);
    EXPECT_FALSE(frost::engine::parseShaderValidation(result, "Assets/Shaders/Broken.glsl", source));
}
TEST(EnginePreview, RefusesInventedPayloadOversizedDimensionsAndHashMismatch) {
    auto result = Json::object()
                      .set("protocol", "frostengine.asset-preview")
                      .set("version", 1)
                      .set("path", "Assets/Textures/T.png")
                      .set("sourceSha256", std::string(64, 'a'))
                      .set("renderer", "FrostEngine")
                      .set("mimeType", "image/png")
                      .set("sha256", std::string(64, 'b'))
                      .set("width", 256)
                      .set("height", 256)
                      .set("png", "bm90IGEgcG5n");
    EXPECT_FALSE(frost::engine::parseAssetPreview(result, "Assets/Textures/T.png", std::string(64, 'a')));
    result.set("width", 65536);
    EXPECT_FALSE(frost::engine::parseAssetPreview(result, "Assets/Textures/T.png", std::string(64, 'a')));
    result.set("renderer", "invented");
    EXPECT_FALSE(frost::engine::parseAssetPreview(result, "Assets/Textures/T.png", std::string(64, 'a')));
}
} // namespace
