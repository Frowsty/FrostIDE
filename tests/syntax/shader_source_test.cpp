// SPDX-License-Identifier: Apache-2.0
// Authored shader lexical source facts, comments/splices, embedded profiles and named limits.
#include "frost/syntax/shader_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
namespace frost::syntax {
namespace {
TEST(ShaderSource, ShaderLabEmbedsHlslWithoutTreatingCommentsOrStringsAsDirectives) {
    const std::string source = R"(Shader "Example/Source" {
// HLSLPROGRAM #include "not-code.hlsl"
Properties { _Value ("HLSLPROGRAM", Float) = 0 }
HLSLPROGRAM
float4 value;
#if FEATURE
#include_with_pragmas "Shared.hlsl"
#endif
ENDHLSL
Pass { Name "Forward" }
})";
    auto result = scanShaderSource(source, ShaderSourceKind::ShaderLab);
    ASSERT_TRUE(result.complete);
    ASSERT_EQ(result.includes.size(), 1);
    EXPECT_EQ(result.includes.front().target, "Shared.hlsl");
    EXPECT_TRUE(result.includes.front().conditional);
    EXPECT_TRUE(result.includes.front().literal);
    EXPECT_EQ(source.substr(result.includes.front().range.begin.value(), result.includes.front().range.size()),
              "Shared.hlsl");
    auto has = [&](std::string_view word, HighlightKind kind) {
        return std::ranges::any_of(result.spans, [&](const auto& span) {
            return span.kind == kind && source.substr(span.range.begin.value(), span.range.size()) == word;
        });
    };
    EXPECT_TRUE(has("Shader", HighlightKind::Keyword));
    EXPECT_TRUE(has("float4", HighlightKind::BuiltinType));
    EXPECT_TRUE(has("Pass", HighlightKind::Keyword));
    for (std::size_t i = 1; i < result.spans.size(); ++i)
        EXPECT_LE(result.spans[i - 1].range.end, result.spans[i].range.begin);
}
TEST(ShaderSource, SplicedCommentsAndMacroBodiesNeverInventLiteralIncludes) {
    // Construct the intended physical backslash-newline source independently of C++ escaping.
    const std::string spliced = std::string("// hidden ") + char(92) + "\n#include \"hidden.hlsl\"\n#define BODY " +
                                char(92) + "\n#include \"macro-body.hlsl\"\n#include TARGET\n#include \"real.hlsl\"\n";
    auto result = scanShaderSource(spliced, ShaderSourceKind::Hlsl);
    ASSERT_EQ(result.includes.size(), 2);
    EXPECT_FALSE(result.includes[0].literal);
    EXPECT_EQ(result.includes[1].target, "real.hlsl");
}
TEST(ShaderSource, LimitsAndCancellationAreExplicitAndExtensionProfilesAreSeparate) {
    EXPECT_EQ(shaderSourceKind("x.SHADER"), ShaderSourceKind::ShaderLab);
    EXPECT_EQ(shaderSourceKind("x.compute"), ShaderSourceKind::Hlsl);
    EXPECT_EQ(shaderSourceKind("x.cpp"), ShaderSourceKind::None);
    EXPECT_FALSE(scanShaderSource(std::string(kShaderMaxSourceBytes + 1, 'x'), ShaderSourceKind::Hlsl).complete);
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = scanShaderSource("float4 value;", ShaderSourceKind::Hlsl, stop.get_token());
    EXPECT_FALSE(cancelled.complete);
    EXPECT_TRUE(cancelled.spans.empty());
}
} // namespace
} // namespace frost::syntax
