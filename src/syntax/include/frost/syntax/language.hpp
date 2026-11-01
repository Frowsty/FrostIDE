// SPDX-License-Identifier: Apache-2.0
//
// frost::syntax languages (ADR 0023): the built-in LanguageId set, the file-name table that picks
// a language for a document, and LanguageRegistry, which owns the vendored tree-sitter grammars and
// compiles our highlight and outline queries for them once. Qt-free; no tree-sitter header here.
#pragma once

#include "frost/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace frost::syntax {

// The built-in languages. Grammars and queries are compiled in; nothing is loaded from disk,
// projects or settings (ADR 0023 D6 g, restricted mode).
enum class LanguageId : std::uint8_t { None, C, Cpp, CSharp, CMake, Xml, Json, Markdown };

// The number of LanguageId values, None included.
inline constexpr std::size_t kLanguageIdCount = 8;
// Every language with a grammar, in enumeration order.
inline constexpr std::array<LanguageId, 7> kAllLanguages = {LanguageId::C,       LanguageId::Cpp, LanguageId::CSharp,
                                                            LanguageId::CMake,   LanguageId::Xml, LanguageId::Json,
                                                            LanguageId::Markdown};

// "c", "cpp", "csharp", "cmake", "xml", "json", "markdown" or "none" (also for out-of-range values).
[[nodiscard]] std::string_view toString(LanguageId id) noexcept;

/*
 * The language of a file name (a base name or a path; only the part after the last '/' or '\'
 * counts), ASCII case-insensitive. O(|name|). The table:
 *   C         .c
 *   Cpp       .cpp .cc .cxx .c++ .hpp .hh .hxx .h++ .h .inl .ipp .tpp .ixx .cppm  (.h is C++)
 *   CSharp    .cs .csx
 *   CMake     CMakeLists.txt, *.cmake, *.cmake.in
 *   Xml       .xml .xsd .xaml .props .targets .vcxproj .filters .csproj .proj .nuspec .resx .natvis
 *             .manifest .wxs .slnx .config .svg
 *   Json      .json .jsonc
 *   Markdown  .md .markdown
 * Anything else is None.
 */
[[nodiscard]] LanguageId languageForFileName(std::string_view utf8Name) noexcept;

// A grammar and its compiled queries, owned by a LanguageRegistry (opaque; src/language_state.hpp).
class Language;

namespace detail {
struct RegistryState;
} // namespace detail

/*
 * The built-in grammars and their queries. Construction is cheap and compiles nothing; prepare()
 * compiles a language's queries once (tens of milliseconds for C++ and C#, so call it on a worker;
 * SyntaxSession::open does). Non-copyable and non-movable: sessions and trees hold pointers into it.
 *
 * Lifetime: the registry must outlive every SyntaxSession and SyntaxTree that uses it, and every
 * task a session hands to its scheduler (shut the scheduler down, or let it drain, before
 * destroying the registry).
 *
 * Thread safety: every member is safe to call concurrently from any thread.
 */
class LanguageRegistry {
public:
    LanguageRegistry() noexcept;
    ~LanguageRegistry();
    LanguageRegistry(const LanguageRegistry&) = delete ("sessions and trees point into the registry");
    LanguageRegistry& operator=(const LanguageRegistry&) = delete ("sessions and trees point into the registry");

    // The grammar of `id`; its queries may not be compiled yet. O(1).
    // Errors: InvalidArgument for None or an out-of-range value; Unsupported when the grammar's
    // ABI version lies outside the runtime's supported range.
    [[nodiscard]] Result<const Language*> get(LanguageId id) const noexcept;

    // Compiles the highlight and outline queries of `id` once (later calls return the first
    // result) and validates their capture names and predicates. Blocking; concurrent callers wait
    // for the first. Errors: those of get(); Corrupt naming the query and the problem
    // ("highlights.scm (cpp): TSQueryErrorNodeType at byte 123"), which is a FrostIDE bug.
    [[nodiscard]] Result<void> prepare(LanguageId id) const noexcept;

    // True once prepare(id) has succeeded (an acquire load). O(1).
    [[nodiscard]] bool queriesReady(LanguageId id) const noexcept;

    // The grammar's tree-sitter ABI version. Errors: those of get(). O(1).
    [[nodiscard]] Result<std::uint32_t> abiVersion(LanguageId id) const noexcept;

private:
    std::unique_ptr<detail::RegistryState> m_state;
};

} // namespace frost::syntax
