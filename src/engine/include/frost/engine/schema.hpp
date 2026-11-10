// SPDX-License-Identifier: Apache-2.0
//
// FrostEngine contract v2 A (FER-1a) on the IDE side: the schema document `FrostEditor
// schema.export` prints, validated and read into plain structs, and its C++ source locations
// resolved to files of the engine checkout. The document lists the engine's components with their
// fields, the whole Lua API (with how each binding reaches Lua and the costs it declares), the Lua
// aliases a script editor offers, the script events and the automation commands. Everything here is
// read from the engine's own output (the truth rule); a document that does not match the schema is
// refused with the JSON path of the first fault, never patched up.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/rpc/json.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::engine {

// The document's name and the one version this IDE reads.
inline constexpr std::string_view kSchemaExportName = "frostengine.schema-export";
inline constexpr std::int64_t kSchemaExportVersion = 1;

// Where the engine registered a binding: a repository-relative file ("Frost/src/...", forward
// slashes, no "." or ".." components) and a 1-based line.
struct SchemaSource {
    std::string file;
    std::uint32_t line = 0;
    friend bool operator==(const SchemaSource&, const SchemaSource&) = default;
};

struct SchemaField {
    std::string name;
    std::string type; // The engine's field type name ("Float", "Vector3", "Entity", ...).
    SchemaSource source;
};

struct SchemaComponent {
    std::string name;
    std::vector<SchemaField> fields;
    SchemaSource source;
};

struct SchemaParameter {
    std::string name;
    std::string type;
    std::string description;
    bool optional = false;
    std::string defaultValue;
};

// One entry of the Lua API (a namespace, function, method, property, component proxy, ...).
struct SchemaLuaEntry {
    std::string kind;          // The engine's ScriptApiKind name ("Function", "Method", "Component", ...).
    std::string scope;         // "Global", "Static", "Instance", "Component", "Member", "Callback".
    std::string owner;         // "Physics", "Entity", "Transform", ... ("" for top-level globals/types).
    std::string name;          // "Raycast".
    std::string fullName;      // "Physics.Raycast", "Transform.Translation".
    std::string qualifiedName; // "Physics.Raycast", "Entity:GetComponent".
    std::string signature;
    std::string description;
    std::vector<SchemaParameter> parameters;
    std::string returnType;
    std::string returnDescription;
    std::string module;
    bool readOnly = false;
    std::string binding;                // One of SchemaDocument::bindingKinds ("Wrapped", "Raw", ...).
    std::vector<std::string> cost;      // Declared cost tags, each one of SchemaDocument::costTags.
    std::optional<SchemaSource> source; // nullopt for Lua's own libraries (no engine C++ behind them).
    std::string sameAs;                 // The qualified name of the entry bound to the same native function, or "".
    bool testModeOnly = false;
};

struct SchemaLuaAlias {
    std::string name; // "ComponentName", "FrostPropertyType".
    std::vector<std::string> values;
};

struct SchemaEvent {
    std::string name; // "OnUpdate".
    std::string signature;
    std::string description;
    std::vector<SchemaParameter> parameters;
    SchemaSource source;
};

struct SchemaCommand {
    std::string name;
    std::string category;
    std::vector<std::string> flags;
};

// The whole document, in the engine's order (registration order throughout).
struct SchemaDocument {
    std::int64_t schemaVersion = 0;
    std::string engineVersion;
    std::vector<std::string> costTags;
    std::vector<std::string> bindingKinds;
    std::vector<SchemaComponent> components;
    std::string luaVersion; // "Lua 5.4".
    std::string scriptsRoot;
    std::vector<SchemaLuaEntry> luaEntries;
    std::vector<SchemaLuaAlias> luaAliases;
    std::vector<SchemaEvent> events;
    std::vector<SchemaCommand> commands;

    // The first Lua entry with this qualified name, or nullptr. O(n).
    [[nodiscard]] const SchemaLuaEntry* findLuaEntry(std::string_view qualifiedName) const noexcept;
    [[nodiscard]] const SchemaComponent* findComponent(std::string_view name) const noexcept;
};

/*
 * Validates `document` (the result object of schema.export) against schema version 1 and reads it.
 * Checked: the schema name; the version; every required member and its JSON type; every source
 * ({ file, line } with a relative forward-slash path and line >= 1; only Lua entries may have
 * null); every entry's binding and cost tags against the document's own vocabularies; every sameAs
 * naming an existing entry; non-empty names. Members the IDE does not read are allowed.
 * Errors: Unsupported for another schema name or version; Corrupt naming the JSON path of the first
 * fault ("lua.entries[12].cost[0]: ..."). Pure; O(size of the document).
 */
[[nodiscard]] Result<SchemaDocument> parseSchemaDocument(const rpc::Json& document);

// A source location of the document as a file of the engine checkout.
struct ResolvedSource {
    std::string path; // Absolute, native separators.
    std::uint32_t line = 0;
    std::string text; // The line's text, without its line break.
};

/*
 * Resolves `source` against the engine checkout at `engineRoot` (absolute): the file must exist
 * there and have at least `source.line` lines. Reads the file through `fs` on every call (callers
 * resolving many locations cache per file with SourceResolver). Errors: InvalidArgument for a
 * relative root or a source path that escapes it; NotFound when the file is missing; OutOfRange
 * when the file is shorter than the line; the file system's read errors.
 */
[[nodiscard]] Result<ResolvedSource> resolveSchemaSource(io::IFileSystem& fs, std::string_view engineRoot,
                                                         const SchemaSource& source);

// resolveSchemaSource with each file read once. Not thread-safe; owns its cache.
class SourceResolver {
public:
    // `fs` must outlive the resolver.
    SourceResolver(io::IFileSystem& fs, std::string engineRoot);

    [[nodiscard]] Result<ResolvedSource> resolve(const SchemaSource& source);

private:
    struct CachedFile {
        std::string path;
        std::optional<Error> error;
        std::vector<std::string> lines;
    };

    io::IFileSystem& m_fs;
    std::string m_root;
    std::map<std::string, CachedFile, std::less<>> m_files; // Keyed by the source's relative path.
};

} // namespace frost::engine
