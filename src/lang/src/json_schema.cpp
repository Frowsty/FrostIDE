// SPDX-License-Identifier: Apache-2.0
// Provides schema-backed JSON completion, validation, and entity structure.
#include "frost/lang/json_schema.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/json_entity_hierarchy.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <set>
namespace frost::lang {
namespace {
// Bundled JSON Schema structural profiles. Sources: CMake cmake-presets(7), LuaLS
// settings (runtime/workspace/diagnostics/completion), and FrostEngine project/data formats.
// These intentionally permit unmodeled properties. They do not claim full preset semantics,
// dynamic engine component schemas, reference integrity, or arbitrary remote JSON Schema support.
const Json& schemaFor(std::string_view path) {
    // The immutable bundled profiles are shared for the lifetime of the process.
    static const Json* const cmake =
        new Json(rpc::parseJson(R"schema({"type":"object","required":["version"],"properties":{
      "$schema":{"type":"string","description":"Schema URI; offline profile remains bundled."},
      "version":{"type":"integer","minimum":1,"description":"CMake preset format version."},
      "cmakeMinimumRequired":{"type":"object","properties":{"major":{"type":"integer"},"minor":{"type":"integer"},"patch":{"type":"integer"}}},
      "include":{"type":"array","items":{"type":"string"}},"vendor":{"type":"object"},
      "configurePresets":{"type":"array","items":{"type":"object","required":["name"],"properties":{
        "name":{"type":"string"},"displayName":{"type":"string"},"description":{"type":"string"},"hidden":{"type":"boolean"},
        "inherits":{"type":["string","array"]},"generator":{"type":"string"},"binaryDir":{"type":"string"},"installDir":{"type":"string"},
        "toolchainFile":{"type":"string"},"cacheVariables":{"type":"object"},"environment":{"type":"object"},"condition":{"type":["object","boolean","null"]}}}},
      "buildPresets":{"type":"array","items":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"configurePreset":{"type":"string"},"configuration":{"type":"string"},"jobs":{"type":"integer","minimum":1},"targets":{"type":["string","array"]},"cleanFirst":{"type":"boolean"}}}},
      "testPresets":{"type":"array","items":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"configurePreset":{"type":"string"},"configuration":{"type":"string"}}}},
      "packagePresets":{"type":"array","items":{"type":"object"}},"workflowPresets":{"type":"array","items":{"type":"object"}}
    }})schema")
                     .value());
    static const Json* const lua = new Json(rpc::parseJson(R"schema({"type":"object","properties":{
      "$schema":{"type":"string"},"runtime.version":{"type":"string","enum":["Lua 5.1","Lua 5.2","Lua 5.3","Lua 5.4","LuaJIT"],"description":"Lua runtime used by LuaLS."},
      "workspace.library":{"type":"array","items":{"type":"string"}},"workspace.checkThirdParty":{"type":["boolean","string"]},
      "diagnostics.enable":{"type":"boolean"},"diagnostics.globals":{"type":"array","items":{"type":"string"}},"diagnostics.disable":{"type":"array","items":{"type":"string"}},
      "completion.enable":{"type":"boolean"},"hint.enable":{"type":"boolean"},"format.enable":{"type":"boolean"},
      "runtime":{"type":"object","properties":{"version":{"type":"string","enum":["Lua 5.1","Lua 5.2","Lua 5.3","Lua 5.4","LuaJIT"]}}},
      "workspace":{"type":"object","properties":{"library":{"type":"array","items":{"type":"string"}},"checkThirdParty":{"type":["boolean","string"]}}},
      "diagnostics":{"type":"object","properties":{"enable":{"type":"boolean"},"globals":{"type":"array","items":{"type":"string"}},"disable":{"type":"array","items":{"type":"string"}}}}
    }})schema")
                                                .value());
    static const Json* const project = new Json(rpc::parseJson(R"schema({"type":"object","properties":{
      "General":{"type":"object","description":"Project identity and startup scene.","properties":{"Name":{"type":"string"},"Company":{"type":"string"},"Version":{"type":"string"},"StartScene":{"type":"integer","minimum":0}}},
      "Build":{"type":"object","properties":{"GameName":{"type":"string"},"OutputDirectory":{"type":"string"},"WindowTitle":{"type":"string"},"WindowWidth":{"type":"integer","minimum":1},"WindowHeight":{"type":"integer","minimum":1},"Fullscreen":{"type":"boolean"},"Resizable":{"type":"boolean"}}},
      "Input":{"type":"object","properties":{"Actions":{"type":"array","items":{"type":"object","properties":{"Name":{"type":"string"}}}}}},
      "Audio":{"type":"object","properties":{"MasterVolume":{"type":"number","minimum":0},"Groups":{"type":"array","items":{"type":"object","properties":{"Name":{"type":"string"},"Volume":{"type":"number","minimum":0},"Muted":{"type":"boolean"}}}}}},
      "Physics":{"type":"object","properties":{"Layers":{"type":"array","items":{"type":"string"}}}},"Render":{"type":"object","properties":{"RenderLayers":{"type":"array","items":{"type":"string"}}}}
    }})schema")
                                                    .value());
    static const Json* const meta = new Json(
        rpc::parseJson(
            R"schema({"type":"object","properties":{"Handle":{"type":"integer","minimum":0,"x-engine-reference":"asset"},"Type":{"type":"string"},"Path":{"type":"string"},"ImporterVersion":{"type":"integer","minimum":0},"Settings":{"type":"object"}}})schema")
            .value());
    static const Json* const data = new Json(
        rpc::parseJson(
            R"schema({"type":"object","properties":{"Format":{"type":"string","enum":["FrostDataSchema"]},"Version":{"type":"integer","minimum":1},"Name":{"type":"string"},"Description":{"type":"string"},"Columns":{"type":"array","items":{"type":"object","required":["Name","Type"],"properties":{"Name":{"type":"string"},"Type":{"type":"string"},"Required":{"type":"boolean"},"Description":{"type":"string"},"Min":{"type":"number"},"Max":{"type":"number"},"MaxLength":{"type":"integer","minimum":0}}}}}})schema")
            .value());
    static const Json* const table = new Json(
        rpc::parseJson(
            R"schema({"type":"object","properties":{"Format":{"type":"string","enum":["FrostDataTable"]},"Version":{"type":"integer","minimum":1},"Schema":{"type":["string","object","integer"],"x-engine-reference":"asset","x-engine-assetTypes":["DataSchema"]},"Rows":{"type":"array","items":{"type":"object"}}}})schema")
            .value());
    const auto name = io::fileName(path);
    if (name == "CMakePresets.json" || name == "CMakeUserPresets.json")
        return *cmake;
    if (name == ".luarc.json" || name == ".luarc.jsonc")
        return *lua;
    if (path.ends_with(".frostproj") || path.ends_with(".feproject"))
        return *project;
    if (path.ends_with(".fmeta"))
        return *meta;
    if (path.ends_with(".fschema"))
        return *data;
    return *table;
}
struct Node {
    std::vector<std::string> path;
    std::size_t begin = 0, end = 0, keyBegin = 0, keyEnd = 0;
    bool object = false, array = false;
};
struct Scanner {
    std::string_view source;
    std::size_t at = 0;
    std::vector<Node> nodes{};
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
        if (schema->find("items") && !component.empty() &&
            std::ranges::all_of(component, [](unsigned char c) { return c >= '0' && c <= '9'; }))
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
bool profileContains(std::string_view root, std::string_view path) {
    const auto key = documentKey(path);
    const auto normalized = documentKey(root);
    if (normalized.empty() || !key.starts_with(normalized) || key.size() <= normalized.size() ||
        (key[normalized.size()] != '/' && key[normalized.size()] != '\\'))
        return false;
    for (std::size_t start = 0; start <= key.size();) {
        const auto end = key.find_first_of("/\\", start);
        if (std::string_view(key).substr(start, end == key.npos ? end : end - start) == "..")
            return false;
        if (end == key.npos)
            break;
        start = end + 1;
    }
    return true;
}
bool safeIndexedPath(std::string_view path) {
    if (!path.starts_with("Assets/") || path.find_first_of("\\:\r\n") != path.npos)
        return false;
    for (std::size_t start = 0; start <= path.size();) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == path.npos ? end : end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (const char character : part) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 32 || byte == 127)
                return false;
        }
        if (end == path.npos)
            break;
        start = end + 1;
    }
    return true;
}
std::optional<std::uint64_t> exactJsonId(const Json& value) {
    if (value.kind() == Json::Kind::Integer)
        return value.asUnsigned();
    const auto* text = value.asString();
    if (!text || text->empty())
        return {};
    std::string_view digits = *text;
    int base = 10;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
        base = 16;
    }
    std::uint64_t id = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), id, base);
    return parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() ? std::optional(id) : std::nullopt;
}
struct CapturedJsonEntity {
    std::uint64_t id = 0;
    std::string name;
    std::size_t begin = 0, end = 0;
};
std::vector<CapturedJsonEntity> capturedEntities(const Scanner& scan, std::string_view source) {
    std::map<std::string, CapturedJsonEntity, std::less<>> rows;
    for (const auto& node : scan.nodes) {
        if (node.path.size() < 3 || node.path[0] != "Entities" || rows.size() > 10000)
            continue;
        const bool id = node.path.size() == 3 && node.path[2] == "ID";
        const bool name =
            node.path.size() == 5 && node.path[2] == "Components" && node.path[3] == "Tag" && node.path[4] == "Tag";
        if (!id && !name)
            continue;
        auto value = rpc::parseJson(source.substr(node.begin, node.end - node.begin));
        if (!value)
            continue;
        auto& row = rows[node.path[1]];
        if (id) {
            if (auto exact = exactJsonId(*value)) {
                row.id = *exact;
                row.begin = node.begin;
                row.end = node.end;
            }
        } else if (const auto* text = value->asString())
            row.name = *text;
    }
    std::vector<CapturedJsonEntity> entities;
    for (auto& [key, row] : rows)
        if (row.id)
            entities.push_back(std::move(row));
    return entities;
}
Result<Json> capturedEntityStructure(const text::Snapshot& snapshot, const Scanner& scan, std::string_view source) {
    auto parsed = rpc::parseJson(source);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto* entities = parsed->find("Entities");
    if (!entities)
        return Json::array();
    if (!entities->isArray())
        return fail(ErrorCode::Corrupt, "Captured scene Entities is not an array");
    if (entities->size() > 10000)
        return fail(ErrorCode::OutOfRange, "Captured scene structure exceeds 10000 entities");
    struct Ranges {
        const Node* whole = nullptr;
        const Node* id = nullptr;
        unsigned seen = 0;
    };
    std::map<std::string, Ranges, std::less<>> ranges;
    unsigned roots = 0;
    for (const auto& node : scan.nodes)
        if (!node.path.empty() && node.path[0] == "Entities") {
            if (node.path.size() == 1) {
                if (++roots > 1)
                    return fail(ErrorCode::Corrupt, "Scene structure has duplicate Entities members");
                continue;
            }
            auto& row = ranges[node.path[1]];
            unsigned bit = 0;
            if (node.path.size() == 2) {
                bit = 1;
                row.whole = &node;
            } else if (node.path.size() == 3 && node.path[2] == "ID") {
                bit = 2;
                row.id = &node;
            } else if (node.path.size() == 3 && node.path[2] == "Components")
                bit = 4;
            else if (node.path.size() == 4 && node.path[2] == "Components" && node.path[3] == "Tag")
                bit = 8;
            else if (node.path.size() == 4 && node.path[2] == "Components" && node.path[3] == "Relationship")
                bit = 16;
            else if (node.path.size() == 5 && node.path[2] == "Components" && node.path[3] == "Tag" &&
                     node.path[4] == "Tag")
                bit = 32;
            else if (node.path.size() == 5 && node.path[2] == "Components" && node.path[3] == "Relationship" &&
                     node.path[4] == "Parent")
                bit = 64;
            if (bit && (row.seen & bit))
                return fail(ErrorCode::Corrupt, "Scene structure has ambiguous duplicate entity members");
            row.seen |= bit;
        }
    std::vector<JsonEntityStructureNode> captured;
    captured.reserve(entities->size());
    for (std::size_t i = 0; i < entities->size(); ++i) {
        const auto& entity = entities->items()[i];
        const auto* value = entity.find("ID");
        const auto id = value ? exactJsonId(*value) : std::nullopt;
        const auto found = ranges.find(std::to_string(i));
        if (!id || found == ranges.end() || !found->second.whole || !found->second.id)
            return fail(ErrorCode::Corrupt, "Captured scene structure needs an exact entity ID and source token");
        const auto& location = found->second;
        JsonEntityStructureNode node;
        node.id = *id;
        if (const auto* components = entity.find("Components")) {
            if (const auto* tag = components->find("Tag"))
                node.name = std::string(tag->stringAt("Tag"));
            if (const auto* relationship = components->find("Relationship"))
                if (const auto* parent = relationship->find("Parent")) {
                    const auto exact = parent->isNull() ? std::optional<std::uint64_t>(0) : exactJsonId(*parent);
                    if (!exact)
                        return fail(ErrorCode::Corrupt, "Captured scene Parent is not an exact entity ID");
                    node.parent = *exact;
                }
        }
        node.range = {toLspPosition(snapshot, ByteOffset{location.whole->begin}, PositionEncoding::Utf16),
                      toLspPosition(snapshot, ByteOffset{location.whole->end}, PositionEncoding::Utf16)};
        node.selectionRange = {toLspPosition(snapshot, ByteOffset{location.id->begin}, PositionEncoding::Utf16),
                               toLspPosition(snapshot, ByteOffset{location.id->end}, PositionEncoding::Utf16)};
        captured.push_back(std::move(node));
    }
    return jsonEntityHierarchy(captured);
}
Result<Json> capturedJsonStructure(const text::Snapshot& snapshot, const Scanner& scan, std::string_view source) {
    auto parsed = rpc::parseJson(source);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (scan.nodes.size() > 10000)
        return fail(ErrorCode::OutOfRange, "JSON structure exceeds 10000 captured nodes");
    std::map<std::vector<std::string>, std::size_t> indices;
    std::vector<std::vector<std::size_t>> children(scan.nodes.size());
    for (std::size_t i = 0; i < scan.nodes.size(); ++i)
        if (!indices.emplace(scan.nodes[i].path, i).second)
            return fail(ErrorCode::Corrupt, "JSON structure contains ambiguous duplicate property paths");
    std::vector<std::size_t> roots;
    for (std::size_t i = 0; i < scan.nodes.size(); ++i)
        if (!scan.nodes[i].path.empty()) {
            auto parent = scan.nodes[i].path;
            parent.pop_back();
            const auto found = indices.find(parent);
            if (found == indices.end() || parent.empty())
                roots.push_back(i);
            else
                children[found->second].push_back(i);
        }
    const auto emit = [&](auto& self, std::size_t i) -> Json {
        const auto& node = scan.nodes[i];
        auto nested = Json::array();
        for (const auto child : children[i])
            nested.push(self(self, child));
        const auto parentPath = [&] {
            auto path = node.path;
            path.pop_back();
            return path;
        }();
        const auto parent = indices.find(parentPath);
        const bool arrayParent = parent != indices.end() && scan.nodes[parent->second].array;
        const auto name = arrayParent ? "[" + node.path.back() + "]" : node.path.back();
        const auto range = LspRange{
            toLspPosition(snapshot, ByteOffset{node.keyEnd ? node.keyBegin : node.begin}, PositionEncoding::Utf16),
            toLspPosition(snapshot, ByteOffset{node.end}, PositionEncoding::Utf16)};
        const auto selection =
            node.keyEnd ? LspRange{toLspPosition(snapshot, ByteOffset{node.keyBegin}, PositionEncoding::Utf16),
                                   toLspPosition(snapshot, ByteOffset{node.keyEnd}, PositionEncoding::Utf16)}
                        : range;
        return Json::object()
            .set("name", name)
            .set("kind", node.object  ? 19
                         : node.array ? 18
                                      : 7)
            .set("range", toJson(range))
            .set("selectionRange", toJson(selection))
            .set("children", nested);
    };
    auto result = Json::array();
    for (const auto root : roots)
        result.push(emit(emit, root));
    return result;
}
std::optional<Json> tableReference(const text::Snapshot& snapshot) {
    if (snapshot.size().value() > 256 * 1024)
        return {};
    const auto source = snapshot.text();
    Scanner scan{source};
    scan.value();
    for (const auto& node : scan.nodes)
        if (node.path == std::vector<std::string>{"Schema"}) {
            const auto parsed = rpc::parseJson(std::string_view(source).substr(node.begin, node.end - node.begin));
            if (parsed)
                return *parsed;
            return {};
        }
    return {};
}
void markDataDeclaration(Json& shape, const std::string& source, std::vector<std::string> path) {
    auto names = Json::array();
    for (const auto& name : path)
        names.push(name);
    shape.set("x-engine-declaration", source).set("x-engine-columnPath", names);
    if (auto* properties = shape.find("properties"))
        for (const auto& key : properties->keys()) {
            auto child = path;
            child.push_back(key);
            markDataDeclaration(*properties->find(key), source, std::move(child));
        }
    if (auto* items = shape.find("items"))
        markDataDeclaration(*items, source, path);
}
std::optional<text::ByteRange> dataColumnDeclaration(const text::Snapshot& snapshot, const Json& names) {
    if (snapshot.size().value() > 2 * 1024 * 1024 || !names.isArray() || !names.size())
        return {};
    const auto source = snapshot.text();
    auto document = rpc::parseJson(source);
    if (!document)
        return {};
    const Json* owner = &*document;
    std::vector<std::string> path;
    for (std::size_t depth = 0; depth < names.size(); ++depth) {
        const Json* columns = owner->find(depth ? "Fields" : "Columns");
        if (!columns && depth)
            if (const auto* element = owner->find("Element")) {
                owner = element;
                path.push_back("Element");
                columns = owner->find("Fields");
            }
        if (!columns || !columns->isArray())
            return {};
        path.push_back(depth ? "Fields" : "Columns");
        const Json* match = nullptr;
        std::size_t index = 0;
        for (std::size_t i = 0; i < columns->size(); ++i)
            if (columns->items()[i].stringAt("Name") == *names.items()[depth].asString()) {
                if (match)
                    return {};
                match = &columns->items()[i];
                index = i;
            }
        if (!match)
            return {};
        owner = match;
        path.push_back(std::to_string(index));
    }
    path.push_back("Name");
    Scanner scan{source};
    scan.value();
    const auto found = std::ranges::find(scan.nodes, path, &Node::path);
    return found == scan.nodes.end() ? std::nullopt
                                     : std::optional(text::ByteRange{ByteOffset{found->begin}, ByteOffset{found->end}});
}
Result<Json> dataColumnShape(const Json& definition, unsigned depth = 0) {
    if (!definition.isObject() || depth > 8)
        return fail(ErrorCode::Corrupt, "Malformed actual data column definition");
    const auto type = definition.stringAt("Type");
    auto shape = Json::object();
    if (type == "Bool")
        shape.set("type", "boolean");
    else if (type == "Int")
        shape.set("type", "integer");
    else if (type == "Float")
        shape.set("type", "number");
    else if (type == "String" || type == "Name" || type == "Enum")
        shape.set("type", "string");
    else if (type == "Ref")
        shape.set("type", Json::array({"string", "null"}));
    else if (type == "Asset") {
        shape.set("type", Json::array({"string", "object", "integer", "null"})).set("x-engine-reference", "asset");
        auto types = Json::array();
        if (const auto assetType = definition.stringAt("AssetType"); !assetType.empty())
            types.push(std::string(assetType));
        shape.set("x-engine-assetTypes", types);
    } else if (type == "Color" || type == "Vec2")
        shape.set("type", "array").set("items", Json::object().set("type", "number"));
    else if (type == "List") {
        const auto* element = definition.find("Element");
        if (!element)
            return fail(ErrorCode::Corrupt, "Actual list column omitted Element");
        auto child = dataColumnShape(*element, depth + 1);
        if (!child)
            return std::unexpected(child.error());
        shape.set("type", "array").set("items", *child);
    } else if (type == "Struct") {
        const auto* fields = definition.find("Fields");
        if (!fields || !fields->isArray() || fields->size() > 256)
            return fail(ErrorCode::Corrupt, "Actual struct column omitted bounded Fields");
        auto properties = Json::object();
        for (const auto& field : fields->items()) {
            const auto name = field.stringAt("Name");
            if (name.empty() || properties.find(name))
                return fail(ErrorCode::Corrupt, "Invalid actual struct field name");
            auto child = dataColumnShape(field, depth + 1);
            if (!child)
                return std::unexpected(child.error());
            properties.set(std::string(name), *child);
        }
        shape.set("type", "object").set("properties", properties);
    } else
        return fail(ErrorCode::Unsupported, "Actual data column type has no structural editing profile");
    if (type == "Enum") {
        const auto* values = definition.find("Values");
        if (!values || !values->isArray())
            return fail(ErrorCode::Corrupt, "Actual enum omitted Values");
        for (const auto& value : values->items())
            if (!value.isString())
                return fail(ErrorCode::Corrupt, "Actual enum values must be strings");
        shape.set("enum", *values);
    }
    if (const auto* minimum = definition.find("Min"))
        shape.set("minimum", *minimum);
    if (const auto* maximum = definition.find("Max"))
        shape.set("maximum", *maximum);
    if (const auto* description = definition.find("Description"))
        shape.set("description", *description);
    return shape;
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
// Dynamic selected-engine schemas are untrusted protocol data. Validate every consumed
// keyword before publishing the complete replacement; unknown keywords stay inert.
bool admissibleEngineShape(const Json& schema, unsigned depth, std::size_t& nodes) {
    if (!schema.isObject() || depth > 64 || ++nodes > 10000)
        return false;
    const auto typeName = [](const Json& type) {
        const auto name = type.asString();
        return name && (*name == "object" || *name == "array" || *name == "string" || *name == "boolean" ||
                        *name == "integer" || *name == "number" || *name == "null");
    };
    if (const auto* type = schema.find("type")) {
        if (type->isArray()) {
            if (type->size() == 0)
                return false;
            for (const auto& value : type->items())
                if (!typeName(value))
                    return false;
        } else if (!typeName(*type))
            return false;
    }
    if (const auto* minimum = schema.find("minimum"); minimum && !minimum->isNumber())
        return false;
    if (const auto* maximum = schema.find("maximum"); maximum && !maximum->isNumber())
        return false;
    if (const auto* choices = schema.find("enum"); choices && !choices->isArray())
        return false;
    for (const auto* keyword : {"x-engine-enumNames", "x-engine-assetTypes"})
        if (const auto* values = schema.find(keyword)) {
            if (!values->isArray())
                return false;
            for (const auto& value : values->items())
                if (!value.isString())
                    return false;
        }
    if (const auto* reference = schema.find("x-engine-reference");
        reference &&
        (!reference->isString() || (*reference->asString() != "asset" && *reference->asString() != "entity")))
        return false;
    if (const auto* declaration = schema.find("x-engine-declaration");
        declaration && (!declaration->isString() || !safeIndexedPath(*declaration->asString()) ||
                        !declaration->asString()->ends_with(".fschema")))
        return false;
    if (const auto* path = schema.find("x-engine-columnPath")) {
        if (!path->isArray() || !path->size() || path->size() > 8)
            return false;
        for (const auto& name : path->items())
            if (!name.isString() || name.asString()->empty() || name.asString()->size() > 64)
                return false;
    }
    if (const auto* description = schema.find("description"); description && !description->isString())
        return false;
    if (const auto* required = schema.find("required")) {
        if (!required->isArray())
            return false;
        for (const auto& key : required->items())
            if (!key.isString())
                return false;
    }
    if (const auto* properties = schema.find("properties")) {
        if (!properties->isObject())
            return false;
        for (const auto& property : properties->items())
            if (!admissibleEngineShape(property, depth + 1, nodes))
                return false;
    }
    if (const auto* items = schema.find("items"); items && !admissibleEngineShape(*items, depth + 1, nodes))
        return false;
    return true;
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
    if (const auto* maximum = schema.find("maximum");
        maximum && value.isNumber() && *value.asDouble() > *maximum->asDouble())
        errors.push_back({path, "Value is above the schema maximum " + maximum->dump()});
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
Result<void> JsonSchemaService::installEngineSchemas(std::string projectRoot, const Json& actualExport) {
    if (!io::isAbsolutePath(projectRoot) || actualExport.stringAt("protocol") != "frostengine.document-schemas" ||
        actualExport.integerAt("version") != 1)
        return fail(ErrorCode::Corrupt, "Unsupported engine document schema contract");
    const auto* entries = actualExport.find("schemas");
    if (!entries || !entries->isArray() || entries->size() > 64 || actualExport.dump().size() > 2 * 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "Engine document schemas exceed their bounded structural profile");
    std::map<std::string, Json, std::less<>> schemas;
    std::size_t shapeNodes = 0;
    for (const auto& entry : entries->items()) {
        const auto extension = entry.stringAt("extension");
        const auto* schema = entry.find("schema");
        if (extension.size() < 2 || extension.size() > 32 || extension.front() != '.' || !schema ||
            !schema->isObject() || entry.stringAt("serializer").empty())
            return fail(ErrorCode::Corrupt, "Invalid engine document schema descriptor");
        if (!admissibleEngineShape(*schema, 0, shapeNodes))
            return fail(ErrorCode::Corrupt, "Malformed or oversized engine schema keyword shape");
        for (char c : extension.substr(1))
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
                return fail(ErrorCode::Corrupt, "Engine schema extension must be a literal lowercase suffix");
        if (!schemas.emplace(std::string(extension), *schema).second)
            return fail(ErrorCode::Corrupt, "Duplicate engine document schema suffix");
    }
    LockGuard lock(m_mutex);
    m_engineRoot = documentKey(projectRoot);
    m_engineTableSchemas.clear();
    m_engineSchemas = std::move(schemas);
    ++m_schemaGeneration;
    return {};
}
Result<void> JsonSchemaService::installEngineTableSchema(std::string path, const text::Snapshot& snapshot,
                                                         const Json& reply) {
    if (!path.ends_with(".ftable") || reply.stringAt("protocol") != "frostengine.table-schema" ||
        reply.integerAt("version") != 1 || reply.dump().size() > 2 * 1024 * 1024)
        return fail(ErrorCode::Corrupt, "Unsupported actual table-schema envelope");
    const auto reference = tableReference(snapshot);
    const auto* echoed = reply.find("reference");
    const auto* info = reply.find("schemaInfo");
    if (!reference || !echoed || *reference != *echoed || !info || !info->isObject() ||
        !info->stringAt("resolveError").empty())
        return fail(ErrorCode::Corrupt, "Actual schema.info differs from captured table reference or cannot resolve");
    const auto* columns = info->find("columns");
    if (!columns || !columns->isArray() || columns->size() > 256)
        return fail(ErrorCode::Corrupt, "Actual schema.info omitted bounded columns");
    auto properties = Json::object().set("Key", Json::object().set("type", "string"));
    for (const auto& column : columns->items()) {
        const auto name = column.stringAt("name");
        const auto* definition = column.find("definition");
        if (name.empty() || properties.find(name) || !definition || definition->stringAt("Name") != name ||
            definition->stringAt("Type") != column.stringAt("type"))
            return fail(ErrorCode::Corrupt, "Actual schema.info column identity is malformed");
        auto shape = dataColumnShape(*definition);
        if (!shape)
            return std::unexpected(shape.error());
        if (const auto* value = column.find("default"))
            shape->set("default", *value);
        std::string description(shape->stringAt("description"));
        description += "\nDeclared in: " + std::string(column.stringAt("declaredIn"));
        shape->set("description", description);
        const auto origin = column.stringAt("declaredIn");
        if (safeIndexedPath(origin) && origin.ends_with(".fschema"))
            markDataDeclaration(*shape, std::string(origin), {std::string(name)});
        properties.set(std::string(name), *shape);
    }
    auto schema = schemaFor(path);
    schema.find("properties")
        ->find("Rows")
        ->set("items", Json::object().set("type", "object").set("properties", properties));
    std::size_t nodes = 0;
    if (!admissibleEngineShape(schema, 0, nodes))
        return fail(ErrorCode::Corrupt, "Malformed actual data editing shape");
    LockGuard lock(m_mutex);
    const auto key = documentKey(path);
    if (m_engineRoot.empty() || !key.starts_with(m_engineRoot) || key.size() <= m_engineRoot.size() ||
        (key[m_engineRoot.size()] != '/' && key[m_engineRoot.size()] != '\\'))
        return fail(ErrorCode::Cancelled, "Selected engine project changed");
    if (m_engineTableSchemas.size() >= 64 && !m_engineTableSchemas.contains(key))
        m_engineTableSchemas.erase(m_engineTableSchemas.begin());
    m_engineTableSchemas.insert_or_assign(key, std::pair{*reference, std::move(schema)});
    ++m_schemaGeneration;
    return {};
}
void JsonSchemaService::setDeclarationReader(DeclarationReader reader) {
    LockGuard lock(m_mutex);
    m_declarationReader = std::move(reader);
}
void JsonSchemaService::setAssetReferences(std::string root, JsonAssetReferenceProvider provider) {
    LockGuard lock(m_mutex);
    if (!io::isAbsolutePath(root) || !provider.resolve || !provider.complete) {
        m_assetRoot.clear();
        m_assetReferences = {};
    } else {
        m_assetRoot = documentKey(root);
        m_assetReferences = std::move(provider);
    }
    ++m_schemaGeneration;
}
void JsonSchemaService::clearEngineSchemas() {
    LockGuard lock(m_mutex);
    m_engineRoot.clear();
    m_engineSchemas.clear();
    m_engineTableSchemas.clear();
    ++m_schemaGeneration;
}
std::uint64_t JsonSchemaService::schemaGeneration() const {
    LockGuard lock(m_mutex);
    return m_schemaGeneration;
}
std::optional<Json> JsonSchemaService::engineSchemaLocked(std::string_view path) const {
    const auto key = documentKey(path);
    // POSIX normalization preserves .. across potential symlinks; profiles refuse traversal.
    for (std::size_t start = 0; start <= key.size();) {
        const auto end = key.find_first_of("/\\", start);
        if (std::string_view(key).substr(start, end == key.npos ? end : end - start) == "..")
            return {};
        if (end == key.npos)
            break;
        start = end + 1;
    }
    if (m_engineRoot.empty() || !key.starts_with(m_engineRoot) || key.size() <= m_engineRoot.size() ||
        (key[m_engineRoot.size()] != '/' && key[m_engineRoot.size()] != '\\'))
        return {};
    if (const auto installed = m_engineTableSchemas.find(key); installed != m_engineTableSchemas.end())
        if (const auto document = m_documents.find(key); document != m_documents.end())
            if (const auto reference = tableReference(document->second);
                reference && *reference == installed->second.first)
                return installed->second.second;
    for (const auto& [extension, schema] : m_engineSchemas)
        if (key.ends_with(extension))
            return schema;
    return {};
}
bool JsonSchemaService::handles(std::string_view path) const {
    if (hasBundledJsonSchema(path))
        return true;
    LockGuard lock(m_mutex);
    return engineSchemaLocked(path).has_value();
}
void JsonSchemaService::removeDocument(std::string_view path) {
    LockGuard lock(m_mutex);
    m_engineTableSchemas.erase(documentKey(path));
    m_documents.erase(documentKey(path));
}
std::optional<ServerCapabilities> JsonSchemaService::serverCapabilities() const {
    ServerCapabilities capabilities;
    capabilities.completion = true;
    capabilities.hover = true;
    capabilities.definition = true;
    capabilities.documentSymbol = true;
    capabilities.completionTriggerCharacters = {"\"", ":"};
    return capabilities;
}
std::vector<LiveDiagnostic> JsonSchemaService::diagnostics(std::string_view path) const {
    text::Snapshot snapshot;
    Json selectedSchema;
    bool engine = false;
    {
        LockGuard lock(m_mutex);
        const auto found = m_documents.find(documentKey(path));
        if (found == m_documents.end())
            return {};
        snapshot = found->second;
        if (auto schema = engineSchemaLocked(path)) {
            selectedSchema = std::move(*schema);
            engine = true;
        } else if (hasBundledJsonSchema(path))
            selectedSchema = schemaFor(path);
        else
            return {};
    }
    const auto source = normalized(path, snapshot);
    auto parsed = source ? rpc::parseJson(*source) : Result<Json>(std::unexpected(source.error()));
    std::vector<std::pair<std::vector<std::string>, std::string>> errors;
    Scanner scan{source ? std::string_view(*source) : std::string_view{}};
    scan.value();
    if (!parsed)
        errors.push_back({{}, parsed.error().message()});
    else
        validate(selectedSchema, *parsed, {}, errors);
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
        diagnostic.source = engine ? "frost-engine-schema-shape" : "frost-json-schema";
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
    if (!path || !handles(*path))
        co_return fail(ErrorCode::Unsupported, "No bundled JSON schema for this document");
    text::Snapshot snapshot;
    Json selectedSchema;
    bool engine = false;
    std::string assetRoot, engineRoot;
    JsonAssetReferenceProvider assets;
    DeclarationReader reader;
    std::uint64_t generation = 0;
    {
        LockGuard lock(m_mutex);
        const auto found = m_documents.find(documentKey(*path));
        if (found == m_documents.end())
            co_return fail(ErrorCode::Busy, "No captured JSON document");
        snapshot = found->second;
        assetRoot = m_assetRoot;
        assets = m_assetReferences;
        engineRoot = m_engineRoot;
        reader = m_declarationReader;
        generation = m_schemaGeneration;
        if (auto schema = engineSchemaLocked(*path)) {
            selectedSchema = std::move(*schema);
            engine = true;
        } else if (hasBundledJsonSchema(*path))
            selectedSchema = schemaFor(*path);
        else
            co_return fail(ErrorCode::Busy, "Engine document schemas were withdrawn");
    }
    auto source = normalized(*path, snapshot);
    if (!source)
        co_return std::unexpected(source.error());
    Scanner scan{*source};
    scan.value();
    if (method == "textDocument/documentSymbol") {
        if (path->ends_with(".fscene"))
            co_return capturedEntityStructure(snapshot, scan, *source);
        co_return capturedJsonStructure(snapshot, scan, *source);
    }
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
    const Node* selected = nullptr;
    for (const auto& node : scan.nodes)
        if ((node.begin <= offset && offset <= node.end) ||
            (node.keyBegin <= offset && offset <= node.keyEnd && node.keyEnd))
            if (!selected || node.path.size() >= selected->path.size())
                selected = &node;
    const Json* referenceShape = nullptr;
    const Node* referenceNode = nullptr;
    const bool selectedKey = selected && selected->keyEnd && selected->keyBegin <= offset && offset <= selected->keyEnd;
    if (selected && !selectedKey) {
        auto context = selected->path;
        for (;;) {
            if (const auto* shape = fieldSchema(selectedSchema, context);
                shape && !shape->stringAt("x-engine-reference").empty()) {
                const auto node = std::ranges::find(scan.nodes, context, &Node::path);
                if (node != scan.nodes.end()) {
                    referenceShape = shape;
                    referenceNode = &*node;
                }
                break;
            }
            if (context.empty())
                break;
            context.pop_back();
        }
    }
    auto referenceValue = referenceNode ? rpc::parseJson(std::string_view(*source).substr(
                                              referenceNode->begin, referenceNode->end - referenceNode->begin))
                                        : Result<Json>(Json{});
    if (!referenceValue && selected)
        referenceValue =
            rpc::parseJson(std::string_view(*source).substr(selected->begin, selected->end - selected->begin));
    std::optional<JsonIndexedAsset> indexed;
    if (referenceShape && referenceShape->stringAt("x-engine-reference") == "asset" && assets.resolve &&
        profileContains(assetRoot, *path) && referenceValue) {
        indexed = assets.resolve(*referenceValue);
        if (indexed && (!indexed->handle || !safeIndexedPath(indexed->path)))
            indexed.reset();
    }
    const auto entities = referenceShape && referenceShape->stringAt("x-engine-reference") == "entity"
                              ? capturedEntities(scan, *source)
                              : std::vector<CapturedJsonEntity>{};
    if (method == "textDocument/definition") {
        auto locations = Json::array();
        if (indexed) {
            const auto target = io::joinPath(assetRoot, indexed->path);
            const auto uri = target ? pathToUri(*target) : Result<std::string>(fail(ErrorCode::InvalidArgument));
            if (uri)
                locations.push(Json::object().set("uri", *uri).set("range", toJson(LspRange{})));
        } else if (referenceValue)
            if (const auto id = exactJsonId(*referenceValue)) {
                const CapturedJsonEntity* match = nullptr;
                bool ambiguous = false;
                for (const auto& entity : entities)
                    if (entity.id == *id) {
                        ambiguous = match != nullptr;
                        match = &entity;
                        if (ambiguous)
                            break;
                    }
                if (match && !ambiguous)
                    locations.push(Json::object()
                                       .set("uri", pathToUri(*path).value())
                                       .set("range", toJson(LspRange{toLspPosition(snapshot, ByteOffset{match->begin},
                                                                                   PositionEncoding::Utf16),
                                                                     toLspPosition(snapshot, ByteOffset{match->end},
                                                                                   PositionEncoding::Utf16)})));
            }
        if (!locations.size() && selected && !engineRoot.empty()) {
            auto context = selected->path;
            const Json* declaration = nullptr;
            for (;;) {
                const auto* shape = fieldSchema(selectedSchema, context);
                if (shape && shape->find("x-engine-declaration") && shape->find("x-engine-columnPath")) {
                    declaration = shape;
                    break;
                }
                if (context.empty())
                    break;
                context.pop_back();
            }
            if (declaration) {
                const auto target = io::joinPath(engineRoot, declaration->stringAt("x-engine-declaration"));
                if (target) {
                    std::optional<text::Snapshot> captured;
                    {
                        LockGuard lock(m_mutex);
                        if (m_engineRoot != engineRoot || m_schemaGeneration != generation)
                            co_return fail(ErrorCode::Cancelled, "Selected engine schema changed");
                        if (const auto open = m_documents.find(documentKey(*target)); open != m_documents.end())
                            captured = open->second;
                    }
                    if (!captured && reader) {
                        auto loaded = reader(engineRoot, *target);
                        if (!loaded)
                            co_return std::unexpected(loaded.error());
                        captured = *loaded;
                    }
                    if (captured)
                        if (const auto bytes =
                                dataColumnDeclaration(*captured, *declaration->find("x-engine-columnPath"))) {
                            const auto uri = pathToUri(*target);
                            if (uri)
                                locations.push(
                                    Json::object()
                                        .set("uri", *uri)
                                        .set("range",
                                             toJson(LspRange{
                                                 toLspPosition(*captured, bytes->begin, PositionEncoding::Utf16),
                                                 toLspPosition(*captured, bytes->end, PositionEncoding::Utf16)})));
                        }
                    {
                        LockGuard lock(m_mutex);
                        if (m_engineRoot != engineRoot || m_schemaGeneration != generation)
                            co_return fail(ErrorCode::Cancelled,
                                           "Selected engine schema changed while reading its declaration");
                    }
                }
            }
        }
        co_return locations;
    }
    const auto& root = selectedSchema;
    if (method == "textDocument/hover") {
        const auto* schema = selected ? fieldSchema(root, selected->path) : &root;
        if (!schema && referenceShape)
            schema = referenceShape;
        if (!schema)
            co_return Json{};
        auto description = std::string(schema->stringAt("description", "Bundled structural schema property."));
        if (const auto* type = schema->find("type"))
            description += "\nType: " + type->dump();
        if (const auto* choices = schema->find("enum"))
            description += "\nValues: " + choices->dump();
        if (const auto* value = schema->find("default"))
            description += "\nDefault: " + value->dump();
        if (const auto* minimum = schema->find("minimum"))
            description += "\nMinimum: " + minimum->dump();
        if (const auto* maximum = schema->find("maximum"))
            description += "\nMaximum: " + maximum->dump();
        if (indexed)
            description += "\nActual engine index snapshot asset: " + indexed->path + " (" + indexed->type + ", " +
                           std::to_string(indexed->handle) + ")";
        description += "\n";
        description += engine ? "Actual engine serializer/default field shapes; semantic validation requires the "
                                "selected engine's document.validate."
                              : bundledJsonSchemaCoverage(*path);
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
    if (!schema && !(referenceShape && !objectKeys))
        co_return list;
    const auto range = [&](std::size_t begin, std::size_t end) {
        return toJson(LspRange{toLspPosition(snapshot, ByteOffset{begin}, PositionEncoding::Utf16),
                               toLspPosition(snapshot, ByteOffset{end}, PositionEncoding::Utf16)});
    };
    if (!objectKeys && referenceShape && selected) {
        const auto editRange = range(selected->begin, selected->end);
        auto references = Json::array();
        bool incomplete = false;
        if (referenceShape->stringAt("x-engine-reference") == "asset" && assets.complete &&
            profileContains(assetRoot, *path)) {
            std::vector<std::string> types;
            if (const auto* accepted = referenceShape->find("x-engine-assetTypes"))
                for (const auto& type : accepted->items())
                    types.push_back(*type.asString());
            std::string prefix;
            auto literal =
                rpc::parseJson(std::string_view(*source).substr(selected->begin, selected->end - selected->begin));
            if (literal && literal->asString() && !exactJsonId(*literal))
                prefix = *literal->asString();
            auto actual = assets.complete(types, prefix, 5001);
            incomplete = actual.size() > 5000;
            if (incomplete)
                actual.resize(5000);
            for (const auto& asset : actual)
                if (asset.handle && safeIndexedPath(asset.path) &&
                    (types.empty() || std::ranges::find(types, asset.type) != types.end())) {
                    const bool handle = (!selected->path.empty() && selected->path.back() == "Handle") ||
                                        (referenceValue && referenceValue->kind() == Json::Kind::Integer);
                    auto item = Json::object()
                                    .set("label", asset.path)
                                    .set("kind", 17)
                                    .set("detail",
                                         asset.type + " — " + std::to_string(asset.handle) + " (engine index snapshot)")
                                    .set("textEdit", Json::object()
                                                         .set("range", editRange)
                                                         .set("newText", handle ? Json(asset.handle).dump()
                                                                                : Json(asset.path).dump()));
                    // Asset handles are authoritative over path hints. A completion inside an
                    // object must update both members in the same completion Undo operation.
                    auto additional = Json::array();
                    if (referenceNode && referenceNode->object)
                        for (const auto& sibling : scan.nodes) {
                            if (sibling.path.size() != referenceNode->path.size() + 1 ||
                                !std::equal(referenceNode->path.begin(), referenceNode->path.end(),
                                            sibling.path.begin()) ||
                                (sibling.path.back() != "Handle" && sibling.path.back() != "Path") ||
                                (sibling.begin == selected->begin && sibling.end == selected->end))
                                continue;
                            additional.push(Json::object()
                                                .set("range", range(sibling.begin, sibling.end))
                                                .set("newText", sibling.path.back() == "Handle"
                                                                    ? Json(asset.handle).dump()
                                                                    : Json(asset.path).dump()));
                        }
                    if (additional.size() > 128)
                        continue;
                    if (additional.size())
                        item.set("additionalTextEdits", additional);
                    references.push(std::move(item));
                }
        } else if (referenceShape->stringAt("x-engine-reference") == "entity") {
            std::set<std::uint64_t> seen;
            for (const auto& entity : entities)
                if (seen.insert(entity.id).second) {
                    if (references.size() == 5000) {
                        incomplete = true;
                        break;
                    }
                    references.push(
                        Json::object()
                            .set("label", entity.name.empty() ? std::to_string(entity.id)
                                                              : entity.name + " (" + std::to_string(entity.id) + ")")
                            .set("kind", 6)
                            .set("detail", "Actual entity in the captured document")
                            .set("textEdit",
                                 Json::object().set("range", editRange).set("newText", Json(entity.id).dump())));
                }
        }
        co_return Json::object().set("isIncomplete", incomplete).set("items", references);
    }
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
        if (!choices)
            choices = schema->find("x-engine-enumNames");
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
