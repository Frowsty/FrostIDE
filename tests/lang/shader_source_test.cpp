// SPDX-License-Identifier: Apache-2.0
// Authored actual filesystem candidate/provenance tests; no compiler or Unity resolution oracle is claimed.
#include "frost/io/mem_fs.hpp"
#include "frost/lang/shader_source.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/document.hpp"

#include <gtest/gtest.h>
namespace frost::lang {
namespace {
std::string root() {
#ifdef _WIN32
    return "C:/shader-project";
#else
    return "/shader-project";
#endif
}
class ShaderSourceTest : public ::testing::Test {
protected:
    io::MemFs fs{systemClock()};
    ShaderLanguageInput input;
    void SetUp() override {
        ASSERT_TRUE(fs.createDirectories(root() + "/Assets"));
        ASSERT_TRUE(fs.createDirectories(root() + "/Includes"));
        input.path = root() + "/Assets/Test.shader";
        input.projectRoot = root();
        input.physicalProjectRoot = fs.physicalPath(root()).value();
    }
    void source(std::string value) {
        ASSERT_TRUE(fs.writeFileAtomic(input.path, value));
        auto document = text::Document::fromUtf8(value);
        ASSERT_TRUE(document);
        input.source = document->text();
    }
    Json params(std::size_t offset) {
        return Json::object()
            .set("textDocument", Json::object().set("uri", pathToUri(input.path).value()))
            .set("position", toJson(toLspPosition(input.source, ByteOffset{offset}, PositionEncoding::Utf16)));
    }
};
TEST_F(ShaderSourceTest, LiteralIncludesNavigateExactPhysicalSourceAndPublishConditionalProvenance) {
    const std::string value = "HLSLPROGRAM\n#if FEATURE\n#include \"Shared.hlsl\"\n#endif\nENDHLSL\n";
    source(value);
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/Assets/Shared.hlsl", "float4 value;"));
    auto result = queryShaderSource(fs, input, "textDocument/definition", params(value.find("Shared")));
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1);
    auto target = uriToPath(result->items()[0].stringAt("uri"));
    ASSERT_TRUE(target);
    EXPECT_EQ(documentKey(*target), documentKey(fs.physicalPath(root() + "/Assets/Shared.hlsl").value()));
    auto hover = queryShaderSource(fs, input, "textDocument/hover", params(value.find("Shared")));
    ASSERT_TRUE(hover);
    auto text = hover->find("contents")->stringAt("value");
    EXPECT_NE(text.find("not compiler-resolved"), text.npos);
    EXPECT_NE(text.find("conditional"), text.npos);
}
TEST_F(ShaderSourceTest, MultipleRootsRemainCandidatesAndMissingPackageMappingsRemainExplicit) {
    const std::string value =
        "HLSLPROGRAM\n#include \"Shared.hlsl\"\n#include \"Packages/com.example/Library.hlsl\"\nENDHLSL\n";
    source(value);
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/Assets/Shared.hlsl", "local"));
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/Includes/Shared.hlsl", "mapped"));
    input.roots.push_back({{},
                           root() + "/Includes",
                           fs.physicalPath(root() + "/Includes").value(),
                           "explicit compiler include root fixture"});
    auto result = queryShaderSource(fs, input, "textDocument/definition", params(value.find("Shared")));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->size(), 2);
    auto hover = queryShaderSource(fs, input, "textDocument/hover", params(value.find("Packages")));
    ASSERT_TRUE(hover);
    EXPECT_NE(hover->find("contents")->stringAt("value").find("No authoritative Unity package mapping"),
              std::string_view::npos);
    auto links = queryShaderSource(fs, input, "textDocument/documentLink", params(0));
    ASSERT_TRUE(links);
    EXPECT_EQ(links->size(), 2);
    EXPECT_EQ(links->items()[0].find("target"), nullptr);
}
TEST_F(ShaderSourceTest, MacroWrongUriAndUnretainedRootCannotBorrowSourceNavigation) {
    const std::string value = "HLSLPROGRAM\n#include TARGET\nENDHLSL\n";
    source(value);
    auto result = queryShaderSource(fs, input, "textDocument/definition", params(value.find("TARGET")));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->size(), 0);
    auto wrong = params(0);
    wrong.set("textDocument", Json::object().set("uri", pathToUri(root() + "/Other.hlsl").value()));
    EXPECT_FALSE(queryShaderSource(fs, input, "textDocument/definition", wrong));
    input.physicalProjectRoot = root() + "/different";
    EXPECT_FALSE(queryShaderSource(fs, input, "textDocument/documentLink", params(0)));
}
} // namespace
} // namespace frost::lang
