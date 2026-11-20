// SPDX-License-Identifier: Apache-2.0
// Compiler-derived names/types and exact captured byte ranges; no inferred semantic candidates.
#include "frost/engine/shader_language.hpp"

#include "frost/io/spawn_log.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <climits>
namespace frost::engine {
namespace {
const ShaderLanguageSource* source(std::span<const ShaderLanguageSource> files, std::string_view path) {
    auto found = std::ranges::find(files, path, &ShaderLanguageSource::path);
    return found == files.end() ? nullptr : &*found;
}
bool boundary(std::string_view s, size_t i) {
    return i <= s.size() && (i == s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80);
}
rpc::Json position(std::string_view s, size_t at) {
    size_t line = 0, start = 0;
    for (size_t i = 0; i < at; ++i)
        if (s[i] == '\n') {
            ++line;
            start = i + 1;
        }
    return rpc::Json::object().set("line", line).set("character", at - start);
}
Result<rpc::Json> range(const rpc::Json& row, std::string_view bytes) {
    auto offset = row.unsignedAt("offset"), length = row.unsignedAt("length");
    if (!offset || !length || *offset > bytes.size() || *length > bytes.size() - *offset || !boundary(bytes, *offset) ||
        !boundary(bytes, *offset + *length))
        return fail(ErrorCode::Corrupt, "Native GLSL range exceeds captured source");
    return rpc::Json::object().set("start", position(bytes, *offset)).set("end", position(bytes, *offset + *length));
}
bool identifier(std::string_view name) {
    return !name.empty() && name.size() <= 1024 && std::ranges::all_of(name, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}
int kind(std::string_view value, bool completion) {
    return value == "function" ? (completion ? 3 : 12)
           : value == "field"  ? (completion ? 5 : 8)
           : value == "file"   ? (completion ? 17 : 1)
                               : (completion ? 6 : 13);
}
} // namespace
Result<ShaderLanguageResult> projectShaderLanguage(const rpc::Json& body, std::string_view method,
                                                   std::string_view query,
                                                   std::span<const ShaderLanguageSource> files) {
    using rpc::Json;
    const auto* active = source(files, query);
    if (!active || body.stringAt("protocol") != "frostengine.shader-language" || body.unsignedAt("version") != 1 ||
        body.stringAt("queryPath") != query || body.stringAt("method") != method ||
        body.stringAt("sourceSha256") != io::sha256Hex(active->bytes))
        return fail(ErrorCode::Corrupt, "Native GLSL query/source identity differs");
    ShaderLanguageResult result;
    const auto* diagnostics = body.find("diagnostics");
    if (!diagnostics || !diagnostics->isArray() || diagnostics->size() > 4096)
        return fail(ErrorCode::Corrupt, "Native GLSL diagnostics exceed schema");
    for (const auto& row : diagnostics->items()) {
        auto line = row.unsignedAt("line"), column = row.unsignedAt("column");
        const auto severity = row.stringAt("severity");
        if (!source(files, row.stringAt("path")) || !line || !column || *line > INT_MAX || *column > INT_MAX ||
            row.stringAt("message").empty() || row.stringAt("message").size() > 65536 ||
            (severity != "error" && severity != "warning"))
            return fail(ErrorCode::Corrupt, "Native GLSL diagnostic location is not a captured input");
        result.diagnostics.push_back({std::string(row.stringAt("path")), std::string(row.stringAt("message")),
                                      std::string(severity), static_cast<uint32_t>(*line),
                                      static_cast<uint32_t>(*column)});
    }
    if (method == "completion") {
        const auto* items = body.find("items");
        if (!items || !items->isArray() || items->size() > 1024)
            return fail(ErrorCode::Corrupt, "Native GLSL completion exceeds schema");
        auto converted = Json::array();
        for (const auto& item : items->items()) {
            const auto label = item.stringAt("label"), insert = item.stringAt("insertText"),
                       type = item.stringAt("kind");
            if (label.empty() || insert.empty() || label.size() > 4096 || item.stringAt("detail").size() > 65536)
                return fail(ErrorCode::Corrupt, "Native GLSL completion source is malformed");
            if (type == "file") {
                if (!insert.ends_with(label) || insert.find_first_of("\r\n\"\\:") != std::string_view::npos ||
                    insert.find("..") != std::string_view::npos || insert.starts_with('/'))
                    return fail(ErrorCode::Corrupt, "Native GLSL include completion is unsafe");
            } else if (label != insert || !identifier(insert) ||
                       (type != "function" && type != "variable" && type != "parameter" && type != "field"))
                return fail(ErrorCode::Corrupt, "Native GLSL semantic completion is not a compiler identifier");
            auto edit = range(item, active->bytes);
            if (!edit)
                return std::unexpected(edit.error());
            converted.push(Json::object()
                               .set("label", label)
                               .set("detail", item.stringAt("detail"))
                               .set("kind", kind(type, true))
                               .set("insertTextFormat", 1)
                               .set("textEdit", Json::object().set("range", std::move(*edit)).set("newText", insert)));
        }
        result.response = Json::object().set("isIncomplete", false).set("items", std::move(converted));
    } else if (method == "definition") {
        auto converted = Json::array();
        const auto* locations = body.find("locations");
        if (!locations || !locations->isArray() || locations->size() > 1024)
            return fail(ErrorCode::Corrupt, "Native GLSL definition exceeds schema");
        for (const auto& row : locations->items()) {
            const auto* input = source(files, row.stringAt("path"));
            if (!input)
                return fail(ErrorCode::Corrupt, "Native GLSL definition lacks a captured source");
            auto span = range(row, input->bytes);
            if (!span)
                return std::unexpected(span.error());
            auto uri = lang::pathToUri(input->physicalPath);
            if (!uri)
                return std::unexpected(uri.error());
            converted.push(Json::object().set("uri", *uri).set("range", std::move(*span)));
        }
        result.response = std::move(converted);
    } else if (method == "hover") {
        const auto text = body.stringAt("hover");
        if (text.size() > 65536)
            return fail(ErrorCode::Corrupt, "Native GLSL hover exceeds schema");
        result.response =
            text.empty() ? Json{}
                         : Json::object().set("contents", Json::object().set("kind", "plaintext").set("value", text));
    } else if (method == "symbols") {
        const auto* symbols = body.find("symbols");
        if (!symbols || !symbols->isArray() || symbols->size() > 8192)
            return fail(ErrorCode::Corrupt, "Native GLSL outline exceeds schema");
        auto converted = Json::array();
        for (const auto& row : symbols->items()) {
            const auto* location = row.find("location");
            if (!location)
                return fail(ErrorCode::Corrupt, "Native GLSL outline lacks source");
            if (location->stringAt("path") != query)
                continue;
            if (!identifier(row.stringAt("name")))
                return fail(ErrorCode::Corrupt, "Native GLSL outline name is malformed");
            auto span = range(*location, active->bytes);
            if (!span)
                return std::unexpected(span.error());
            converted.push(Json::object()
                               .set("name", row.stringAt("name"))
                               .set("detail", row.stringAt("detail"))
                               .set("kind", kind(row.stringAt("kind"), false))
                               .set("range", *span)
                               .set("selectionRange", *span));
        }
        result.response = std::move(converted);
    } else if (method == "diagnostics")
        result.response = Json{};
    else
        return fail(ErrorCode::Unsupported, "Native GLSL query method unsupported");
    return result;
}
} // namespace frost::engine
