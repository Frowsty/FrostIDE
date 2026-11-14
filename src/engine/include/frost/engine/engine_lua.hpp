// SPDX-License-Identifier: Apache-2.0
//
// Engine-aware Lua (FEC-10b, PLAN.md Section 7 R4 & Appendix C):
//   1. Generates LuaLS `---@meta` stub definitions from `frost::engine::SchemaDocument`
//      (`FrostEditor schema.export`, FER-1a) and supplies them through `frost::lang::ILuaHostProvider`
//      (`LUA-13`) so `frost::lang`'s generic Lua module (`lua_service`, `lua_rc`, `lua_host`)
//      contains zero FrostEngine-specific code.
//   2. Resolves Lua symbols (namespace calls, methods, component names, component fields, and
//      script event callbacks) to their C++ `SchemaSource` (`file:line`) from `schema.export`
//      for the "Go to C++ Binding" (`navigate.cppBinding`) action.
#pragma once

#include "frost/core/mutex.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/engine/schema.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/lua_host.hpp"
#include "frost/text/positions.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::engine {

// File name used for the generated `schema.export` LuaLS meta definitions inside FrostIDE's
// per-user host-meta directory.
inline constexpr std::string_view kEngineLuaStubFileName = "frost_schema.lua";

// Exact identity/range recorded while this schema entry is emitted, never recovered by searching
// the output text. Offsets are UTF-8 bytes in GeneratedEngineLuaMetadata::text. Duplicate schema
// identities or generated Lua declaration collisions are flagged, not resolved by first match.
struct EngineLuaDeclaration {
    std::size_t schemaEntryIndex = 0;
    std::string qualifiedName;
    std::string emittedIdentity;
    text::ByteRange selection;
    bool ambiguous = false;
};
struct GeneratedEngineLuaMetadata {
    std::string text;
    std::vector<EngineLuaDeclaration> declarations;
};

struct EngineLuaSchemaCapture {
    SchemaDocument schema;
    std::string engineRoot;
    std::string program; // Actual cache-producing editor binary; empty only for explicit fixed-schema providers.
    IClock::SystemTime programMtime{};
};

// Owning, pure metadata generation; safe concurrently with immutable schema inputs. The caller
// binds these byte ranges to the exact output URI/snapshot/provider generation before querying
// LuaLS. Entries that do not emit a declaration have no anchor; there is no lexical fallback.
// O(output bytes + entries^2), including collision detection.
[[nodiscard]] GeneratedEngineLuaMetadata generateEngineLuaMetadata(const SchemaDocument& schema);

// Generates deterministic LuaLS `---@meta` definitions from a validated `SchemaDocument`:
//   - `@alias` unions from `schema.luaAliases` (`ComponentName`, `FrostPropertyType`)
//   - Component proxy `@class` definitions with the exact fields and methods `schema.export` lists
//   - `Entity:GetComponent` / `Entity:AddComponent` `@overload` annotations mapping each component
//     name literal to its proxy class
//   - `@class FrostScript` and callback stubs for every event in `schema.events`
//   - Types, enums, namespaces, functions, methods, and properties from `schema.luaEntries`, with
//     cost tags, binding kinds, and `Source: <file>:<line>` comments.
[[nodiscard]] std::string generateEngineLuaStubs(const SchemaDocument& schema);

// Wraps `generateEngineLuaStubs(schema)` as a `LuaMetaFile` vector (`{kEngineLuaStubFileName, ...}`).
[[nodiscard]] std::vector<lang::LuaMetaFile> generateEngineLuaMetaFiles(const SchemaDocument& schema);

// Returns the deduplicated list of Lua global identifiers declared by `schema` (namespaces, global
// functions/values, types, enums, `FrostScript`, and script event callbacks).
[[nodiscard]] std::vector<std::string> schemaLuaGlobals(const SchemaDocument& schema);

// Returns the exact entry names (properties and methods, in schema order) that `schema` lists for
// `componentName` (`components[i].fields` and `lua.entries` with `scope == "Component"`).
[[nodiscard]] std::vector<std::string> schemaComponentEntryNames(const SchemaDocument& schema,
                                                                 std::string_view componentName);

// Resolves the Lua symbol at `caretOffset` in `luaText` (a user `.lua` script or generated meta
// stub) to its C++ `SchemaSource` (`{file, line}`) in `schema`, or `std::nullopt` when no engine
// C++ binding matches.
[[nodiscard]] std::optional<SchemaSource> resolveLuaCppBinding(const SchemaDocument& schema, std::string_view luaText,
                                                               ByteOffset caretOffset);

// Resolves the Lua symbol at `caretOffset` in `luaText` against `schema` and verifies/reads the
// target C++ file and line in `engineRoot` via `resolveSchemaSource`.
[[nodiscard]] Result<ResolvedSource> resolveLuaCppBindingSource(io::IFileSystem& fs, std::string_view engineRoot,
                                                                const SchemaDocument& schema, std::string_view luaText,
                                                                ByteOffset caretOffset);

// FrostEngine implementation of `lang::ILuaHostProvider` (FEC-10b, upgrading FEC-10a):
// When a built `FrostEditor` binary (or pre-loaded `SchemaDocument`) is available for the
// workspace, runs `schema.export` once per binary mtime and supplies generated `metaFiles`
// (`frost_schema.lua`), globals, and runtime version through `LUA-13`. When the engine checkout is
// unbuilt, falls back to `Docs/ScriptingAPI.md` and `.frost/lua-api` (FEC-10a).
class EngineLuaHostProvider final : public lang::ILuaHostProvider {
public:
    EngineLuaHostProvider();
    explicit EngineLuaHostProvider(io::IProcessLauncher& launcher, std::string preset = {});
    explicit EngineLuaHostProvider(SchemaDocument fixedSchema, std::string fixedEngineRoot = {});

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::optional<lang::LuaHostContribution> contribute(io::IFileSystem& fs,
                                                                      std::string_view folder) override;

    // Locates the FrostEngine checkout root for `folder` (either `folder` itself or an enclosing
    // engine checkout up to 4 levels above a sample/test project).
    [[nodiscard]] std::optional<std::string> engineRootForWorkspace(io::IFileSystem& fs, std::string_view folder) const;

    // Returns the cached or freshly exported `SchemaDocument` for `folder`, if available.
    [[nodiscard]] std::optional<SchemaDocument> schemaForWorkspace(io::IFileSystem& fs, std::string_view folder) const;
    // Reads an already exported cache only; NEVER starts a process. Blocking path/status discovery
    // belongs on a worker. Returns nullopt while warmup is absent/stale. Thread-safe; O(path I/O + schema copy).
    [[nodiscard]] std::optional<EngineLuaSchemaCapture> cachedSchemaCapture(io::IFileSystem& fs,
                                                                            std::string_view folder) const;

    // Returns true when `folder` belongs to a FrostEngine workspace with a built FrostEditor binary
    // whose schema.export has not yet been cached for its current mtime.
    void invalidateSchemaCache();

    [[nodiscard]] bool needsSchemaWarmup(io::IFileSystem& fs, std::string_view folder) const;

private:
    struct CachedSchema {
        IClock::SystemTime mtime{};
        std::optional<SchemaDocument> document;
        std::vector<std::string> globals;
        std::vector<lang::LuaMetaFile> metaFiles;
    };

    io::IProcessLauncher* m_launcher = nullptr;
    std::string m_preset;
    std::optional<SchemaDocument> m_fixedSchema;
    std::vector<std::string> m_fixedGlobals;
    std::vector<lang::LuaMetaFile> m_fixedMetaFiles;
    std::string m_fixedEngineRoot;
    mutable Mutex m_mutex;
    mutable std::map<std::string, CachedSchema, std::less<>> m_cache FROST_GUARDED_BY(m_mutex);
};

} // namespace frost::engine
