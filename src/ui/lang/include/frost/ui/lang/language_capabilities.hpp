// SPDX-License-Identifier: Apache-2.0
//
// The language capability table (FND-3: LUA-2, ADR 0078): which editor-side language features may
// run for a document, per LSP languageId, so a C++ or C# feature never fires in a .lua file and a
// command without a correct engine is disabled with the epic that owns it.
/*
 * Columns. postfix (postfix templates on Tab/Ctrl+Space), liveTemplates (Insert Live Template;
 * the set is per language), surround (Surround With), completeStatement (Complete Current
 * Statement), optimizeImports (Optimize Imports/Includes through the language server: clangd's
 * include cleaner for C++, the server's source.organizeImports action for C#), textRefactorings
 * (refactorings computed on line text by the client: none, for every language; the column exists so
 * no future slice re-enables them for a language without an ADR), codeVision (the Code Vision lens
 * row; usage and implementation counts arrive with CPP-9) and clangdTweaks (the named refactor and
 * generate commands map to clangd tweaks, clangd_actions.hpp).
 *
 * Lua gets only the features its language server answers (completion, navigation, rename, code
 * actions, formatting): every column is false. Unknown languages ("plaintext") get nothing either.
 * Threads: constexpr data, safe anywhere.
 */
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace frost::ui::lang {

struct LanguageCapabilities {
    std::string_view languageId; // LSP languageId: "cpp", "csharp", "lua" or "plaintext".
    std::string_view serverName; // For messages: "clangd", "the C# language server", "LuaLS", "".
    bool postfix = false;
    bool liveTemplates = false;
    bool surround = false;
    bool completeStatement = false;
    bool optimizeImports = false;
    bool textRefactorings = false;
    bool codeVision = false;
    bool clangdTweaks = false;
};

inline constexpr std::array<LanguageCapabilities, 5> kLanguageCapabilities{{
    {.languageId = "cpp",
     .serverName = "clangd",
     .postfix = true,
     .liveTemplates = true,
     .surround = true,
     .completeStatement = true,
     .optimizeImports = true,
     .textRefactorings = false,
     .codeVision = true,
     .clangdTweaks = true},
    {.languageId = "csharp",
     .serverName = "the C# language server",
     .postfix = true,
     .liveTemplates = true,
     .surround = true,
     .completeStatement = true,
     .optimizeImports = true,
     .textRefactorings = false,
     .codeVision = true,
     .clangdTweaks = false},
    {.languageId = "cmake", .serverName = "CMake", .postfix = true, .liveTemplates = true},
    {.languageId = "lua", .serverName = "LuaLS"},
    {.languageId = "plaintext", .serverName = ""},
}};

// The LSP languageId of `path` by extension (ASCII case-insensitive): C, C++ and Objective-C(++)
// sources and headers are "cpp", .cs/.csx "csharp", .lua "lua", anything else "plaintext". O(path).
[[nodiscard]] constexpr std::string_view languageIdForPath(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (name == "CMakeLists.txt" || name == "cmakelists.txt")
        return "cmake";
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size() || name.size() - dot > 8) {
        return "plaintext";
    }
    char ext[8]{};
    const std::string_view raw = name.substr(dot + 1);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        ext[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view e(ext, raw.size());
    constexpr std::array<std::string_view, 18> kCpp{"c",   "cc",  "cpp", "cxx", "c++", "h",  "hh",   "hpp", "hxx",
                                                    "h++", "inl", "ipp", "tpp", "m",   "mm", "cppm", "ixx", "cu"};
    for (std::string_view candidate : kCpp) {
        if (e == candidate) {
            return "cpp";
        }
    }
    if (e == "cmake")
        return "cmake";
    if (e == "cs" || e == "csx") {
        return "csharp";
    }
    if (e == "lua") {
        return "lua";
    }
    return "plaintext";
}

// The row of `languageId` (the "plaintext" row for an unknown id). O(rows).
[[nodiscard]] constexpr const LanguageCapabilities& capabilitiesFor(std::string_view languageId) noexcept {
    for (const LanguageCapabilities& row : kLanguageCapabilities) {
        if (row.languageId == languageId) {
            return row;
        }
    }
    return kLanguageCapabilities.back();
}

// capabilitiesFor(languageIdForPath(path)).
[[nodiscard]] constexpr const LanguageCapabilities& capabilitiesForPath(std::string_view path) noexcept {
    return capabilitiesFor(languageIdForPath(path));
}

} // namespace frost::ui::lang
