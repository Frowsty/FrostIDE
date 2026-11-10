// SPDX-License-Identifier: Apache-2.0
//
// Engine-aware Lua (engine_lua.hpp, FEC-10b): LuaLS `---@meta` stub generation from
// `SchemaDocument`, component entry queries, C++ binding resolution (`source_location`), and
// `EngineLuaHostProvider`.

#include "frost/engine/engine_lua.hpp"

#include "frost/engine/engine_cli.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/frost_engine_lua.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <set>
#include <utility>

namespace frost::engine {
namespace {

constexpr std::array<std::string_view, 22> kLuaKeywords = {
    "and", "break", "do",  "else", "elseif", "end",    "false",  "for",  "function", "goto",  "if",
    "in",  "local", "nil", "not",  "or",     "repeat", "return", "then", "true",     "until", "while",
};

[[nodiscard]] bool isLuaKeyword(std::string_view word) noexcept { return std::ranges::contains(kLuaKeywords, word); }

[[nodiscard]] bool isIdentStart(char c) noexcept {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) != 0 || c == '_';
}

[[nodiscard]] bool isIdentChar(char c) noexcept {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_';
}

[[nodiscard]] std::string_view trimView(std::string_view text) noexcept {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string sanitizeIdentifier(std::string_view raw, std::string_view fallback = "arg") {
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (const char ch : raw) {
        if (isIdentChar(ch)) {
            cleaned.push_back(ch);
        }
    }
    if (cleaned.empty()) {
        cleaned = std::string(fallback);
    }
    if (!cleaned.empty() && std::isdigit(static_cast<unsigned char>(cleaned.front())) != 0) {
        cleaned.insert(cleaned.begin(), '_');
    }
    if (isLuaKeyword(cleaned)) {
        cleaned.push_back('_');
    }
    return cleaned;
}

[[nodiscard]] std::string normalizeAtomType(std::string_view rawAtom) {
    const std::string_view atom = trimView(rawAtom);
    if (atom.empty() || atom == "void" || atom == "nil" || atom == "none") {
        return {};
    }
    if (atom == "bool" || atom == "Bool" || atom == "boolean") {
        return "boolean";
    }
    if (atom == "int" || atom == "int8" || atom == "int16" || atom == "int32" || atom == "int64" || atom == "uint" ||
        atom == "uint8" || atom == "uint16" || atom == "uint32" || atom == "uint64" || atom == "size_t" ||
        atom == "Int" || atom == "UInt" || atom == "Int64" || atom == "UInt64" || atom == "integer") {
        return "integer";
    }
    if (atom == "float" || atom == "double" || atom == "Float" || atom == "Double" || atom == "number") {
        return "number";
    }
    if (atom == "string" || atom == "String" || atom == "Asset" || atom == "Prefab" || atom == "Sound" ||
        atom == "Texture" || atom == "Material" || atom == "Mesh" || atom == "Font") {
        return "string";
    }
    if (atom == "Vector2") {
        return "Vec2";
    }
    if (atom == "Vector3" || atom == "Color3") {
        return "Vec3";
    }
    if (atom == "Vector4" || atom == "Color") {
        return "Vec4";
    }
    if (atom == "Quat" || atom == "Quaternion") {
        return "Quat";
    }
    if (atom == "table" || atom == "Table") {
        return "table";
    }
    if (atom == "function" || atom == "Function" || atom == "Callback") {
        return "fun(...): any";
    }
    if (atom == "any" || atom == "...") {
        return "any";
    }
    if (atom.starts_with("array<") && atom.ends_with('>')) {
        const std::string inner = normalizeAtomType(atom.substr(6, atom.size() - 7));
        return (inner.empty() ? "any" : inner) + "[]";
    }
    if (atom.starts_with("table<") || atom.starts_with("fun(")) {
        return std::string(atom);
    }
    return sanitizeIdentifier(atom, "any");
}

[[nodiscard]] std::string normalizeLuaType(std::string_view rawType) {
    std::string_view trimmed = trimView(rawType);
    if (trimmed.empty()) {
        return "any";
    }
    bool nullable = false;
    if (trimmed.ends_with('?')) {
        nullable = true;
        trimmed.remove_suffix(1);
    }
    std::string result;
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t bar = trimmed.find('|', start);
        const std::string_view part =
            bar == std::string_view::npos ? trimmed.substr(start) : trimmed.substr(start, bar - start);
        const std::string norm = normalizeAtomType(part);
        if (!norm.empty()) {
            if (!result.empty()) {
                result += '|';
            }
            result += norm;
        }
        if (bar == std::string_view::npos) {
            break;
        }
        start = bar + 1;
    }
    if (result.empty()) {
        return {};
    }
    if (nullable && result.find("nil") == std::string::npos && !result.ends_with('?')) {
        result += '?';
    }
    return result;
}

void appendDocLines(std::string& out, std::string_view text) {
    const std::string_view trimmed = trimView(text);
    if (trimmed.empty()) {
        return;
    }
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t nl = trimmed.find('\n', start);
        std::string_view line =
            nl == std::string_view::npos ? trimmed.substr(start) : trimmed.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out += "--- ";
        out += line;
        out += '\n';
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
}

void appendMetadataDocs(std::string& out, const SchemaLuaEntry& entry) {
    appendDocLines(out, entry.description);
    if (!entry.cost.empty()) {
        out += "--- Cost: ";
        for (std::size_t i = 0; i < entry.cost.size(); ++i) {
            if (i > 0) {
                out += ", ";
            }
            out += entry.cost[i];
        }
        out += '\n';
    }
    if (!entry.binding.empty()) {
        out += std::format("--- Binding: {}\n", entry.binding);
    }
    if (entry.testModeOnly) {
        out += "--- Test mode only.\n";
    }
    if (!entry.sameAs.empty()) {
        out += std::format("--- Same as: {}\n", entry.sameAs);
    }
    if (entry.source) {
        out += std::format("--- Source: {}:{}\n", entry.source->file, entry.source->line);
    }
}

void appendSourceDoc(std::string& out, const std::optional<SchemaSource>& source) {
    if (source && !source->file.empty() && source->line > 0) {
        out += std::format("--- Source: {}:{}\n", source->file, source->line);
    }
}

[[nodiscard]] bool isComponentNameLookupMethod(std::string_view owner, std::string_view methodName) noexcept {
    if (owner != "Entity" && owner != "Scene" && !owner.empty()) {
        return false;
    }
    return methodName == "GetComponent" || methodName == "AddComponent" || methodName == "HasComponent" ||
           methodName == "RemoveComponent";
}

void appendCallableStub(std::string& out, std::string_view declarationName, std::string_view selfType,
                        std::span<const SchemaParameter> parameters, std::string_view returnType,
                        std::string_view returnDescription, std::string_view ownerName = {},
                        std::string_view rawMethodName = {},
                        std::span<const std::pair<std::string, std::string>> componentOverloads = {}) {
    std::vector<std::string> paramNames;
    std::set<std::string, std::less<>> usedNames;
    if (!selfType.empty()) {
        usedNames.insert("self");
    }
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const SchemaParameter& param = parameters[i];
        if (param.name == "..." || param.type == "...") {
            out += "---@param ... any";
            if (!param.description.empty()) {
                out += ' ';
                out += param.description;
            }
            out += '\n';
            paramNames.emplace_back("...");
            continue;
        }
        std::string pName = sanitizeIdentifier(param.name, std::format("arg{}", i + 1));
        while (usedNames.contains(pName)) {
            pName += '_';
        }
        usedNames.insert(pName);
        paramNames.push_back(pName);

        std::string pType = normalizeLuaType(param.type);
        if (pType.empty()) {
            pType = "any";
        }
        if (isComponentNameLookupMethod(ownerName, rawMethodName) &&
            (param.name == "name" || param.name == "componentName" || param.name == "type" || i == 0)) {
            pType = "ComponentName";
        }
        out += std::format("---@param {}{} {}", pName, param.optional ? "?" : "", pType);
        if (!param.description.empty()) {
            out += ' ';
            out += param.description;
        }
        out += '\n';
    }
    if ((rawMethodName == "GetComponent" || rawMethodName == "AddComponent") && !componentOverloads.empty() &&
        ownerName == "Entity") {
        for (const auto& [compLiteral, compClass] : componentOverloads) {
            out += std::format("---@overload fun(self: Entity, name: \"{}\"): {}\n", compLiteral, compClass);
        }
    }
    const std::string normReturn = normalizeLuaType(returnType);
    if (!normReturn.empty()) {
        out += std::format("---@return {}", normReturn);
        if (!returnDescription.empty()) {
            out += ' ';
            out += returnDescription;
        }
        out += '\n';
    }
    out += "function ";
    out += declarationName;
    out += '(';
    for (std::size_t i = 0; i < paramNames.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += paramNames[i];
    }
    out += ") end\n\n";
}

// Collects non-Component top-level symbols so component proxy classes never collide with a global
// namespace, type, enum, or library table of the same name (e.g. `Camera`, `Script`, `Animation`).
[[nodiscard]] std::set<std::string, std::less<>> nonComponentTopLevelSymbols(const SchemaDocument& schema) {
    std::set<std::string, std::less<>> names{"FrostScript", "Entity"};
    for (const SchemaLuaEntry& entry : schema.luaEntries) {
        if (entry.kind == "Namespace" || entry.kind == "Type" || entry.kind == "Enum" || entry.kind == "Library" ||
            entry.kind == "GlobalFunction" || entry.kind == "GlobalValue") {
            names.insert(sanitizeIdentifier(entry.name));
        } else if (!entry.owner.empty() && entry.scope != "Component" && entry.kind != "Component") {
            names.insert(sanitizeIdentifier(entry.owner));
        }
    }
    return names;
}

[[nodiscard]] std::string componentClassName(std::string_view componentName,
                                             const std::set<std::string, std::less<>>& reservedTopLevel) {
    const std::string ident = sanitizeIdentifier(componentName, "Component");
    if (reservedTopLevel.contains(ident)) {
        return ident + "Component";
    }
    return ident;
}

[[nodiscard]] bool matchesComponentOwner(const SchemaLuaEntry& entry, std::string_view compName,
                                         std::string_view sanitizedCompName) noexcept {
    if (entry.scope != "Component" && entry.owner != compName && entry.owner != sanitizedCompName) {
        return false;
    }
    return entry.owner == compName || entry.owner == sanitizedCompName ||
           sanitizeIdentifier(entry.owner) == sanitizedCompName;
}

struct ComponentSurface {
    std::string name;
    std::string sanitizedName;
    std::string className;
    SchemaSource source;
    struct PropertyItem {
        std::string name;
        std::string luaType;
        SchemaSource source;
        const SchemaLuaEntry* luaEntry = nullptr;
    };
    std::vector<PropertyItem> properties;
    std::vector<const SchemaLuaEntry*> methods;
};

[[nodiscard]] std::vector<ComponentSurface> buildComponentSurfaces(const SchemaDocument& schema) {
    const std::set<std::string, std::less<>> reserved = nonComponentTopLevelSymbols(schema);
    std::vector<ComponentSurface> out;
    out.reserve(schema.components.size());
    for (const SchemaComponent& comp : schema.components) {
        ComponentSurface surface;
        surface.name = comp.name;
        surface.sanitizedName = sanitizeIdentifier(comp.name, "Component");
        surface.className = componentClassName(comp.name, reserved);
        surface.source = comp.source;

        std::set<std::string, std::less<>> seenProps;
        for (const SchemaField& field : comp.fields) {
            ComponentSurface::PropertyItem item;
            item.name = field.name;
            item.luaType = normalizeLuaType(field.type);
            if (item.luaType.empty()) {
                item.luaType = "any";
            }
            item.source = field.source;
            for (const SchemaLuaEntry& entry : schema.luaEntries) {
                if (entry.kind == "Property" && entry.name == field.name &&
                    matchesComponentOwner(entry, comp.name, surface.sanitizedName)) {
                    item.luaEntry = &entry;
                    if (!entry.returnType.empty()) {
                        const std::string rt = normalizeLuaType(entry.returnType);
                        if (!rt.empty()) {
                            item.luaType = rt;
                        }
                    }
                    break;
                }
            }
            seenProps.insert(item.name);
            surface.properties.push_back(std::move(item));
        }
        for (const SchemaLuaEntry& entry : schema.luaEntries) {
            if (!matchesComponentOwner(entry, comp.name, surface.sanitizedName)) {
                continue;
            }
            if (entry.kind == "Property" && !seenProps.contains(entry.name)) {
                ComponentSurface::PropertyItem item;
                item.name = entry.name;
                item.luaType = normalizeLuaType(entry.returnType);
                if (item.luaType.empty()) {
                    item.luaType = "any";
                }
                if (entry.source) {
                    item.source = *entry.source;
                }
                item.luaEntry = &entry;
                seenProps.insert(item.name);
                surface.properties.push_back(std::move(item));
            } else if (entry.kind == "Method") {
                surface.methods.push_back(&entry);
            }
        }
        out.push_back(std::move(surface));
    }
    return out;
}

} // namespace

std::vector<std::string> schemaComponentEntryNames(const SchemaDocument& schema, std::string_view componentName) {
    const std::string sanitizedTarget = sanitizeIdentifier(componentName);
    const std::vector<ComponentSurface> surfaces = buildComponentSurfaces(schema);
    std::vector<std::string> names;
    for (const ComponentSurface& surface : surfaces) {
        if (surface.name == componentName || surface.sanitizedName == componentName ||
            surface.sanitizedName == sanitizedTarget || surface.className == componentName) {
            names.reserve(surface.properties.size() + surface.methods.size());
            for (const auto& prop : surface.properties) {
                names.push_back(prop.name);
            }
            for (const SchemaLuaEntry* method : surface.methods) {
                names.push_back(method->name);
            }
            return names;
        }
    }
    return names;
}

std::vector<std::string> schemaLuaGlobals(const SchemaDocument& schema) {
    std::vector<std::string> globals;
    std::set<std::string, std::less<>> seen;
    const auto add = [&](std::string_view raw) {
        if (raw.empty()) {
            return;
        }
        const std::string ident = sanitizeIdentifier(raw);
        if (ident.empty() || isLuaKeyword(ident)) {
            return;
        }
        if (seen.insert(ident).second) {
            globals.push_back(ident);
        }
    };

    for (const SchemaLuaEntry& entry : schema.luaEntries) {
        if (entry.binding == "LuaLibrary") {
            continue;
        }
        if (entry.kind == "Namespace" || entry.kind == "Type" || entry.kind == "Enum" || entry.kind == "Library" ||
            entry.kind == "GlobalFunction" || entry.kind == "GlobalValue") {
            add(entry.name);
        } else if (!entry.owner.empty() && entry.scope != "Component" && entry.kind != "Component") {
            add(entry.owner);
        }
    }
    add("FrostScript");
    for (const SchemaEvent& event : schema.events) {
        add(event.name);
    }
    return globals;
}

std::string generateEngineLuaStubs(const SchemaDocument& schema) {
    std::string out;
    out.reserve(64 * 1024);
    out += "---@meta frost_schema\n";
    out += "-- Generated by FrostIDE from FrostEngine schema.export (contract v2).\n";
    if (!schema.engineVersion.empty()) {
        out += std::format("-- Engine version: {} ({})\n", schema.engineVersion, schema.luaVersion);
    }
    out += '\n';

    // 1. Aliases (`ComponentName`, `FrostPropertyType`, ...).
    bool hasComponentNameAlias = false;
    for (const SchemaLuaAlias& alias : schema.luaAliases) {
        const std::string aliasName = sanitizeIdentifier(alias.name, "Alias");
        if (aliasName == "ComponentName") {
            hasComponentNameAlias = true;
        }
        if (alias.values.empty()) {
            out += std::format("---@alias {} string\n\n", aliasName);
            continue;
        }
        out += std::format("---@alias {}\n", aliasName);
        for (const std::string& val : alias.values) {
            out += std::format("---| '\"{}\"\'\n", val);
        }
        out += '\n';
    }
    if (!hasComponentNameAlias && !schema.components.empty()) {
        out += "---@alias ComponentName\n";
        for (const SchemaComponent& comp : schema.components) {
            out += std::format("---| '\"{}\"\'\n", comp.name);
        }
        out += '\n';
    }

    // 2. Component proxy classes (`schema.components` + `schema.luaEntries` with `scope == "Component"`).
    const std::vector<ComponentSurface> components = buildComponentSurfaces(schema);
    std::vector<std::pair<std::string, std::string>> componentOverloads;
    std::set<std::string, std::less<>> seenOverloads;
    for (const ComponentSurface& surface : components) {
        if (seenOverloads.insert(surface.name).second) {
            componentOverloads.emplace_back(surface.name, surface.className);
        }
        if (surface.sanitizedName != surface.name && seenOverloads.insert(surface.sanitizedName).second) {
            componentOverloads.emplace_back(surface.sanitizedName, surface.className);
        }
    }

    for (const ComponentSurface& surface : components) {
        if (!surface.methods.empty()) {
            out += "do\n";
        }
        out += std::format("--- Component `{}`.\n", surface.name);
        appendSourceDoc(out, surface.source);
        for (const auto& prop : surface.properties) {
            if (!prop.source.file.empty() && prop.source.line > 0) {
                out += std::format("--- Field {}: {}:{}\n", prop.name, prop.source.file, prop.source.line);
            }
        }
        out += std::format("---@class {}\n", surface.className);
        for (const auto& prop : surface.properties) {
            const std::string fieldName = sanitizeIdentifier(prop.name, "field");
            out += std::format("---@field {} {}", fieldName, prop.luaType);
            if (!prop.source.file.empty() && prop.source.line > 0) {
                out += std::format(" @Source: {}:{}", prop.source.file, prop.source.line);
            }
            out += '\n';
        }
        if (!surface.methods.empty()) {
            out += std::format("local {} = {{}}\n\n", surface.className);
            for (const SchemaLuaEntry* method : surface.methods) {
                appendMetadataDocs(out, *method);
                const std::string methodName = sanitizeIdentifier(method->name, "Method");
                appendCallableStub(out, std::format("{}:{}", surface.className, methodName), surface.className,
                                   method->parameters, method->returnType, method->returnDescription, surface.className,
                                   method->name);
            }
            out += "end\n";
        }
        if (surface.className != surface.sanitizedName + "Component") {
            out += std::format("---@alias {}Component {}\n", surface.sanitizedName, surface.className);
        }
        out += '\n';
    }

    // 3. Collect Types, Enums, Namespaces/Libraries, and Globals from `schema.luaEntries`.
    struct OwnerBucket {
        const SchemaLuaEntry* header = nullptr;
        std::vector<const SchemaLuaEntry*> properties;
        std::vector<const SchemaLuaEntry*> staticFunctions;
        std::vector<const SchemaLuaEntry*> instanceMethods;
    };
    std::vector<std::string> typeOrder;
    std::map<std::string, OwnerBucket, std::less<>> types;
    std::vector<std::string> enumOrder;
    std::map<std::string, OwnerBucket, std::less<>> enums;
    std::vector<std::string> namespaceOrder;
    std::map<std::string, OwnerBucket, std::less<>> namespaces;
    std::vector<const SchemaLuaEntry*> globalFunctions;
    std::vector<const SchemaLuaEntry*> globalValues;

    const auto ensureBucket = [](std::vector<std::string>& order, std::map<std::string, OwnerBucket, std::less<>>& map,
                                 std::string_view name) -> OwnerBucket& {
        auto it = map.find(name);
        if (it == map.end()) {
            order.emplace_back(name);
            it = map.emplace(std::string(name), OwnerBucket{}).first;
        }
        return it->second;
    };

    for (const SchemaLuaEntry& entry : schema.luaEntries) {
        if (entry.binding == "LuaLibrary" || entry.scope == "Component" || entry.kind == "Component" ||
            entry.kind == "Callback") {
            continue;
        }
        if (entry.kind == "Type") {
            ensureBucket(typeOrder, types, sanitizeIdentifier(entry.name)).header = &entry;
        } else if (entry.kind == "Enum") {
            ensureBucket(enumOrder, enums, sanitizeIdentifier(entry.name)).header = &entry;
        } else if (entry.kind == "Namespace" || entry.kind == "Library") {
            ensureBucket(namespaceOrder, namespaces, sanitizeIdentifier(entry.name)).header = &entry;
        }
    }

    for (const SchemaLuaEntry& entry : schema.luaEntries) {
        if (entry.binding == "LuaLibrary" || entry.scope == "Component" || entry.kind == "Component" ||
            entry.kind == "Callback" || entry.kind == "Type" || entry.kind == "Enum" || entry.kind == "Namespace" ||
            entry.kind == "Library") {
            continue;
        }
        if (entry.owner.empty() || entry.kind == "GlobalFunction" || entry.kind == "GlobalValue") {
            if (entry.kind == "GlobalFunction" || entry.kind == "Function") {
                globalFunctions.push_back(&entry);
            } else if (entry.kind == "GlobalValue" || entry.kind == "Property") {
                globalValues.push_back(&entry);
            }
            continue;
        }
        if (entry.scope == "ScriptClass" || entry.kind == "PropertyKey" || entry.kind == "PropertyType" ||
            entry.kind == "ClassField" || entry.kind == "ScriptClass") {
            continue;
        }
        const std::string ownerIdent = sanitizeIdentifier(entry.owner);
        if (enums.contains(ownerIdent)) {
            enums[ownerIdent].properties.push_back(&entry);
            continue;
        }
        const bool isMethodOrInstance =
            entry.kind == "Method" || entry.scope == "Instance" || entry.qualifiedName.find(':') != std::string::npos;
        if (types.contains(ownerIdent) || (isMethodOrInstance && !namespaces.contains(ownerIdent))) {
            OwnerBucket& bucket = ensureBucket(typeOrder, types, ownerIdent);
            if (entry.kind == "Property") {
                bucket.properties.push_back(&entry);
            } else if (isMethodOrInstance) {
                bucket.instanceMethods.push_back(&entry);
            } else {
                bucket.staticFunctions.push_back(&entry);
            }
        } else {
            OwnerBucket& bucket = ensureBucket(namespaceOrder, namespaces, ownerIdent);
            if (entry.kind == "Property") {
                bucket.properties.push_back(&entry);
            } else if (isMethodOrInstance) {
                bucket.instanceMethods.push_back(&entry);
            } else {
                bucket.staticFunctions.push_back(&entry);
            }
        }
    }

    // Ensure `Entity` exists with `GetComponent` / `AddComponent` overloads whenever components exist.
    if (!schema.components.empty() && !types.contains("Entity")) {
        ensureBucket(typeOrder, types, "Entity");
    }

    // 4. Emit Types (`Entity`, `Vec2`, `Vec3`, `Vec4`, `Quat`, ...).
    for (const std::string& typeName : typeOrder) {
        const OwnerBucket& bucket = types.at(typeName);
        if (bucket.header != nullptr) {
            appendMetadataDocs(out, *bucket.header);
        }
        out += std::format("---@class {}\n", typeName);
        for (const SchemaLuaEntry* prop : bucket.properties) {
            const std::string fieldName = sanitizeIdentifier(prop->name, "field");
            std::string fieldType = normalizeLuaType(prop->returnType);
            if (fieldType.empty()) {
                fieldType = "any";
            }
            out += std::format("---@field {} {}", fieldName, fieldType);
            if (prop->source) {
                out += std::format(" @Source: {}:{}", prop->source->file, prop->source->line);
            }
            out += '\n';
        }
        out += std::format("{} = {{}}\n\n", typeName);

        bool hasGetComponent = false;
        bool hasAddComponent = false;
        for (const SchemaLuaEntry* fn : bucket.staticFunctions) {
            appendMetadataDocs(out, *fn);
            const std::string fnName = sanitizeIdentifier(fn->name, "Func");
            appendCallableStub(out, std::format("{}.{}", typeName, fnName), {}, fn->parameters, fn->returnType,
                               fn->returnDescription, typeName, fn->name);
        }
        for (const SchemaLuaEntry* method : bucket.instanceMethods) {
            if (typeName == "Entity" && method->name == "GetComponent") {
                hasGetComponent = true;
            }
            if (typeName == "Entity" && method->name == "AddComponent") {
                hasAddComponent = true;
            }
            appendMetadataDocs(out, *method);
            const std::string methodName = sanitizeIdentifier(method->name, "Method");
            appendCallableStub(out, std::format("{}:{}", typeName, methodName), typeName, method->parameters,
                               method->returnType, method->returnDescription, typeName, method->name,
                               componentOverloads);
        }
        if (typeName == "Entity" && !componentOverloads.empty()) {
            const std::array<SchemaParameter, 1> nameParam = {SchemaParameter{
                .name = "name", .type = "ComponentName", .description = "Component name", .defaultValue = {}}};
            if (!hasGetComponent) {
                appendCallableStub(out, "Entity:GetComponent", "Entity", nameParam, "any", "Component proxy", "Entity",
                                   "GetComponent", componentOverloads);
            }
            if (!hasAddComponent) {
                appendCallableStub(out, "Entity:AddComponent", "Entity", nameParam, "any", "Component proxy", "Entity",
                                   "AddComponent", componentOverloads);
            }
        }
    }

    // 5. Emit Enums (`Key`, `Mouse`, `GamepadButton`, ...).
    for (const std::string& enumName : enumOrder) {
        const OwnerBucket& bucket = enums.at(enumName);
        if (bucket.header != nullptr) {
            appendMetadataDocs(out, *bucket.header);
        }
        out += std::format("---@class {}\n", enumName);
        for (const SchemaLuaEntry* prop : bucket.properties) {
            const std::string fieldName = sanitizeIdentifier(prop->name, "Value");
            std::string fieldType = normalizeLuaType(prop->returnType);
            if (fieldType.empty()) {
                fieldType = "integer";
            }
            out += std::format("---@field {} {}", fieldName, fieldType);
            if (prop->source) {
                out += std::format(" @Source: {}:{}", prop->source->file, prop->source->line);
            }
            out += '\n';
        }
        out += std::format("{} = {{}}\n\n", enumName);
    }

    // 6. Emit Namespaces & Libraries (`Physics`, `Scene`, `Input`, `Time`, `UI`, ...).
    for (const std::string& nsName : namespaceOrder) {
        const OwnerBucket& bucket = namespaces.at(nsName);
        if (bucket.header != nullptr) {
            appendMetadataDocs(out, *bucket.header);
        }
        out += std::format("---@class {}\n", nsName);
        for (const SchemaLuaEntry* prop : bucket.properties) {
            const std::string fieldName = sanitizeIdentifier(prop->name, "Value");
            std::string fieldType = normalizeLuaType(prop->returnType);
            if (fieldType.empty()) {
                fieldType = "any";
            }
            out += std::format("---@field {} {}", fieldName, fieldType);
            if (prop->source) {
                out += std::format(" @Source: {}:{}", prop->source->file, prop->source->line);
            }
            out += '\n';
        }
        out += std::format("{} = {{}}\n\n", nsName);

        for (const SchemaLuaEntry* fn : bucket.staticFunctions) {
            appendMetadataDocs(out, *fn);
            const std::string fnName = sanitizeIdentifier(fn->name, "Func");
            appendCallableStub(out, std::format("{}.{}", nsName, fnName), {}, fn->parameters, fn->returnType,
                               fn->returnDescription, nsName, fn->name);
        }
        for (const SchemaLuaEntry* method : bucket.instanceMethods) {
            appendMetadataDocs(out, *method);
            const std::string methodName = sanitizeIdentifier(method->name, "Method");
            appendCallableStub(out, std::format("{}:{}", nsName, methodName), nsName, method->parameters,
                               method->returnType, method->returnDescription, nsName, method->name);
        }
    }

    // 7. Emit Global Values and Global Functions.
    for (const SchemaLuaEntry* val : globalValues) {
        appendMetadataDocs(out, *val);
        const std::string valName = sanitizeIdentifier(val->name, " GlobalVal");
        std::string valType = normalizeLuaType(val->returnType);
        if (valType.empty()) {
            valType = "any";
        }
        out += std::format("---@type {}\n{} = nil\n\n", valType, valName);
    }
    for (const SchemaLuaEntry* fn : globalFunctions) {
        appendMetadataDocs(out, *fn);
        const std::string fnName = sanitizeIdentifier(fn->name, "GlobalFunc");
        appendCallableStub(out, fnName, {}, fn->parameters, fn->returnType, fn->returnDescription, {}, fn->name);
    }

    // 8. Emit `FrostScript` and Script Event callbacks (`schema.events`).
    out += "--- Base class for FrostEngine Lua entity scripts.\n";
    out += "---@class FrostScript\n";
    out += "---@field Entity Entity\n";
    out += "---@field Properties? table<string, any>\n";
    for (const SchemaEvent& event : schema.events) {
        const std::string evName = sanitizeIdentifier(event.name, "OnEvent");
        std::string funSig = "fun(self: FrostScript";
        for (std::size_t i = 0; i < event.parameters.size(); ++i) {
            const SchemaParameter& param = event.parameters[i];
            const std::string pName = sanitizeIdentifier(param.name, std::format("arg{}", i + 1));
            std::string pType = normalizeLuaType(param.type);
            if (pType.empty()) {
                pType = "any";
            }
            funSig += std::format(", {}{}: {}", pName, param.optional ? "?" : "", pType);
        }
        funSig += ')';
        out += std::format("---@field {}? {}", evName, funSig);
        if (!event.source.file.empty() && event.source.line > 0) {
            out += std::format(" @Source: {}:{}", event.source.file, event.source.line);
        }
        out += '\n';
    }
    out += "FrostScript = {}\n\n";

    for (const SchemaEvent& event : schema.events) {
        const std::string evName = sanitizeIdentifier(event.name, "OnEvent");
        appendDocLines(out, event.description);
        appendSourceDoc(out, event.source);
        appendCallableStub(out, std::format("FrostScript:{}", evName), "FrostScript", event.parameters, {}, {});
        appendDocLines(out, event.description);
        appendSourceDoc(out, event.source);
        appendCallableStub(out, evName, {}, event.parameters, {}, {});
    }

    return out;
}

std::vector<lang::LuaMetaFile> generateEngineLuaMetaFiles(const SchemaDocument& schema) {
    return {lang::LuaMetaFile{
        .name = std::string(kEngineLuaStubFileName),
        .content = generateEngineLuaStubs(schema),
    }};
}

namespace {

[[nodiscard]] std::optional<SchemaSource> parseSourceTag(std::string_view line) {
    constexpr std::string_view kMarker = "Source: ";
    const std::size_t pos = line.find(kMarker);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view rest = trimView(line.substr(pos + kMarker.size()));
    const std::size_t colon = rest.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= rest.size()) {
        return std::nullopt;
    }
    const std::string_view filePart = trimView(rest.substr(0, colon));
    const std::string_view linePart = trimView(rest.substr(colon + 1));
    std::uint32_t lineNumber = 0;
    const auto [ptr, ec] = std::from_chars(linePart.data(), linePart.data() + linePart.size(), lineNumber);
    if (ec != std::errc{} || lineNumber == 0 || filePart.empty()) {
        return std::nullopt;
    }
    return SchemaSource{std::string(filePart), lineNumber};
}

// Checks whether the caret is inside a generated meta stub block carrying `Source: <file>:<line>`.
[[nodiscard]] std::optional<SchemaSource> resolveStubCommentSource(std::string_view text, std::size_t offset) {
    if (!text.starts_with("---@meta")) {
        return std::nullopt;
    }
    const std::size_t clamped = std::min(offset, text.size());
    std::size_t lineStart = text.rfind('\n', clamped == 0 ? 0 : clamped - 1);
    lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
    std::size_t lineEnd = text.find('\n', clamped);
    if (lineEnd == std::string_view::npos) {
        lineEnd = text.size();
    }
    const std::string_view currentLine = text.substr(lineStart, lineEnd - lineStart);
    if (auto direct = parseSourceTag(currentLine)) {
        return direct;
    }
    // Walk upwards through contiguous `---` comment lines above a `function` or `---@class` line.
    std::size_t cursor = lineStart;
    while (cursor > 0) {
        const std::size_t prevEnd = cursor - 1;
        std::size_t prevStart = text.rfind('\n', prevEnd == 0 ? 0 : prevEnd - 1);
        prevStart = prevStart == std::string_view::npos ? 0 : prevStart + 1;
        const std::string_view prevLine = trimView(text.substr(prevStart, prevEnd - prevStart));
        if (!prevLine.starts_with("---")) {
            break;
        }
        if (auto found = parseSourceTag(prevLine)) {
            return found;
        }
        cursor = prevStart;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SchemaSource> entrySource(const SchemaDocument& schema, const SchemaLuaEntry& entry) {
    if (entry.source) {
        return entry.source;
    }
    if (!entry.sameAs.empty()) {
        if (const SchemaLuaEntry* target = schema.findLuaEntry(entry.sameAs); target != nullptr && target->source) {
            return target->source;
        }
    }
    return std::nullopt;
}

[[nodiscard]] const SchemaComponent* findComponentFlexible(const SchemaDocument& schema, std::string_view name) {
    if (const SchemaComponent* exact = schema.findComponent(name)) {
        return exact;
    }
    const std::string sanitized = sanitizeIdentifier(name);
    for (const SchemaComponent& comp : schema.components) {
        const std::string compSanitized = sanitizeIdentifier(comp.name);
        if (compSanitized == name || compSanitized == sanitized || compSanitized + "Component" == name) {
            return &comp;
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<SchemaSource>
findComponentMemberSource(const SchemaDocument& schema, const SchemaComponent& comp, std::string_view memberName) {
    for (const SchemaField& field : comp.fields) {
        if (field.name == memberName || sanitizeIdentifier(field.name) == memberName) {
            return field.source;
        }
    }
    const std::string compSanitized = sanitizeIdentifier(comp.name);
    for (const SchemaLuaEntry& entry : schema.luaEntries) {
        if (matchesComponentOwner(entry, comp.name, compSanitized) &&
            (entry.name == memberName || sanitizeIdentifier(entry.name) == memberName)) {
            if (auto src = entrySource(schema, entry)) {
                return src;
            }
        }
    }
    return std::nullopt;
}

// Infers the component name or engine type of a local variable `varName` from `luaText`
// (e.g. `local t = entity:GetComponent("Transform")` or `---@type Transform`).
[[nodiscard]] std::optional<std::string> inferVariableType(std::string_view luaText, std::string_view varName) {
    if (varName.empty()) {
        return std::nullopt;
    }
    if (varName == "Entity" || varName == "entity" || varName == "self.Entity") {
        return "Entity";
    }
    // Search for `<varName> = ...GetComponent("...")` or `AddComponent("...")`.
    std::size_t pos = 0;
    while ((pos = luaText.find(varName, pos)) != std::string_view::npos) {
        const bool leftOk = pos == 0 || !isIdentChar(luaText[pos - 1]);
        const std::size_t after = pos + varName.size();
        const bool rightOk = after >= luaText.size() || !isIdentChar(luaText[after]);
        if (leftOk && rightOk) {
            std::size_t lineEnd = luaText.find('\n', after);
            if (lineEnd == std::string_view::npos) {
                lineEnd = luaText.size();
            }
            const std::string_view restOfLine = luaText.substr(after, lineEnd - after);
            for (const std::string_view callName : {"GetComponent", "AddComponent"}) {
                const std::size_t callPos = restOfLine.find(callName);
                if (callPos != std::string_view::npos) {
                    const std::string_view afterCall = restOfLine.substr(callPos + callName.size());
                    const std::size_t quote1 = afterCall.find_first_of("\"'");
                    if (quote1 != std::string_view::npos) {
                        const char q = afterCall[quote1];
                        const std::size_t quote2 = afterCall.find(q, quote1 + 1);
                        if (quote2 != std::string_view::npos && quote2 > quote1 + 1) {
                            return std::string(afterCall.substr(quote1 + 1, quote2 - quote1 - 1));
                        }
                    }
                }
            }
            // Also check the preceding line for `---@type <TypeName>`.
            std::size_t lineStart = luaText.rfind('\n', pos == 0 ? 0 : pos - 1);
            if (lineStart != std::string_view::npos && lineStart > 0) {
                std::size_t prevStart = luaText.rfind('\n', lineStart - 1);
                prevStart = prevStart == std::string_view::npos ? 0 : prevStart + 1;
                const std::string_view prevLine = trimView(luaText.substr(prevStart, lineStart - prevStart));
                constexpr std::string_view kTypeTag = "---@type";
                if (prevLine.starts_with(kTypeTag)) {
                    const std::string_view typeText = trimView(prevLine.substr(kTypeTag.size()));
                    std::size_t endIdent = 0;
                    while (endIdent < typeText.size() && isIdentChar(typeText[endIdent])) {
                        ++endIdent;
                    }
                    if (endIdent > 0) {
                        return std::string(typeText.substr(0, endIdent));
                    }
                }
            }
        }
        pos = after;
    }
    return std::nullopt;
}

} // namespace

std::optional<SchemaSource> resolveLuaCppBinding(const SchemaDocument& schema, std::string_view luaText,
                                                 ByteOffset caretOffset) {
    if (luaText.empty()) {
        return std::nullopt;
    }
    const std::size_t clamped = std::min(static_cast<std::size_t>(caretOffset.value()), luaText.size());

    // 0. If the caret is in the generated meta stub itself, check `Source: <file>:<line>` first.
    if (auto fromStub = resolveStubCommentSource(luaText, clamped)) {
        return fromStub;
    }

    // 1. Check if the caret is inside a single-line string literal `"..."` or `'...'` (e.g. `"Transform"`).
    {
        std::size_t lineStart = luaText.rfind('\n', clamped == 0 ? 0 : clamped - 1);
        lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        std::size_t lineEnd = luaText.find('\n', clamped);
        if (lineEnd == std::string_view::npos) {
            lineEnd = luaText.size();
        }
        const std::size_t col = clamped - lineStart;
        const std::string_view line = luaText.substr(lineStart, lineEnd - lineStart);
        char openQuote = '\0';
        std::size_t quoteStart = 0;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (openQuote == '\0' && (c == '"' || c == '\'')) {
                openQuote = c;
                quoteStart = i;
            } else if (openQuote != '\0' && c == openQuote && (i == 0 || line[i - 1] != '\\')) {
                if (col >= quoteStart && col <= i) {
                    const std::string_view literal = line.substr(quoteStart + 1, i - quoteStart - 1);
                    if (const SchemaComponent* comp = findComponentFlexible(schema, literal)) {
                        return comp->source;
                    }
                    if (const SchemaLuaEntry* entry = schema.findLuaEntry(literal)) {
                        if (auto src = entrySource(schema, *entry)) {
                            return src;
                        }
                    }
                    for (const SchemaEvent& ev : schema.events) {
                        if (ev.name == literal) {
                            return ev.source;
                        }
                    }
                }
                openQuote = '\0';
            }
        }
    }

    // 2. Locate the identifier around `clamped` (or immediately adjacent to `.` / `:`).
    std::size_t pos = clamped;
    if (pos == luaText.size() && pos > 0) {
        --pos;
    }
    if (pos < luaText.size() && (luaText[pos] == '.' || luaText[pos] == ':') && pos + 1 < luaText.size() &&
        isIdentStart(luaText[pos + 1])) {
        ++pos;
    } else if (pos < luaText.size() && !isIdentChar(luaText[pos]) && pos > 0 && isIdentChar(luaText[pos - 1])) {
        --pos;
    }
    if (pos >= luaText.size() || !isIdentChar(luaText[pos])) {
        return std::nullopt;
    }

    std::size_t identStart = pos;
    while (identStart > 0 && isIdentChar(luaText[identStart - 1])) {
        --identStart;
    }
    std::size_t identEnd = pos;
    while (identEnd < luaText.size() && isIdentChar(luaText[identEnd])) {
        ++identEnd;
    }
    const std::string_view ident = luaText.substr(identStart, identEnd - identStart);

    // Check if `ident` is preceded by `.` or `:`.
    std::size_t before = identStart;
    while (before > 0 && (luaText[before - 1] == ' ' || luaText[before - 1] == '\t')) {
        --before;
    }
    if (before > 0 && (luaText[before - 1] == '.' || luaText[before - 1] == ':')) {
        const char sep = luaText[before - 1];
        std::size_t recvEnd = before - 1;
        while (recvEnd > 0 && (luaText[recvEnd - 1] == ' ' || luaText[recvEnd - 1] == '\t')) {
            --recvEnd;
        }
        // Case 2a: Chained call `...:GetComponent("Comp").Field`
        if (recvEnd > 0 && luaText[recvEnd - 1] == ')') {
            const std::size_t openParen = luaText.rfind('(', recvEnd - 1);
            if (openParen != std::string_view::npos && openParen < recvEnd) {
                const std::string_view args = luaText.substr(openParen + 1, recvEnd - openParen - 2);
                const std::size_t q1 = args.find_first_of("\"'");
                if (q1 != std::string_view::npos) {
                    const char q = args[q1];
                    const std::size_t q2 = args.find(q, q1 + 1);
                    if (q2 != std::string_view::npos && q2 > q1 + 1) {
                        const std::string_view compName = args.substr(q1 + 1, q2 - q1 - 1);
                        if (const SchemaComponent* comp = findComponentFlexible(schema, compName)) {
                            if (auto src = findComponentMemberSource(schema, *comp, ident)) {
                                return src;
                            }
                        }
                    }
                }
            }
        }
        // Case 2b: Identifier receiver `Receiver.ident` or `Receiver:ident`
        std::size_t recvStart = recvEnd;
        while (recvStart > 0 && isIdentChar(luaText[recvStart - 1])) {
            --recvStart;
        }
        if (recvStart < recvEnd) {
            const std::string_view receiver = luaText.substr(recvStart, recvEnd - recvStart);
            for (const std::string& candidate :
                 {std::format("{}{}{}", receiver, sep, ident), std::format("{}.{}", receiver, ident),
                  std::format("{}:{}", receiver, ident)}) {
                if (const SchemaLuaEntry* entry = schema.findLuaEntry(candidate)) {
                    if (auto src = entrySource(schema, *entry)) {
                        return src;
                    }
                }
            }
            if (const SchemaComponent* comp = findComponentFlexible(schema, receiver)) {
                if (auto src = findComponentMemberSource(schema, *comp, ident)) {
                    return src;
                }
            }
            if (auto inferred = inferVariableType(luaText, receiver)) {
                if (const SchemaComponent* comp = findComponentFlexible(schema, *inferred)) {
                    if (auto src = findComponentMemberSource(schema, *comp, ident)) {
                        return src;
                    }
                }
                for (const std::string& candidate :
                     {std::format("{}.{}", *inferred, ident), std::format("{}:{}", *inferred, ident)}) {
                    if (const SchemaLuaEntry* entry = schema.findLuaEntry(candidate)) {
                        if (auto src = entrySource(schema, *entry)) {
                            return src;
                        }
                    }
                }
            }
            // Script event definition `function MyScript:OnUpdate(dt)`
            for (const SchemaEvent& ev : schema.events) {
                if (ev.name == ident) {
                    return ev.source;
                }
            }
        }
    }

    // 3. If `ident` is followed by `.Member` or `:Member` (e.g. caret on `Physics` in `Physics.Raycast`),
    //    check if `ident` is a namespace/type/component prefix of a qualified entry!
    std::size_t after = identEnd;
    while (after < luaText.size() && (luaText[after] == ' ' || luaText[after] == '\t')) {
        ++after;
    }
    if (after + 1 < luaText.size() && (luaText[after] == '.' || luaText[after] == ':') &&
        isIdentStart(luaText[after + 1])) {
        const char sep = luaText[after];
        const std::size_t memberStart = after + 1;
        std::size_t memberEnd = memberStart;
        while (memberEnd < luaText.size() && isIdentChar(luaText[memberEnd])) {
            ++memberEnd;
        }
        const std::string_view member = luaText.substr(memberStart, memberEnd - memberStart);
        for (const std::string& candidate :
             {std::format("{}{}{}", ident, sep, member), std::format("{}.{}", ident, member),
              std::format("{}:{}", ident, member)}) {
            if (const SchemaLuaEntry* entry = schema.findLuaEntry(candidate)) {
                if (auto src = entrySource(schema, *entry)) {
                    return src;
                }
            }
        }
        if (const SchemaComponent* comp = findComponentFlexible(schema, ident)) {
            if (auto src = findComponentMemberSource(schema, *comp, member)) {
                return src;
            }
        }
    }

    // 4. Bare component name (`Transform`), event name (`OnUpdate`), or Lua entry (`Entity`, `Raycast`).
    if (const SchemaComponent* comp = findComponentFlexible(schema, ident)) {
        return comp->source;
    }
    for (const SchemaEvent& ev : schema.events) {
        if (ev.name == ident) {
            return ev.source;
        }
    }
    if (const SchemaLuaEntry* entry = schema.findLuaEntry(ident)) {
        if (auto src = entrySource(schema, *entry)) {
            return src;
        }
    }
    for (const SchemaLuaEntry& entry : schema.luaEntries) {
        if (entry.name == ident) {
            if (auto src = entrySource(schema, entry)) {
                return src;
            }
        }
    }
    for (const SchemaComponent& comp : schema.components) {
        for (const SchemaField& field : comp.fields) {
            if (field.name == ident) {
                return field.source;
            }
        }
    }
    return std::nullopt;
}

Result<ResolvedSource> resolveLuaCppBindingSource(io::IFileSystem& fs, std::string_view engineRoot,
                                                  const SchemaDocument& schema, std::string_view luaText,
                                                  ByteOffset caretOffset) {
    auto source = resolveLuaCppBinding(schema, luaText, caretOffset);
    if (!source) {
        return fail(ErrorCode::NotFound, "no FrostEngine C++ binding matches the symbol at the caret");
    }
    return resolveSchemaSource(fs, engineRoot, *source);
}

namespace {

[[nodiscard]] std::optional<std::string> findBuiltEditorBinary(io::IFileSystem& fs, std::string_view engineRoot,
                                                               std::string_view preferredPreset) {
    std::vector<std::string> presets;
    if (!preferredPreset.empty()) {
        presets.emplace_back(preferredPreset);
    }
#ifdef _WIN32
    for (std::string_view preset : {"windows-debug", "windows-release", "windows-relwithdebinfo"}) {
        if (preset != preferredPreset) {
            presets.emplace_back(preset);
        }
    }
#else
    for (std::string_view preset : {"linux-debug", "linux-release", "linux-relwithdebinfo"}) {
        if (preset != preferredPreset) {
            presets.emplace_back(preset);
        }
    }
#endif
    for (const std::string& preset : presets) {
        if (auto found = findEngineEditorProgram(fs, engineRoot, preset)) {
            return *found;
        }
    }
    if (auto buildDir = io::joinPath(engineRoot, "build")) {
        if (auto entries = fs.listDirectory(*buildDir)) {
            for (const io::DirEntry& entry : *entries) {
                if (entry.type != io::FileType::Directory) {
                    continue;
                }
                if (auto found = findEngineEditorProgram(fs, engineRoot, entry.name)) {
                    return *found;
                }
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool isFrostEngineRootCandidate(io::IFileSystem& fs, std::string_view dir,
                                              std::string_view preferredPreset) {
    if (auto docPath = io::joinPath(dir, lang::kFrostEngineScriptApiReference)) {
        if (auto st = fs.status(*docPath); st && st->type == io::FileType::Regular) {
            return true;
        }
    }
    return findBuiltEditorBinary(fs, dir, preferredPreset).has_value();
}

void addFolderScriptRuntimePaths(io::IFileSystem& fs, std::string_view folder, std::string_view scriptsRoot,
                                 std::vector<std::string>& runtimePath) {
    const auto addPattern = [&](std::string pattern) {
        if (!std::ranges::contains(runtimePath, pattern)) {
            runtimePath.push_back(std::move(pattern));
        }
    };
    for (const std::string_view relDir : {std::string_view("Assets/Scripts"), scriptsRoot}) {
        if (relDir.empty()) {
            continue;
        }
        if (auto full = io::joinPath(folder, relDir)) {
            if (auto st = fs.status(*full); st && st->type == io::FileType::Directory) {
                addPattern(std::format("{}/?.lua", relDir));
                addPattern(std::format("{}/?/init.lua", relDir));
            }
        }
    }
}

} // namespace

EngineLuaHostProvider::EngineLuaHostProvider() : m_launcher(&io::realProcessLauncher()) {}

EngineLuaHostProvider::EngineLuaHostProvider(io::IProcessLauncher& launcher, std::string preset)
    : m_launcher(&launcher), m_preset(std::move(preset)) {}

EngineLuaHostProvider::EngineLuaHostProvider(SchemaDocument fixedSchema, std::string fixedEngineRoot)
    : m_fixedSchema(std::move(fixedSchema)), m_fixedEngineRoot(std::move(fixedEngineRoot)) {}

std::string_view EngineLuaHostProvider::name() const noexcept { return "FrostEngine"; }

std::optional<std::string> EngineLuaHostProvider::engineRootForWorkspace(io::IFileSystem& fs,
                                                                         std::string_view folder) const {
    if (!m_fixedEngineRoot.empty()) {
        return m_fixedEngineRoot;
    }
    auto normalized = io::normalizePath(folder);
    if (!normalized || !io::isAbsolutePath(*normalized)) {
        return std::nullopt;
    }
    std::string current = *normalized;
    for (int depth = 0; depth <= 4 && !current.empty(); ++depth) {
        if (isFrostEngineRootCandidate(fs, current, m_preset)) {
            return current;
        }
        const std::string parent(io::parentPath(current));
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

std::optional<SchemaDocument> EngineLuaHostProvider::schemaForWorkspace(io::IFileSystem& fs,
                                                                        std::string_view folder) const {
    if (m_fixedSchema) {
        return m_fixedSchema;
    }
    if (m_launcher == nullptr) {
        return std::nullopt;
    }
    const auto engineRoot = engineRootForWorkspace(fs, folder);
    if (!engineRoot) {
        return std::nullopt;
    }
    const auto program = findBuiltEditorBinary(fs, *engineRoot, m_preset);
    if (!program) {
        return std::nullopt;
    }
    IClock::SystemTime mtime{};
    if (auto st = fs.status(*program)) {
        mtime = st->mtime;
    }
    {
        LockGuard lock(m_mutex);
        if (auto it = m_cache.find(*program); it != m_cache.end() && it->second.mtime == mtime) {
            return it->second.document;
        }
    }
    EngineCliRequest request;
    request.command = "schema.export";
    request.workingDirectory = *engineRoot;
    auto response = runEngineCliResult(*m_launcher, *program, request);
    if (!response) {
        return std::nullopt;
    }
    auto parsed = parseSchemaDocument(*response);
    if (!parsed) {
        return std::nullopt;
    }
    {
        LockGuard lock(m_mutex);
        m_cache[*program] = CachedSchema{.mtime = mtime, .document = *parsed};
    }
    return *parsed;
}

std::optional<lang::LuaHostContribution> EngineLuaHostProvider::contribute(io::IFileSystem& fs,
                                                                           std::string_view folder) {
    auto normalized = io::normalizePath(folder);
    if (!normalized || !io::isAbsolutePath(*normalized)) {
        return std::nullopt;
    }

    // Query FEC-10a fallback first so we inherit subproject folders (`Samples/MiniMMO`,
    // `TestProject`, ...) and watched input paths.
    lang::FrostEngineLuaProvider fallbackProvider;
    std::optional<lang::LuaHostContribution> fallback = fallbackProvider.contribute(fs, *normalized);

    const std::optional<SchemaDocument> schema = schemaForWorkspace(fs, *normalized);
    if (!schema) {
        return fallback;
    }

    lang::LuaHostContribution out;
    out.provider = std::string(name());
    out.runtimeVersion = schema->luaVersion;
    out.globals = schemaLuaGlobals(*schema);
    out.metaFiles = generateEngineLuaMetaFiles(*schema);
    addFolderScriptRuntimePaths(fs, *normalized, schema->scriptsRoot, out.runtimePath);

    if (fallback) {
        out.folders = std::move(fallback->folders);
        out.inputs = std::move(fallback->inputs);
    }
    if (const auto engineRoot = engineRootForWorkspace(fs, *normalized)) {
        if (const auto program = findBuiltEditorBinary(fs, *engineRoot, m_preset)) {
            if (!std::ranges::contains(out.inputs, *program)) {
                out.inputs.push_back(*program);
            }
        }
    }
    out.note = std::format("schema.export ({} components, {} Lua entries, {} events)", schema->components.size(),
                           schema->luaEntries.size(), schema->events.size());
    return out;
}

} // namespace frost::engine
