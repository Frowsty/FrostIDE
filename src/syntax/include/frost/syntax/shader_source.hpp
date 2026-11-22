// SPDX-License-Identifier: Apache-2.0
// Bounded lexical HLSL/ShaderLab source facts; never compiler or variant resolution.
#pragma once
#include "frost/syntax/highlight.hpp"

#include <stop_token>
#include <string>
#include <string_view>
namespace frost::syntax {
enum class ShaderSourceKind { None, Hlsl, ShaderLab };
[[nodiscard]] ShaderSourceKind shaderSourceKind(std::string_view path) noexcept;
struct ShaderInclude {
    text::ByteRange range; // Literal contents, without delimiters, in original source bytes.
    std::string target, directive;
    bool quoted{}, conditional{}, literal{};
};
struct ShaderSourceScan {
    std::vector<HighlightSpan> spans;
    std::vector<ShaderInclude> includes;
    bool complete = true;
};
inline constexpr std::size_t kShaderMaxSourceBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kShaderMaxSpans = 250000;
inline constexpr std::size_t kShaderMaxIncludes = 4096;
// Pure O(bytes) lexical scan, source offsets preserved. Handles ShaderLab program boundaries,
// comments/strings and literal include directives. Conditional activity/macros stay unevaluated.
// Over-limit input returns incomplete with no invented ranges or compiler diagnostics.
[[nodiscard]] ShaderSourceScan scanShaderSource(std::string_view source, ShaderSourceKind kind,
                                                const std::stop_token& stop = {});
} // namespace frost::syntax
