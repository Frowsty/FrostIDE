// SPDX-License-Identifier: Apache-2.0
// Strict engine-native payload readers; no invented rows, file positions or image bytes.
#include "frost/engine/shader_preview.hpp"

#include "frost/io/spawn_log.hpp"

#include <algorithm>
#include <climits>

#include <sodium.h>
namespace frost::engine {
namespace {
bool hash(std::string_view s) {
    return s.size() == 64 &&
           std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool path(std::string_view s) {
    if ((!s.starts_with("Assets/") && !s.starts_with("Engine/")) || s.size() > 4096 ||
        s.find_first_of("\\:\0", 0, 3) != std::string_view::npos)
        return false;
    while (!s.empty()) {
        const auto slash = s.find('/');
        const auto part = s.substr(0, slash);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (slash == std::string_view::npos)
            break;
        s.remove_prefix(slash + 1);
    }
    return true;
}
Result<void> bad() { return fail(ErrorCode::Corrupt, "Malformed or stale native engine shader/preview response"); }
} // namespace
Result<ShaderValidation> parseShaderValidation(const rpc::Json& value, std::string_view requested,
                                               std::string_view source) {
    if (value.stringAt("protocol") != "frostengine.shader-validation" || value.unsignedAt("version") != 1 ||
        value.stringAt("path") != requested || !path(requested) ||
        value.stringAt("sourceSha256") != io::sha256Hex(source) || value.stringAt("variant") != "base")
        return std::unexpected(bad().error());
    const auto *issues = value.find("issues"), *inputs = value.find("inputs");
    if (!issues || !issues->isArray() || issues->size() > 4096 || !inputs || !inputs->isArray() ||
        inputs->size() > 128 || !value.boolAt("valid"))
        return std::unexpected(bad().error());
    ShaderValidation result;
    result.path = requested;
    result.sourceSha256 = value.stringAt("sourceSha256");
    result.compiler = value.stringAt("compiler");
    result.valid = *value.boolAt("valid");
    size_t errors = 0;
    for (const auto& row : issues->items()) {
        const auto line = row.unsignedAt("line"), column = row.unsignedAt("column");
        const auto severity = row.stringAt("severity");
        if (!path(row.stringAt("path")) || row.stringAt("message").empty() || row.stringAt("message").size() > 65536 ||
            !line || *line > INT_MAX || !column || *column > INT_MAX || (severity != "error" && severity != "warning"))
            return std::unexpected(bad().error());
        result.issues.push_back({std::string(row.stringAt("path")), std::string(row.stringAt("message")),
                                 std::string(severity), static_cast<uint32_t>(*line), static_cast<uint32_t>(*column)});
        if (severity == "error")
            ++errors;
    }
    if (value.unsignedAt("errors") != errors || result.valid != (errors == 0) || result.compiler.empty())
        return std::unexpected(bad().error());
    for (const auto& row : inputs->items()) {
        if (!path(row.stringAt("path")) || (!hash(row.stringAt("sha256")) && row.boolAt("missing") != true) ||
            row.stringAt("physicalPath").empty())
            return std::unexpected(bad().error());
        result.inputs.push_back({std::string(row.stringAt("path")), std::string(row.stringAt("physicalPath")),
                                 std::string(row.stringAt("sha256")), row.boolAt("missing").value_or(false)});
    }
    return result;
}
Result<AssetPreview> parseAssetPreview(const rpc::Json& value, std::string_view requested,
                                       std::string_view sourceHash) {
    const auto width = value.unsignedAt("width"), height = value.unsignedAt("height");
    const auto encoded = value.stringAt("png");
    if (value.stringAt("protocol") != "frostengine.asset-preview" || value.unsignedAt("version") != 1 ||
        value.stringAt("path") != requested || !path(requested) || !hash(sourceHash) ||
        value.stringAt("sourceSha256") != sourceHash || value.stringAt("renderer") != "FrostEngine" ||
        value.stringAt("mimeType") != "image/png" || !width || !height || *width != *height || *width < 32 ||
        *width > 256 || encoded.empty() || encoded.size() > 1400000 || !hash(value.stringAt("sha256")))
        return std::unexpected(bad().error());
    std::string bytes(1024 * 1024, '\0');
    size_t size = 0;
    const char* end = nullptr;
    if (sodium_base642bin(reinterpret_cast<unsigned char*>(bytes.data()), bytes.size(), encoded.data(), encoded.size(),
                          nullptr, &size, &end, sodium_base64_VARIANT_ORIGINAL) != 0 ||
        end != encoded.data() + encoded.size())
        return std::unexpected(bad().error());
    bytes.resize(size);
    if (bytes.size() < 33 || bytes.substr(0, 8) != std::string("\x89PNG\r\n\x1a\n", 8) ||
        bytes.substr(12, 4) != "IHDR" || io::sha256Hex(bytes) != value.stringAt("sha256"))
        return std::unexpected(bad().error());
    const auto u32 = [&](size_t i) {
        uint32_t n = 0;
        for (size_t j = 0; j < 4; ++j)
            n = (n << 8) | static_cast<unsigned char>(bytes[i + j]);
        return n;
    };
    if (u32(8) != 13 || u32(16) != *width || u32(20) != *height)
        return std::unexpected(bad().error());
    const auto* inputs = value.find("inputs");
    if (!inputs || !inputs->isArray() || inputs->size() > 128)
        return std::unexpected(bad().error());
    AssetPreview result{
        std::string(requested), std::string(sourceHash), std::move(bytes), static_cast<uint32_t>(*width), {}};
    for (const auto& input : inputs->items()) {
        if (!path(input.stringAt("path")) || !hash(input.stringAt("sha256")) || input.stringAt("physicalPath").empty())
            return std::unexpected(bad().error());
        result.inputs.push_back({std::string(input.stringAt("path")), std::string(input.stringAt("physicalPath")),
                                 std::string(input.stringAt("sha256"))});
    }
    return result;
}
} // namespace frost::engine
