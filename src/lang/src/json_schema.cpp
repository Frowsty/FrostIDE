// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/json_schema.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
namespace frost::lang {
namespace {
// Bundled JSON Schema structural profiles. Sources: CMake cmake-presets(7), LuaLS
// settings (runtime/workspace/diagnostics/completion), and FrostEngine project/data formats.
// These intentionally permit unmodeled properties. They do not claim full preset semantics,
// dynamic engine component schemas, reference integrity, or arbitrary remote JSON Schema support.
const Json& schemaFor(std::string_view path) {
    static const Json cmake = rpc::parseJson(R"schema({"type":"object","required":["version"],"properties":{
      "$schema":{"type":"string","description":"Schema URI; offline profile remains bundled."},
      "version":{"type":"integer","minimum":1,"description":"CMake preset format version."},
      "cmakeMinimumRequired":{"type":"object","properties":{"major":{"type":"integer"},"minor":{"type":"integer"},"patch":{"type":"integer"}}},
      "include":{"type":"array","items":{"type":"string"}},"vendor":{"type":"object"},
      "configurePresets":{"type":"array","items":{"type":"object","required":["name"],"properties":{
        "name":{"type":"string"},"displayName":{"type":"string"},"description":{"type":"string"},"hidden":{"type":"boolean"},
        "inherits":{"type":["string","array"]},"generator":{"type":"string"},"binaryDir":{"type":"string"},"installDir":{"type":"string"},
        "toolchainFile":{"type":"string"},"cacheVariables":{"type":"object"},"environment":{"type":"object"},"condition":{"type":["object","boolean","null"]}}},
      "buildPresets":{"type":"array","items":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"configurePreset":{"type":"string"},"configuration":{"type":"string"},"jobs":{"type":"integer","minimum":1},"targets":{"type":["string","array"]},"cleanFirst":{"type":"boolean"}}}},
      "testPresets":{"type":"array","items":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"configurePreset":{"type":"string"},"configuration":{"type":"string"}}}},
      "packagePresets":{"type":"array","items":{"type":"object"}},"workflowPresets":{"type":"array","items":{"type":"object"}}
    }})schema")
                                  .value();
    static const Json lua = rpc::parseJson(R"schema({"type":"object","properties":{
      "$schema":{"type":"string"},"runtime.version":{"type":"string","enum":["Lua 5.1","Lua 5.2","Lua 5.3","Lua 5.4","LuaJIT"],"description":"Lua runtime used by LuaLS."},
      "workspace.library":{"type":"array","items":{"type":"string"}},"workspace.checkThirdParty":{"type":["boolean","string"]},
      "diagnostics.enable":{"type":"boolean"},"diagnostics.globals":{"type":"array","items":{"type":"string"}},"diagnostics.disable":{"type":"array","items":{"type":"string"}},
      "completion.enable":{"type":"boolean"},"hint.enable":{"type":"boolean"},"format.enable":{"type":"boolean"},
      "runtime":{"type":"object","properties":{"version":{"type":"string","enum":["Lua 5.1","Lua 5.2","Lua 5.3","Lua 5.4","LuaJIT"]}}},
      "workspace":{"type":"object","properties":{"library":{"type":"array","items":{"type":"string"}},"checkThirdParty":{"type":["boolean","string"]}}},
      "diagnostics":{"type":"object","properties":{"enable":{"type":"boolean"},"globals":{"type":"array","items":{"type":"string"}},"disable":{"type":"array","items":{"type":"string"}}}}
    }})schema")
                                .value();
    static const Json project = rpc::parseJson(R"schema({"type":"object","properties":{
      "General":{"type":"object","description":"Project identity and startup scene.","properties":{"Name":{"type":"string"},"Company":{"type":"string"},"Version":{"type":"string"},"StartScene":{"type":"integer","minimum":0}}},
      "Build":{"type":"object","properties":{"GameName":{"type":"string"},"OutputDirectory":{"type":"string"},"WindowTitle":{"type":"string"},"WindowWidth":{"type":"integer","minimum":1},"WindowHeight":{"type":"integer","minimum":1},"Fullscreen":{"type":"boolean"},"Resizable":{"type":"boolean"}}},
      "Input":{"type":"object","properties":{"Actions":{"type":"array","items":{"type":"object","properties":{"Name":{"type":"string"}}}}}},
      "Audio":{"type":"object","properties":{"MasterVolume":{"type":"number","minimum":0},"Groups":{"type":"array","items":{"type":"object","properties":{"Name":{"type":"string"},"Volume":{"type":"number","minimum":0},"Muted":{"type":"boolean"}}}}}},
      "Physics":{"type":"object","properties":{"Layers":{"type":"array","items":{"type":"string"}}}},"Render":{"type":"object","properties":{"RenderLayers":{"type":"array","items":{"type":"string"}}}}
    }})schema")
                                    .value();
    static const Json meta =
        rpc::parseJson(
            R"schema({"type":"object","properties":{"Handle":{"type":"integer","minimum":0},"Type":{"type":"string"},"Path":{"type":"string"},"ImporterVersion":{"type":"integer","minimum":0},"Settings":{"type":"object"}}})schema")
            .value();
    static const Json data =
        rpc::parseJson(
            R"schema({"type":"object","properties":{"Format":{"type":"string","enum":["FrostDataSchema"]},"Version":{"type":"integer","minimum":1},"Name":{"type":"string"},"Description":{"type":"string"},"Columns":{"type":"array","items":{"type":"object","required":["Name","Type"],"properties":{"Name":{"type":"string"},"Type":{"type":"string"},"Required":{"type":"boolean"},"Description":{"type":"string"},"Min":{"type":"number"},"Max":{"type":"number"},"MaxLength":{"type":"integer","minimum":0}}}}}})schema")
            .value();
    static const Json table =
        rpc::parseJson(
            R"schema({"type":"object","properties":{"Format":{"type":"string","enum":["FrostDataTable"]},"Version":{"type":"integer","minimum":1},"Schema":{"type":["string","object","integer"]},"Rows":{"type":"array","items":{"type":"object"}}}})schema")
            .value();
    const auto name = io::fileName(path);
    if (name == "CMakePresets.json" || name == "CMakeUserPresets.json")
        return cmake;
    if (name == ".luarc.json" || name == ".luarc.jsonc")
        return lua;
    if (path.ends_with(".frostproj") || path.ends_with(".feproject"))
        return project;
    if (path.ends_with(".fmeta"))
        return meta;
    if (path.ends_with(".fschema"))
        return data;
    return table;
}
struct Node {
    std::vector<std::string> path;
    std::size_t begin = 0, end = 0, keyBegin = 0, keyEnd = 0;
    bool object = false, array = false;
};
struct Scanner {
    std::string_view source;
    std::size_t at = 0;
    std::vector<Node> nodes;
    void space() {
        while (at < source.size() && std::isspace(static_cast<unsigned char>(source[at])))
            ++at;
    }
    std::pair<std::string, std::pair<std::size_t, std::size_t>> string() {
        const auto start = at++;
        bool escaped = false;
        while (at < source.size()) {
            const char ch = source[at++];
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                break;
        }
        const auto raw = source.substr(start, at - start);
        const auto decoded = rpc::parseJson(raw);
        return {decoded && decoded->isString() ? *decoded->asString() : std::string(raw.substr(1)), {start, at}};
    }
    void value(std::vector<std::string> path = {}, unsigned depth = 0, std::size_t keyBegin = 0,
               std::size_t keyEnd = 0) {
        space();
        if (depth > 128 || nodes.size() >= 100000)
            return;
        const auto index = nodes.size();
        const char ch = at < source.size() ? source[at] : '\0';
        nodes.push_back({path, at, source.size(), keyBegin, keyEnd, ch == '{', ch == '['});
        if (ch == '{' || ch == '[') {
            ++at;
            space();
            std::size_t element = 0;
            while (at < source.size() && source[at] != (ch == '{' ? '}' : ']')) {
                const auto previous = at;
                auto child = path;
                if (ch == '{') {
                    if (source[at] != '"')
                        break;
                    const auto key = string();
                    child.push_back(key.first);
                    space();
                    if (at >= source.size() || source[at] != ':') {
                        nodes.push_back({child, at, source.size(), key.second.first, key.second.second});
                        break;
                    }
                    ++at;
                    value(std::move(child), depth + 1, key.second.first, key.second.second);
                } else {
                    child.push_back(std::to_string(element++));
                    value(std::move(child), depth + 1);
                }
                space();
                if (at <= previous || at >= source.size() || source[at] != ',')
                    break;
                ++at;
                space();
            }
            if (at < source.size() && source[at] == (ch == '{' ? '}' : ']'))
                ++at;
        } else if (ch == '"')
            (void)string();
        else
            while (at < source.size() && source[at] != ',' && source[at] != '}' && source[at] != ']' &&
                   !std::isspace(static_cast<unsigned char>(source[at])))
                ++at;
        nodes[index].end = at;
    }
};
const Json* fieldSchema(const Json& root, std::span<const std::string> path) {
    const Json* schema = &root;
    for (const auto& component : path) {
        if (schema->stringAt("type") == "array")
            schema = schema->find("items");
        else {
            const auto* properties = schema->find("properties");
            schema = properties ? properties->find(component) : nullptr;
        }
        if (!schema)
            return nullptr;
    }
    return schema;
}
bool typeMatches(const Json& value, std::string_view type) {
    return type == "object"    ? value.isObject()
           : type == "array"   ? value.isArray()
           : type == "string"  ? value.isString()
           : type == "boolean" ? value.isBool()
           : type == "integer" ? value.isNumber() && std::trunc(*value.asDouble()) == *value.asDouble()
           : type == "number"  ? value.isNumber()
           : type == "null"    ? value.isNull()
                               : true;
}
void validate(const Json& schema, const Json& value, std::vector<std::string> path,
              std::vector<std::pair<std::vector<std::string>, std::string>>& errors, unsigned depth = 0) {
    if (depth > 128 || errors.size() >= 256)
        return;
    if (const auto* type = schema.find("type")) {
        const bool valid = type->isArray()
                               ? std::ranges::any_of(type->items(),
                                                     [&](const auto& item) {
                                                         return item.isString() && typeMatches(value, *item.asString());
                                                     })
                               : typeMatches(value, *type->asString());
        if (!valid) {
            errors.push_back({path, "Expected " + type->dump()});
            return;
        }
    }
    if (const auto* choices = schema.find("enum");
        choices && std::ranges::find(choices->items(), value) == choices->items().end())
        errors.push_back({path, "Value is outside the bundled schema enum"});
    if (const auto* minimum = schema.find("minimum");
        minimum && value.isNumber() && *value.asDouble() < *minimum->asDouble())
        errors.push_back({path, "Value is below the schema minimum " + minimum->dump()});
    if (value.isObject()) {
        if (const auto* required = schema.find("required"))
            for (const auto& key : required->items())
                if (!value.find(*key.asString()))
                    errors.push_back({path, "Missing required property " + *key.asString()});
        if (const auto* properties = schema.find("properties"))
            for (std::size_t i = 0; i < value.size(); ++i)
                if (const auto* childSchema = properties->find(value.keys()[i])) {
                    auto child = path;
                    child.push_back(value.keys()[i]);
                    validate(*childSchema, value.items()[i], std::move(child), errors, depth + 1);
                }
    } else if (value.isArray()) {
        if (const auto* items = schema.find("items"))
            for (std::size_t i = 0; i < value.size(); ++i) {
                auto child = path;
                child.push_back(std::to_string(i));
                validate(*items, value.items()[i], std::move(child), errors, depth + 1);
            }
    }
}
Result<std::string> normalized(std::string_view path, const text::Snapshot& snapshot) {
    if (snapshot.size().value() > 2U * 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "Offline JSON profile limit: 2 MiB");
    auto source = snapshot.text();
    return io::fileName(path).starts_with(".luarc.") ? rpc::normalizeJsonComments(source)
                                                     : Result<std::string>(std::move(source));
}
} // namespace
bool hasBundledJsonSchema(std::string_view path) {
    const auto name = io::fileName(path);
    return name == "CMakePresets.json" || name == "CMakeUserPresets.json" || name == ".luarc.json" ||
           name == ".luarc.jsonc" || path.ends_with(".frostproj") || path.ends_with(".feproject") ||
           path.ends_with(".fmeta") || path.ends_with(".fschema") || path.ends_with(".ftable");
}
std::string_view bundledJsonSchemaCoverage(std::string_view) {
    return "Bundled structural properties, required keys, types, enums and minima. Unknown properties allowed; no "
           "remote schemas or engine semantic validation.";
}
void JsonSchemaService::updateDocument(std::string path, text::Snapshot snapshot) {
    LockGuard lock(m_mutex);
    if (m_documents.size() >= 256 && !m_documents.contains(documentKey(path)))
        m_documents.erase(m_documents.begin());
    m_documents.insert_or_assign(documentKey(path), std::move(snapshot));
}
void JsonSchemaService::removeDocument(std::string_view path) {
    LockGuard lock(m_mutex);
    m_documents.erase(documentKey(path));
}
std::optional<ServerCapabilities> JsonSchemaService::serverCapabilities() const {
    ServerCapabilities capabilities;
    capabilities.completion = true;
    capabilities.hover = true;
    capabilities.completionTriggerCharacters = {"\"", ":"};
    return capabilities;
}
std::vector<LiveDiagnostic> JsonSchemaService::diagnostics(std::string_view path) const {
    text::Snapshot snapshot;
    {
        LockGuard lock(m_mutex);
        const auto found = m_documents.find(documentKey(path));
        if (found == m_documents.end())
            return {};
        snapshot = found->second;
    }
    if (!hasBundledJsonSchema(path))
        return {};
    const auto source = normalized(path, snapshot);
    auto parsed = source ? rpc::parseJson(*source) : Result<Json>(std::unexpected(source.error()));
    std::vector<std::pair<std::vector<std::string>, std::string>> errors;
    Scanner scan{source ? std::string_view(*source) : std::string_view{}};
    scan.value();
    if (!parsed)
        errors.push_back({{}, parsed.error().message()});
    else
        validate(schemaFor(path), *parsed, {}, errors);
    std::vector<LiveDiagnostic> diagnostics;
    for (const auto& [nodePath, message] : errors) {
        auto node = std::ranges::find(scan.nodes, nodePath, &Node::path);
        auto begin = node == scan.nodes.end() ? 0 : node->begin;
        auto end = node == scan.nodes.end() ? std::min<std::size_t>(1, snapshot.size().value()) : node->end;
        if (!parsed) {
            constexpr std::string_view marker = "at byte ";
            const auto found = message.find(marker);
            std::size_t byte = 0;
            if (found != std::string::npos) {
                const auto digits = message.data() + found + marker.size();
                const auto converted = std::from_chars(digits, message.data() + message.size(), byte);
                if (converted.ec == std::errc{})
                    begin = std::min<std::size_t>(byte, snapshot.size().value());
            }
            while (begin && !snapshot.isCharBoundary(ByteOffset{begin}))
                --begin;
            end = std::min<std::size_t>(begin + 1, snapshot.size().value());
            while (end < snapshot.size().value() && !snapshot.isCharBoundary(ByteOffset{end}))
                ++end;
        }
        LiveDiagnostic diagnostic;
        diagnostic.file = path;
        diagnostic.source = "frost-json-schema";
        diagnostic.code = parsed ? "schema" : "syntax";
        diagnostic.message = message;
        diagnostic.range = text::ByteRange{ByteOffset{begin}, ByteOffset{end}};
        diagnostic.textVersion = snapshot.version();
        diagnostic.lspRange = {toLspPosition(snapshot, ByteOffset{begin}, PositionEncoding::Utf16),
                               toLspPosition(snapshot, ByteOffset{end}, PositionEncoding::Utf16)};
        diagnostic.line = static_cast<int>(diagnostic.lspRange.start.line) + 1;
        diagnostic.column = static_cast<int>(diagnostic.lspRange.start.character) + 1;
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}
sched::Task<Result<Json>> JsonSchemaService::request(std::string method, Json params, std::stop_token token) {
    if (token.stop_requested())
        co_return fail(ErrorCode::Cancelled);
    const auto* document = params.find("textDocument");
    auto path = uriToPath(document ? document->stringAt("uri") : std::string_view{});
    if (!path || !hasBundledJsonSchema(*path))
        co_return fail(ErrorCode::Unsupported, "No bundled JSON schema for this document");
    text::Snapshot snapshot;
    {
        LockGuard lock(m_mutex);
        const auto found = m_documents.find(documentKey(*path));
        if (found == m_documents.end())
            co_return fail(ErrorCode::Busy, "No captured JSON document");
        snapshot = found->second;
    }
    auto source = normalized(*path, snapshot);
    if (!source)
        co_return std::unexpected(source.error());
    const auto* position = params.find("position");
    const auto point = position ? parsePosition(*position) : std::nullopt;
    if (!point)
        co_return fail(ErrorCode::InvalidArgument, "A JSON request needs an exact LSP position");
    const auto byteOffset = fromLspPosition(snapshot, *point, PositionEncoding::Utf16);
    if (!byteOffset)
        co_return std::unexpected(byteOffset.error());
    if (toLspPosition(snapshot, *byteOffset, PositionEncoding::Utf16) != *point)
        co_return fail(ErrorCode::OutOfRange, "A JSON request position cannot be clamped into another token");
    const auto offset = byteOffset->value();
    Scanner scan{*source};
    scan.value();
    const Node* selected = nullptr;
    for (const auto& node : scan.nodes)
        if ((node.begin <= offset && offset <= node.end) ||
            (node.keyBegin <= offset && offset <= node.keyEnd && node.keyEnd))
            if (!selected || node.path.size() >= selected->path.size())
                selected = &node;
    const auto& root = schemaFor(*path);
    if (method == "textDocument/hover") {
        const auto* schema = selected ? fieldSchema(root, selected->path) : &root;
        if (!schema)
            co_return Json{};
        auto description = std::string(schema->stringAt("description", "Bundled structural schema property."));
        if (const auto* type = schema->find("type"))
            description += "\nType: " + type->dump();
        if (const auto* choices = schema->find("enum"))
            description += "\nValues: " + choices->dump();
        description += "\n";
        description += bundledJsonSchemaCoverage(*path);
        co_return Json::object().set("contents", Json::object().set("kind", "plaintext").set("value", description));
    }
    if (method != "textDocument/completion")
        co_return fail(ErrorCode::Unsupported, "JSON profile supports completion and hover");
    auto list = Json::array();
    if (!selected)
        co_return list;
    const bool inKey = selected->keyEnd && offset <= selected->keyEnd;
    auto context = selected->path;
    const bool objectKeys = selected->object || inKey;
    if (inKey && !context.empty())
        context.pop_back();
    const auto* schema = fieldSchema(root, context);
    if (!schema)
        co_return list;
    const auto range = [&](std::size_t begin, std::size_t end) {
        return toJson(LspRange{toLspPosition(snapshot, ByteOffset{begin}, PositionEncoding::Utf16),
                               toLspPosition(snapshot, ByteOffset{end}, PositionEncoding::Utf16)});
    };
    if (objectKeys) {
        const auto* properties = schema->find("properties");
        if (!properties)
            co_return list;
        for (std::size_t i = 0; i < properties->size(); ++i) {
            const auto& name = properties->keys()[i];
            const auto literal = Json(name).dump();
            const auto insertion = literal + (inKey && selected->begin > selected->keyEnd ? "" : ": ");
            list.push(Json::object()
                          .set("label", name)
                          .set("kind", 10)
                          .set("detail", properties->items()[i].stringAt("description", "Bundled structural property"))
                          .set("textEdit", Json::object()
                                               .set("range", range(inKey ? selected->keyBegin : offset,
                                                                   inKey ? selected->keyEnd : offset))
                                               .set("newText", insertion)));
        }
    } else {
        const auto* choices = schema->find("enum");
        Json values = choices                                 ? *choices
                      : schema->stringAt("type") == "boolean" ? Json::array({true, false})
                                                              : Json::array();
        for (const auto& choice : values.items())
            list.push(Json::object()
                          .set("label", choice.dump())
                          .set("kind", 20)
                          .set("textEdit", Json::object()
                                               .set("range", range(selected->begin, selected->end))
                                               .set("newText", choice.dump())));
    }
    co_return list;
}
} // namespace frost::lang
