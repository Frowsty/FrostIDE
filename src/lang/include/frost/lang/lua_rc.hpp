// SPDX-License-Identifier: Apache-2.0
//
// A project's .luarc.json as the Lua settings page edits it (LUA-4, PLAN.md Appendix C R4): the
// runtime version, the libraries, the globals and the diagnostics switches, read from and written
// back into the file LuaLS itself reads, so the page and LuaLS can never disagree.
/*
 * Keys. LuaLS accepts each setting nested ({"runtime": {"version": "Lua 5.4"}}), dotted
 * ({"runtime.version": "Lua 5.4"}) and with its "Lua." prefix ({"Lua.runtime.version": ...}).
 * Reading takes the first form present in the order dotted with prefix, dotted, nested. Writing
 * updates every form the file already uses and adds a missing setting in the nested form; a value
 * equal to LuaLS's default (no version, empty lists, diagnostics enabled) removes the key, and
 * objects left empty by that are removed too. Every other key and the order of keys are kept.
 *
 * Format. The written text is the file's JSON re-printed with four-space indentation and a final
 * newline (comments are not supported: such a file is refused, Corrupt, and left alone).
 *
 * Thread safety: pure functions, except readLuaRc/writeLuaRc, which use `fs`.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

inline constexpr std::string_view kLuaRcFileName = ".luarc.json";

// The .luarc.json settings the Lua settings page shows.
struct LuaRcSettings {
    std::string runtimeVersion;                   // runtime.version; "" when the file sets none.
    std::vector<std::string> libraries;           // workspace.library.
    std::vector<std::string> globals;             // diagnostics.globals.
    std::vector<std::string> disabledDiagnostics; // diagnostics.disable (LuaLS diagnostic codes).
    bool diagnosticsEnabled = true;               // diagnostics.enable (LuaLS's default: true).
    friend bool operator==(const LuaRcSettings&, const LuaRcSettings&) = default;
};

// Reads the settings from .luarc.json text ("" reads as the defaults). Errors: Corrupt (not
// JSON, not an object, or a setting of the wrong type). O(size).
[[nodiscard]] Result<LuaRcSettings> parseLuaRc(std::string_view text);

// `text` with `settings` applied (header comment); "" starts from an empty object. Errors: as
// parseLuaRc. O(size).
[[nodiscard]] Result<std::string> applyLuaRc(std::string_view text, const LuaRcSettings& settings);

// A project's .luarc.json as read.
struct LuaRcFile {
    std::string path;    // <root>/.luarc.json, absolute.
    bool exists = false; // False: the file is missing and `text` is empty.
    std::string text;    // As read; writeLuaRc checks the disk still holds it.
    LuaRcSettings settings;
};

// Reads <root>/.luarc.json. Errors: InvalidArgument (relative root), Corrupt (as parseLuaRc), and
// the file system's other than NotFound (a missing file reads as the defaults).
[[nodiscard]] Result<LuaRcFile> readLuaRc(io::IFileSystem& fs, std::string_view root);

/*
 * Writes `settings` into the file `loaded` describes, atomically, when the disk still holds what
 * was read (or still holds no file). Writes nothing when the text would not change. Errors: Busy
 * (the file changed on disk since it was read; read it again), Corrupt, and the file system's.
 */
[[nodiscard]] Result<void> writeLuaRc(io::IFileSystem& fs, const LuaRcFile& loaded, const LuaRcSettings& settings);

} // namespace frost::lang
