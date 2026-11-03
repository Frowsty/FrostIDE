// SPDX-License-Identifier: Apache-2.0
//
// language.cpp: frost::syntax's LanguageId names, the file-name table and LanguageRegistry
// (ADR 0023): grammar lookup, the ABI check and once-only query compilation.

#include "frost/syntax/language.hpp"

#include "language_state.hpp"
#include "ts.hpp"

#include <cstddef>
#include <format>
#include <string_view>

namespace frost::syntax {

namespace {

// ASCII lower-case copy of a short suffix into `buffer`; false if it does not fit.
[[nodiscard]] bool lowerAscii(std::string_view in, std::span<char> buffer, std::string_view& out) noexcept {
    if (in.size() > buffer.size()) {
        return false;
    }
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        buffer[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    out = std::string_view(buffer.data(), in.size());
    return true;
}

struct Extension {
    std::string_view suffix; // Lower case, with the dot.
    LanguageId language;
};

// The table of language.hpp; ".cmake.in" is matched before ".in" would be (none is listed).
constexpr Extension kExtensions[] = {
    {".c", LanguageId::C},          {".cpp", LanguageId::Cpp},
    {".cc", LanguageId::Cpp},       {".cxx", LanguageId::Cpp},
    {".c++", LanguageId::Cpp},      {".hpp", LanguageId::Cpp},
    {".hh", LanguageId::Cpp},       {".hxx", LanguageId::Cpp},
    {".h++", LanguageId::Cpp},      {".h", LanguageId::Cpp},
    {".inl", LanguageId::Cpp},      {".ipp", LanguageId::Cpp},
    {".tpp", LanguageId::Cpp},      {".ixx", LanguageId::Cpp},
    {".cppm", LanguageId::Cpp},     {".cs", LanguageId::CSharp},
    {".csx", LanguageId::CSharp},   {".cmake", LanguageId::CMake},
    {".xml", LanguageId::Xml},      {".xsd", LanguageId::Xml},
    {".xaml", LanguageId::Xml},     {".props", LanguageId::Xml},
    {".targets", LanguageId::Xml},  {".vcxproj", LanguageId::Xml},
    {".filters", LanguageId::Xml},  {".csproj", LanguageId::Xml},
    {".proj", LanguageId::Xml},     {".nuspec", LanguageId::Xml},
    {".resx", LanguageId::Xml},     {".natvis", LanguageId::Xml},
    {".manifest", LanguageId::Xml}, {".wxs", LanguageId::Xml},
    {".slnx", LanguageId::Xml},     {".config", LanguageId::Xml},
    {".svg", LanguageId::Xml},      {".json", LanguageId::Json},
    {".lua", LanguageId::Lua},      {".jsonc", LanguageId::Json},
    {".md", LanguageId::Markdown},  {".markdown", LanguageId::Markdown},
};

[[nodiscard]] const TSLanguage* grammarOf(LanguageId id) noexcept {
    switch (id) {
    case LanguageId::C: return tree_sitter_c();
    case LanguageId::Cpp: return tree_sitter_cpp();
    case LanguageId::CSharp: return tree_sitter_c_sharp();
    case LanguageId::CMake: return tree_sitter_cmake();
    case LanguageId::Xml: return tree_sitter_xml();
    case LanguageId::Json: return tree_sitter_json();
    case LanguageId::Markdown: return tree_sitter_markdown();
    case LanguageId::Lua: return tree_sitter_lua();
    case LanguageId::None: break;
    }
    return nullptr;
}

[[nodiscard]] std::size_t indexOf(LanguageId id) noexcept { return static_cast<std::size_t>(id); }

} // namespace

std::string_view toString(LanguageId id) noexcept {
    switch (id) {
    case LanguageId::C: return "c";
    case LanguageId::Cpp: return "cpp";
    case LanguageId::CSharp: return "csharp";
    case LanguageId::CMake: return "cmake";
    case LanguageId::Xml: return "xml";
    case LanguageId::Json: return "json";
    case LanguageId::Markdown: return "markdown";
    case LanguageId::Lua: return "lua";
    case LanguageId::None: break;
    }
    return "none";
}

LanguageId languageForFileName(std::string_view utf8Name) noexcept {
    const std::size_t slash = utf8Name.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? utf8Name : utf8Name.substr(slash + 1);
    std::array<char, 32> buffer{};
    std::string_view lower;
    if (name.size() == 14 && lowerAscii(name, buffer, lower) && lower == "cmakelists.txt") {
        return LanguageId::CMake;
    }
    if (name.size() > 9 && lowerAscii(name.substr(name.size() - 9), buffer, lower) && lower == ".cmake.in") {
        return LanguageId::CMake;
    }
    const std::size_t dot = name.rfind('.');
    // A leading dot alone (".gitignore") is a hidden file's name, not an extension.
    if (dot == std::string_view::npos || dot == 0) {
        return LanguageId::None;
    }
    if (!lowerAscii(name.substr(dot), buffer, lower)) {
        return LanguageId::None;
    }
    for (const Extension& extension : kExtensions) {
        if (extension.suffix == lower) {
            return extension.language;
        }
    }
    return LanguageId::None;
}

LanguageRegistry::LanguageRegistry() noexcept : m_state(std::make_unique<detail::RegistryState>()) {
    for (const LanguageId id : kAllLanguages) {
        auto language = std::make_unique<Language>();
        language->id = id;
        language->grammar = grammarOf(id);
        m_state->languages[indexOf(id)] = std::move(language);
    }
}

LanguageRegistry::~LanguageRegistry() = default;

Result<const Language*> LanguageRegistry::get(LanguageId id) const noexcept {
    if (id == LanguageId::None || indexOf(id) >= kLanguageIdCount) {
        return fail(ErrorCode::InvalidArgument, "no grammar for language 'none'");
    }
    const Language* language = m_state->languages[indexOf(id)].get();
    const std::uint32_t abi = ts_language_abi_version(language->grammar);
    if (abi < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION || abi > TREE_SITTER_LANGUAGE_VERSION) {
        return fail(ErrorCode::Unsupported,
                    std::format("the {} grammar has ABI {}, outside [{}, {}]", toString(id), abi,
                                TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION, TREE_SITTER_LANGUAGE_VERSION));
    }
    return language;
}

Result<void> LanguageRegistry::prepare(LanguageId id) const noexcept {
    Result<const Language*> found = get(id);
    if (!found) {
        return std::unexpected(std::move(found.error()));
    }
    const Language& language = **found;
    std::call_once(language.m_once, [&language]() noexcept {
        const std::string_view name = toString(language.id);
        Result<detail::CompiledQuery> highlights =
            detail::compileQuery(language.grammar, detail::querySource(language.id, detail::QueryKind::Highlights),
                                 detail::QueryKind::Highlights, std::format("highlights.scm ({})", name));
        if (!highlights) {
            language.m_error.emplace(std::move(highlights.error()));
            return;
        }
        Result<detail::CompiledQuery> outline =
            detail::compileQuery(language.grammar, detail::querySource(language.id, detail::QueryKind::Outline),
                                 detail::QueryKind::Outline, std::format("outline.scm ({})", name));
        if (!outline) {
            language.m_error.emplace(std::move(outline.error()));
            return;
        }
        if (language.id == LanguageId::Lua || language.id == LanguageId::Cpp || language.id == LanguageId::CSharp ||
            language.id == LanguageId::CMake) {
            Result<detail::CompiledQuery> folds =
                detail::compileQuery(language.grammar, detail::querySource(language.id, detail::QueryKind::Folds),
                                     detail::QueryKind::Folds, std::format("folds.scm ({})", name));
            if (!folds) {
                language.m_error.emplace(std::move(folds.error()));
                return;
            }
            language.m_folds = std::move(*folds);
        }
        language.m_highlights = std::move(*highlights);
        language.m_outline = std::move(*outline);
        // Release: pairs with queriesReady()'s acquire, so readers see the compiled queries.
        language.m_ready.store(true, std::memory_order_release);
    });
    // call_once synchronises with the completed call, so m_error is visible here.
    if (language.m_error) {
        return std::unexpected(*language.m_error);
    }
    return {};
}

bool LanguageRegistry::queriesReady(LanguageId id) const noexcept {
    if (id == LanguageId::None || indexOf(id) >= kLanguageIdCount) {
        return false;
    }
    return m_state->languages[indexOf(id)]->queriesReady();
}

Result<std::uint32_t> LanguageRegistry::abiVersion(LanguageId id) const noexcept {
    Result<const Language*> found = get(id);
    if (!found) {
        return std::unexpected(std::move(found.error()));
    }
    return ts_language_abi_version((*found)->grammar);
}

} // namespace frost::syntax
