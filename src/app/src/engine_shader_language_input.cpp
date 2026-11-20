// SPDX-License-Identifier: Apache-2.0
// Exact captured buffers and physical dependency/absence receipts, bounded before reading.
#include "engine_shader_language_input.hpp"

#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <set>
namespace frost::app {
namespace {
Result<std::pair<std::string, std::string>> virtualPath(io::IFileSystem& fs, std::string_view root,
                                                        std::string_view path) {
    auto actual = fs.physicalPath(path);
    if (!actual)
        return std::unexpected(actual.error());
    auto key = lang::documentKey(*actual), prefix = lang::documentKey(root);
    std::replace(key.begin(), key.end(), '\\', '/');
    std::replace(prefix.begin(), prefix.end(), '\\', '/');
    if (!prefix.ends_with('/'))
        prefix += '/';
    if (!key.starts_with(prefix))
        return fail(ErrorCode::PermissionDenied, "GLSL source is outside the selected physical project");
    std::string normalized = *actual;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const auto relative = normalized.substr(prefix.size());
    if (!relative.starts_with("Assets/Shaders/") || relative.find("../") != std::string::npos)
        return fail(ErrorCode::Unsupported, "Native GLSL editing requires the project's Assets/Shaders root");
    return std::pair{relative, *actual};
}
} // namespace
Result<ShaderLanguageCapture> captureShaderLanguage(io::IFileSystem& fs, std::string root, std::string program,
                                                    std::string query, const ShaderLanguageImages& images,
                                                    std::string method, size_t offset) {
    auto owning = virtualPath(fs, root, program), active = virtualPath(fs, root, query);
    if (!owning)
        return std::unexpected(owning.error());
    if (!active)
        return std::unexpected(active.error());
    ShaderLanguageCapture result;
    result.root = std::move(root);
    result.program = owning->first;
    result.query = active->first;
    result.programPhysical = owning->second;
    auto overlays = lang::Json::array();
    size_t bytes = 0;
    for (const auto& [path, image] : images) {
        auto mapped = virtualPath(fs, result.root, path);
        if (!mapped)
            continue;
        if (image.size() > 256 * 1024 || image.size() > 4 * 1024 * 1024 - bytes || result.overlays.size() >= 128)
            return fail(ErrorCode::OutOfRange, "Native GLSL editor snapshots exceed input budget");
        auto [it, inserted] = result.overlays.emplace(mapped->first, image);
        if (!inserted && it->second != image)
            return fail(ErrorCode::Busy, "Aliased GLSL editors have conflicting source images");
        if (!inserted)
            continue;
        bytes += image.size();
        overlays.push(lang::Json::object().set("path", mapped->first).set("source", image));
    }
    if (!result.overlays.contains(result.query))
        return fail(ErrorCode::Busy, "The queried shader is no longer open");
    result.params = lang::Json::object()
                        .set("path", result.program)
                        .set("queryPath", result.query)
                        .set("shaderRoot", "Assets/Shaders")
                        .set("method", std::move(method))
                        .set("offset", offset)
                        .set("overlays", std::move(overlays));
    if (result.params.dump().size() > 768 * 1024)
        return fail(ErrorCode::OutOfRange, "Native GLSL snapshots exceed the authenticated transport budget");
    return result;
}
Result<ShaderLanguageAnswer> validateShaderLanguage(io::IFileSystem& fs, const ShaderLanguageCapture& capture,
                                                    const lang::Json& body, std::string_view method) {
    const auto* inputs = body.find("inputs");
    if (!inputs || !inputs->isArray() || inputs->size() > 128 || body.stringAt("path") != capture.program)
        return fail(ErrorCode::Corrupt, "Native GLSL input receipt schema differs");
    ShaderLanguageAnswer answer;
    size_t total = 0;
    std::set<std::string> seen;
    for (const auto& input : inputs->items()) {
        const std::string path(input.stringAt("path"));
        if (!path.starts_with("Assets/Shaders/") || path.find("..") != std::string::npos ||
            path.find_first_of("\\:\0", 0, 3) != std::string::npos || !seen.insert(path).second)
            return fail(ErrorCode::Corrupt, "Native GLSL receipt path is unsafe or duplicate");
        auto joined = io::joinPath(capture.root, path);
        if (!joined)
            return std::unexpected(joined.error());
        if (input.boolAt("missing") == true) {
            auto exists = fs.exists(*joined);
            if (!exists || *exists || capture.overlays.contains(path))
                return fail(ErrorCode::Busy, "A missing GLSL dependency appeared");
            continue;
        }
        auto mapped = virtualPath(fs, capture.root, *joined);
        if (!mapped)
            return std::unexpected(mapped.error());
        auto reported = fs.physicalPath(input.stringAt("physicalPath"));
        if (!reported || lang::documentKey(*reported) != lang::documentKey(mapped->second))
            return fail(ErrorCode::Busy, "GLSL dependency physical identity changed");
        std::string bytes;
        if (input.boolAt("overlay") == true) {
            auto found = capture.overlays.find(path);
            if (found == capture.overlays.end())
                return fail(ErrorCode::Corrupt, "Native GLSL claims an uncaptured overlay");
            bytes = found->second;
        } else {
            if (capture.overlays.contains(path))
                return fail(ErrorCode::Corrupt, "Native GLSL ignored an authoritative editor image");
            auto read = fs.readFileSnapshot(mapped->second, 256 * 1024);
            if (!read)
                return std::unexpected(read.error());
            bytes = std::move(*read);
            if (bytes.starts_with("\xEF\xBB\xBF"))
                bytes.erase(0, 3);
        }
        if (bytes.size() > 4 * 1024 * 1024 - total || io::sha256Hex(bytes) != input.stringAt("sha256"))
            return fail(ErrorCode::Busy, "GLSL dependency bytes changed");
        total += bytes.size();
        answer.sources.push_back({path, mapped->second, std::move(bytes)});
    }
    auto projected = engine::projectShaderLanguage(body, method, capture.query, answer.sources);
    if (!projected)
        return std::unexpected(projected.error());
    answer.result = std::move(*projected);
    return answer;
}
} // namespace frost::app
