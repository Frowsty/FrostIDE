// SPDX-License-Identifier: Apache-2.0
//
// frost::engine's contract v2 A reader (schema.hpp): validates the schema.export document member by
// member with JSON-path error messages, and resolves its source locations to lines of the engine
// checkout.

#include "frost/engine/schema.hpp"

#include "frost/io/path.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <set>
#include <utility>

namespace frost::engine {
namespace {

using rpc::Json;

std::string_view kindName(Json::Kind kind) {
    switch (kind) {
    case Json::Kind::Null: return "null";
    case Json::Kind::Bool: return "a boolean";
    case Json::Kind::Integer: return "an integer";
    case Json::Kind::Double: return "a number";
    case Json::Kind::String: return "a string";
    case Json::Kind::Array: return "an array";
    case Json::Kind::Object: return "an object";
    }
    return "a value";
}

// `path`.`key`, or `key` at the top level.
std::string child(const std::string& path, std::string_view key) {
    return path.empty() ? std::string(key) : std::format("{}.{}", path, key);
}

// Reads members of one object, naming the first fault by its JSON path. Every accessor returns
// nullptr/nullopt after a fault, and the first fault is kept.
class Reader {
public:
    explicit Reader(std::optional<Error>& fault) : m_fault(fault) {}

    [[nodiscard]] bool failed() const noexcept { return m_fault.has_value(); }

    void fault(const std::string& path, std::string_view message) {
        if (!m_fault) {
            m_fault.emplace(ErrorCode::Corrupt, std::format("schema.export {}: {}", path, message));
        }
    }

    const Json* member(const Json& object, const std::string& path, std::string_view key, Json::Kind kind) {
        if (failed()) {
            return nullptr;
        }
        const Json* value = object.find(key);
        const std::string at = child(path, key);
        if (value == nullptr) {
            fault(at, "missing");
            return nullptr;
        }
        if (value->kind() != kind && !(kind == Json::Kind::Integer && value->asInteger())) {
            fault(at, std::format("is {}, expected {}", kindName(value->kind()), kindName(kind)));
            return nullptr;
        }
        return value;
    }

    std::string string(const Json& object, const std::string& path, std::string_view key, bool required = false) {
        const Json* value = member(object, path, key, Json::Kind::String);
        if (value == nullptr) {
            return {};
        }
        if (required && value->asString()->empty()) {
            fault(child(path, key), "is empty");
        }
        return *value->asString();
    }

    bool boolean(const Json& object, const std::string& path, std::string_view key) {
        const Json* value = member(object, path, key, Json::Kind::Bool);
        return value != nullptr && *value->asBool();
    }

    const Json* array(const Json& object, const std::string& path, std::string_view key) {
        return member(object, path, key, Json::Kind::Array);
    }

    // An array of strings, each checked against `vocabulary` when one is given.
    std::vector<std::string> strings(const Json& object, const std::string& path, std::string_view key,
                                     const std::set<std::string, std::less<>>* vocabulary = nullptr) {
        std::vector<std::string> out;
        const Json* list = array(object, path, key);
        if (list == nullptr) {
            return out;
        }
        for (std::size_t i = 0; i < list->size(); ++i) {
            const std::string at = std::format("{}[{}]", child(path, key), i);
            const std::string* text = list->items()[i].asString();
            if (text == nullptr) {
                fault(at, std::format("is {}, expected a string", kindName(list->items()[i].kind())));
                return out;
            }
            if (vocabulary != nullptr && !vocabulary->contains(*text)) {
                fault(at, std::format("\"{}\" is not one of the document's own names", *text));
                return out;
            }
            out.push_back(*text);
        }
        return out;
    }

    // A source location; null only when `nullable`.
    std::optional<SchemaSource> source(const Json& object, const std::string& path, bool nullable) {
        const Json* value = object.find("source");
        const std::string at = std::format("{}.source", path);
        if (failed()) {
            return std::nullopt;
        }
        if (value == nullptr) {
            fault(at, "missing");
            return std::nullopt;
        }
        if (value->isNull()) {
            if (!nullable) {
                fault(at, "is null: every engine registration names its C++ source");
            }
            return std::nullopt;
        }
        if (!value->isObject()) {
            fault(at, std::format("is {}, expected an object or null", kindName(value->kind())));
            return std::nullopt;
        }
        SchemaSource out;
        out.file = string(*value, at, "file", true);
        const Json* line = member(*value, at, "line", Json::Kind::Integer);
        if (failed()) {
            return std::nullopt;
        }
        const std::int64_t number = *line->asInteger();
        if (number < 1 || number > std::numeric_limits<std::uint32_t>::max()) {
            fault(at + ".line", std::format("{} is not a line number", number));
            return std::nullopt;
        }
        out.line = static_cast<std::uint32_t>(number);
        // Repository-relative with forward slashes, never escaping the checkout.
        if (out.file.find('\\') != std::string::npos || out.file.front() == '/' || io::isAbsolutePath(out.file) ||
            out.file.find(':') != std::string::npos) {
            fault(at + ".file", std::format("\"{}\" is not a repository-relative path", out.file));
            return std::nullopt;
        }
        std::size_t start = 0;
        while (start <= out.file.size()) {
            const std::size_t end = std::min(out.file.find('/', start), out.file.size());
            const std::string_view part = std::string_view(out.file).substr(start, end - start);
            if (part.empty() || part == "." || part == "..") {
                fault(at + ".file", std::format("\"{}\" has an empty, \".\" or \"..\" component", out.file));
                return std::nullopt;
            }
            start = end + 1;
        }
        return out;
    }

private:
    std::optional<Error>& m_fault;
};

// Splits text into lines without their breaks (\n or \r\n); a final break adds no line.
std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.emplace_back(line);
        start = end + 1;
    }
    return lines;
}

Result<std::string> sourcePath(std::string_view engineRoot, std::string_view file) {
    if (!io::isAbsolutePath(engineRoot)) {
        return fail(ErrorCode::InvalidArgument, "the engine root must be absolute");
    }
    bool escapes = file.empty() || io::isAbsolutePath(file) || file.find(':') != std::string_view::npos ||
                   file.front() == '/' || file.front() == '\\';
    for (std::size_t start = 0; !escapes && start <= file.size();) {
        const std::size_t end = std::min(file.find_first_of("/\\", start), file.size());
        escapes = file.substr(start, end - start) == "..";
        start = end + 1;
    }
    if (escapes) {
        return fail(ErrorCode::InvalidArgument, std::format("\"{}\" is not a path inside the engine checkout", file));
    }
    auto root = io::normalizePath(engineRoot);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    return io::joinPath(*root, file);
}

} // namespace

const SchemaLuaEntry* SchemaDocument::findLuaEntry(std::string_view qualifiedName) const noexcept {
    const auto it = std::ranges::find(luaEntries, qualifiedName, &SchemaLuaEntry::qualifiedName);
    return it != luaEntries.end() ? &*it : nullptr;
}

const SchemaComponent* SchemaDocument::findComponent(std::string_view name) const noexcept {
    const auto it = std::ranges::find(components, name, &SchemaComponent::name);
    return it != components.end() ? &*it : nullptr;
}

Result<SchemaDocument> parseSchemaDocument(const rpc::Json& document) {
    if (!document.isObject()) {
        return fail(ErrorCode::Corrupt,
                    std::format("schema.export printed {}, not an object", kindName(document.kind())));
    }
    if (document.stringAt("schema") != kSchemaExportName) {
        return fail(ErrorCode::Unsupported,
                    std::format("not a FrostEngine schema export (schema \"{}\")", document.stringAt("schema")));
    }
    if (document.integerAt("schemaVersion") != kSchemaExportVersion) {
        const Json* version = document.find("schemaVersion");
        return fail(ErrorCode::Unsupported,
                    std::format("FrostEngine schema export version {} is not {} (this IDE reads {})",
                                version != nullptr ? version->dump() : "missing", kSchemaExportVersion,
                                kSchemaExportVersion));
    }

    std::optional<Error> fault;
    Reader read(fault);
    SchemaDocument out;
    out.schemaVersion = kSchemaExportVersion;
    if (const Json* engine = read.member(document, "", "engine", Json::Kind::Object)) {
        out.engineVersion = read.string(*engine, "engine", "version", true);
    }
    out.costTags = read.strings(document, "", "costTags");
    out.bindingKinds = read.strings(document, "", "bindingKinds");
    const std::set<std::string, std::less<>> costTags(out.costTags.begin(), out.costTags.end());
    const std::set<std::string, std::less<>> bindingKinds(out.bindingKinds.begin(), out.bindingKinds.end());

    if (const Json* components = read.array(document, "", "components")) {
        for (std::size_t i = 0; i < components->size() && !read.failed(); ++i) {
            const Json& component = components->items()[i];
            const std::string at = std::format("components[{}]", i);
            if (!component.isObject()) {
                read.fault(at, "is not an object");
                break;
            }
            SchemaComponent parsed;
            parsed.name = read.string(component, at, "name", true);
            if (auto source = read.source(component, at, false)) {
                parsed.source = std::move(*source);
            }
            if (const Json* fields = read.array(component, at, "fields")) {
                for (std::size_t f = 0; f < fields->size() && !read.failed(); ++f) {
                    const Json& field = fields->items()[f];
                    const std::string fieldAt = std::format("{}.fields[{}]", at, f);
                    if (!field.isObject()) {
                        read.fault(fieldAt, "is not an object");
                        break;
                    }
                    SchemaField parsedField;
                    parsedField.name = read.string(field, fieldAt, "name", true);
                    parsedField.type = read.string(field, fieldAt, "type", true);
                    if (auto source = read.source(field, fieldAt, false)) {
                        parsedField.source = std::move(*source);
                    }
                    parsed.fields.push_back(std::move(parsedField));
                }
            }
            out.components.push_back(std::move(parsed));
        }
    }

    if (const Json* lua = read.member(document, "", "lua", Json::Kind::Object)) {
        out.luaVersion = read.string(*lua, "lua", "version", true);
        out.scriptsRoot = read.string(*lua, "lua", "scriptsRoot", true);
        (void)read.string(*lua, "lua", "modulePathPattern", true);
        if (const Json* entries = read.array(*lua, "lua", "entries")) {
            for (std::size_t i = 0; i < entries->size() && !read.failed(); ++i) {
                const Json& entry = entries->items()[i];
                const std::string at = std::format("lua.entries[{}]", i);
                if (!entry.isObject()) {
                    read.fault(at, "is not an object");
                    break;
                }
                SchemaLuaEntry parsed;
                parsed.kind = read.string(entry, at, "kind", true);
                parsed.qualifiedName = read.string(entry, at, "qualifiedName", true);
                parsed.name = read.string(entry, at, "name", true);
                parsed.signature = read.string(entry, at, "signature");
                parsed.module = read.string(entry, at, "module");
                (void)read.string(entry, at, "description");
                (void)read.array(entry, at, "parameters");
                (void)read.member(entry, at, "returns", Json::Kind::Object);
                (void)read.boolean(entry, at, "readOnly");
                parsed.testModeOnly = read.boolean(entry, at, "testModeOnly");
                if (const Json* binding = read.member(entry, at, "binding", Json::Kind::String)) {
                    parsed.binding = *binding->asString();
                    if (!bindingKinds.contains(parsed.binding)) {
                        read.fault(at + ".binding", std::format("\"{}\" is not one of bindingKinds", parsed.binding));
                    }
                }
                parsed.cost = read.strings(entry, at, "cost", &costTags);
                parsed.source = read.source(entry, at, true);
                if (const Json* sameAs = entry.find("sameAs")) {
                    if (!sameAs->isString() || sameAs->asString()->empty()) {
                        read.fault(at + ".sameAs", "is not a qualified name");
                    } else {
                        parsed.sameAs = *sameAs->asString();
                    }
                }
                out.luaEntries.push_back(std::move(parsed));
            }
        }
        if (const Json* aliases = read.array(*lua, "lua", "aliases")) {
            for (std::size_t i = 0; i < aliases->size() && !read.failed(); ++i) {
                const Json& alias = aliases->items()[i];
                const std::string at = std::format("lua.aliases[{}]", i);
                SchemaLuaAlias parsed;
                parsed.name = read.string(alias, at, "name", true);
                if (const Json* values = read.array(alias, at, "values")) {
                    for (std::size_t v = 0; v < values->size() && !read.failed(); ++v) {
                        parsed.values.push_back(
                            read.string(values->items()[v], std::format("{}.values[{}]", at, v), "value", true));
                    }
                }
                out.luaAliases.push_back(std::move(parsed));
            }
        }
    }

    if (const Json* events = read.array(document, "", "events")) {
        for (std::size_t i = 0; i < events->size() && !read.failed(); ++i) {
            const Json& event = events->items()[i];
            const std::string at = std::format("events[{}]", i);
            SchemaEvent parsed;
            parsed.name = read.string(event, at, "name", true);
            parsed.signature = read.string(event, at, "signature");
            if (auto source = read.source(event, at, false)) {
                parsed.source = std::move(*source);
            }
            out.events.push_back(std::move(parsed));
        }
    }

    if (const Json* commands = read.array(document, "", "commands")) {
        for (std::size_t i = 0; i < commands->size() && !read.failed(); ++i) {
            const Json& command = commands->items()[i];
            const std::string at = std::format("commands[{}]", i);
            SchemaCommand parsed;
            parsed.name = read.string(command, at, "name", true);
            parsed.category = read.string(command, at, "category");
            parsed.flags = read.strings(command, at, "flags");
            out.commands.push_back(std::move(parsed));
        }
    }

    // Every alias of a binding names an entry of the document.
    if (!read.failed()) {
        std::set<std::string_view, std::less<>> names;
        for (const SchemaLuaEntry& entry : out.luaEntries) {
            names.insert(entry.qualifiedName);
        }
        for (std::size_t i = 0; i < out.luaEntries.size(); ++i) {
            const std::string& sameAs = out.luaEntries[i].sameAs;
            if (!sameAs.empty() && !names.contains(sameAs)) {
                read.fault(std::format("lua.entries[{}].sameAs", i), std::format("\"{}\" names no entry", sameAs));
                break;
            }
        }
    }

    if (fault) {
        return std::unexpected(std::move(*fault));
    }
    return out;
}

Result<ResolvedSource> resolveSchemaSource(io::IFileSystem& fs, std::string_view engineRoot,
                                           const SchemaSource& source) {
    SourceResolver resolver(fs, std::string(engineRoot));
    return resolver.resolve(source);
}

SourceResolver::SourceResolver(io::IFileSystem& fs, std::string engineRoot) : m_fs(fs), m_root(std::move(engineRoot)) {}

Result<ResolvedSource> SourceResolver::resolve(const SchemaSource& source) {
    auto it = m_files.find(source.file);
    if (it == m_files.end()) {
        CachedFile file;
        auto path = sourcePath(m_root, source.file);
        if (!path) {
            return std::unexpected(std::move(path.error())); // Not cached: a caller error, not a file state.
        }
        file.path = std::move(*path);
        auto text = m_fs.readFile(file.path);
        if (text) {
            file.lines = splitLines(*text);
        } else {
            file.error = std::move(text.error());
        }
        it = m_files.emplace(source.file, std::move(file)).first;
    }
    const CachedFile& file = it->second;
    if (file.error) {
        return std::unexpected(*file.error);
    }
    if (source.line == 0 || source.line > file.lines.size()) {
        return fail(ErrorCode::OutOfRange,
                    std::format("{} has {} lines, not line {}", file.path, file.lines.size(), source.line));
    }
    return ResolvedSource{file.path, source.line, file.lines[source.line - 1]};
}

} // namespace frost::engine
