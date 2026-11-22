// SPDX-License-Identifier: Apache-2.0
// Worker-only immutable native GLSL source/receipt handling.
#pragma once
#include "frost/engine/shader_language.hpp"
#include "frost/io/file_system.hpp"
#include "frost/lang/json.hpp"

#include <map>
namespace frost::app {
using ShaderLanguageImages = std::map<std::string, std::string>;
struct ShaderLanguageCapture {
    std::string root, program, query, programPhysical;
    lang::Json params;
    ShaderLanguageImages overlays;
};
struct ShaderLanguageAnswer {
    engine::ShaderLanguageResult result;
    std::vector<engine::ShaderLanguageSource> sources;
};
[[nodiscard]] Result<ShaderLanguageCapture> captureShaderLanguage(io::IFileSystem&, std::string root,
                                                                  const std::string& program, const std::string& query,
                                                                  const ShaderLanguageImages&, std::string method,
                                                                  size_t offset);
[[nodiscard]] Result<ShaderLanguageAnswer> validateShaderLanguage(io::IFileSystem&, const ShaderLanguageCapture&,
                                                                  const lang::Json&, std::string_view method);
} // namespace frost::app
