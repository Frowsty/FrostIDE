// SPDX-License-Identifier: Apache-2.0
//
// The per-language table of frost/text/editor/language.hpp (ADR 0025): comment tokens and the file
// names and extensions that select each language. constexpr data, no static initialisation.

#include "frost/text/editor/language.hpp"

#include <array>
#include <cstddef>

namespace frost::text {

namespace {

constexpr std::array kLanguages{
    LanguageInfo{"text", "Plain text", "", "", ""},
    LanguageInfo{"c", "C", "//", "/*", "*/"},
    LanguageInfo{"cpp", "C++", "//", "/*", "*/"},
    LanguageInfo{"csharp", "C#", "//", "/*", "*/"},
    LanguageInfo{"hlsl", "HLSL", "//", "/*", "*/"},
    LanguageInfo{"glsl", "GLSL", "//", "/*", "*/"},
    LanguageInfo{"javascript", "JavaScript", "//", "/*", "*/"},
    LanguageInfo{"typescript", "TypeScript", "//", "/*", "*/"},
    LanguageInfo{"jsonc", "JSON with comments", "//", "/*", "*/"},
    LanguageInfo{"json", "JSON", "", "", ""},
    LanguageInfo{"cmake", "CMake", "#", "#[[", "]]"},
    LanguageInfo{"python", "Python", "#", "", ""},
    LanguageInfo{"shell", "Shell script", "#", "", ""},
    LanguageInfo{"powershell", "PowerShell", "#", "<#", "#>"},
    LanguageInfo{"batch", "Batch file", "REM", "", ""},
    LanguageInfo{"yaml", "YAML", "#", "", ""},
    LanguageInfo{"toml", "TOML", "#", "", ""},
    LanguageInfo{"ini", "INI", ";", "", ""},
    LanguageInfo{"xml", "XML", "", "<!--", "-->"},
    LanguageInfo{"html", "HTML", "", "<!--", "-->"},
    LanguageInfo{"markdown", "Markdown", "", "<!--", "-->"},
    LanguageInfo{"sql", "SQL", "--", "/*", "*/"},
    LanguageInfo{"lua", "Lua", "--", "--[[", "]]"},
};

// File names first (exact, case-insensitive), then extensions with their dot.
struct PathRule {
    std::string_view suffix;
    std::string_view language;
};
constexpr std::array kNames{
    PathRule{"cmakelists.txt", "cmake"},
};
constexpr std::array kExtensions{
    PathRule{".c", "c"},
    PathRule{".h", "cpp"},
    PathRule{".cpp", "cpp"},
    PathRule{".cc", "cpp"},
    PathRule{".cxx", "cpp"},
    PathRule{".c++", "cpp"},
    PathRule{".hpp", "cpp"},
    PathRule{".hh", "cpp"},
    PathRule{".hxx", "cpp"},
    PathRule{".h++", "cpp"},
    PathRule{".inl", "cpp"},
    PathRule{".ipp", "cpp"},
    PathRule{".tpp", "cpp"},
    PathRule{".ixx", "cpp"},
    PathRule{".cppm", "cpp"},
    PathRule{".cs", "csharp"},
    PathRule{".hlsl", "hlsl"},
    PathRule{".hlsli", "hlsl"},
    PathRule{".fx", "hlsl"},
    PathRule{".glsl", "glsl"},
    PathRule{".vert", "glsl"},
    PathRule{".frag", "glsl"},
    PathRule{".comp", "glsl"},
    PathRule{".js", "javascript"},
    PathRule{".mjs", "javascript"},
    PathRule{".ts", "typescript"},
    PathRule{".jsonc", "jsonc"},
    PathRule{".json", "json"},
    PathRule{".cmake", "cmake"},
    PathRule{".py", "python"},
    PathRule{".sh", "shell"},
    PathRule{".bash", "shell"},
    PathRule{".ps1", "powershell"},
    PathRule{".psm1", "powershell"},
    PathRule{".psd1", "powershell"},
    PathRule{".bat", "batch"},
    PathRule{".cmd", "batch"},
    PathRule{".yml", "yaml"},
    PathRule{".yaml", "yaml"},
    PathRule{".toml", "toml"},
    PathRule{".ini", "ini"},
    PathRule{".xml", "xml"},
    PathRule{".vcxproj", "xml"},
    PathRule{".filters", "xml"},
    PathRule{".props", "xml"},
    PathRule{".targets", "xml"},
    PathRule{".csproj", "xml"},
    PathRule{".slnx", "xml"},
    PathRule{".natvis", "xml"},
    PathRule{".xaml", "xml"},
    PathRule{".resx", "xml"},
    PathRule{".html", "html"},
    PathRule{".htm", "html"},
    PathRule{".md", "markdown"},
    PathRule{".sql", "sql"},
    PathRule{".lua", "lua"},
};

constexpr char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// True when `text` ends with `suffix` (ASCII case-insensitive).
constexpr bool endsWithNoCase(std::string_view text, std::string_view suffix) noexcept {
    if (suffix.size() > text.size()) {
        return false;
    }
    const std::size_t start = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (lower(text[start + i]) != suffix[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

std::span<const LanguageInfo> languages() noexcept { return kLanguages; }

const LanguageInfo& plainTextLanguage() noexcept { return kLanguages[0]; }

const LanguageInfo* languageById(std::string_view id) noexcept {
    for (const LanguageInfo& language : kLanguages) {
        if (language.id == id) {
            return &language;
        }
    }
    return nullptr;
}

const LanguageInfo& languageForPath(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    for (const PathRule& rule : kNames) {
        if (name.size() == rule.suffix.size() && endsWithNoCase(name, rule.suffix)) {
            return *languageById(rule.language);
        }
    }
    for (const PathRule& rule : kExtensions) {
        if (name.size() > rule.suffix.size() && endsWithNoCase(name, rule.suffix)) {
            return *languageById(rule.language);
        }
    }
    return plainTextLanguage();
}

} // namespace frost::text
