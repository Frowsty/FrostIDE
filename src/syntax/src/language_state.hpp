// SPDX-License-Identifier: Apache-2.0
//
// language_state.hpp: the private state behind frost::syntax::Language and LanguageRegistry
// (ADR 0023): a grammar, its compiled highlight and outline queries with their capture tables and
// text predicates, and the once-only compilation.
#pragma once

#include "frost/core/error.hpp"
#include "frost/syntax/highlight.hpp"
#include "frost/syntax/language.hpp"
#include "frost/syntax/outline.hpp"
#include "frost/text/snapshot.hpp"

#include "ts.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::syntax {

namespace detail {

// The text predicates our queries may use (ADR 0023 D6 f); anything else rejects the query.
// No regular expressions (#match?), so predicate cost is bounded by the comparand.
enum class PredicateOp : std::uint8_t {
    Eq,         // #eq? @a "text" or #eq? @a @b
    NotEq,      // #not-eq? ...
    AnyOf,      // #any-of? @a "x" "y" ...
    NotAnyOf,   // #not-any-of? ...
    Upper,      // #frost-upper? @a: ASCII [A-Z][A-Z0-9_]*, at least two bytes (macro-style names)
    DocComment, // #frost-doc? @a: starts with "///", "//!", "/*!" or "/**" (but is not "/**/")
};

// One predicate of a pattern. `capture` is the capture tested. Eq and NotEq compare it with
// `otherCapture` when set, else with strings[0]; AnyOf and NotAnyOf with every entry of `strings`.
struct Predicate {
    PredicateOp op = PredicateOp::Eq;
    std::uint32_t capture = 0;
    std::optional<std::uint32_t> otherCapture;
    std::vector<std::string> strings;
};

enum class QueryKind : std::uint8_t { Highlights, Outline, Folds };

// What a capture of an outline query means: the declaration (kind set) or its name.
struct OutlineRole {
    std::optional<OutlineKind> kind;
    bool isName = false;
};

struct QueryDeleter {
    void operator()(TSQuery* query) const noexcept { ts_query_delete(query); }
};

// A compiled query with its tables. Immutable after compilation; TSQuery is safe to read from
// several threads at once (each query call uses its own TSQueryCursor).
struct CompiledQuery {
    std::unique_ptr<TSQuery, QueryDeleter> query;
    std::vector<std::optional<HighlightKind>> highlightKinds; // Highlights: per capture id; nullopt for "_x".
    std::vector<OutlineRole> outlineRoles;                    // Outline: per capture id.
    std::vector<std::vector<Predicate>> predicates;           // Per pattern index.
};

// The embedded text of a query (src/syntax/queries/<language>/<kind>.scm). Empty for None.
[[nodiscard]] std::string_view querySource(LanguageId language, QueryKind kind) noexcept;

// Compiles `source` for `grammar` and builds its tables. Errors: Corrupt naming `label`, for a
// tree-sitter query error, an unknown capture name or an unknown or malformed predicate.
[[nodiscard]] Result<CompiledQuery> compileQuery(const TSLanguage* grammar, std::string_view source, QueryKind kind,
                                                 std::string_view label) noexcept;

// True when every predicate of the match's pattern holds. Node text is read from `text` (the
// snapshot whose coordinates the tree is in), at most comparand length + 1 bytes per test.
[[nodiscard]] bool predicatesHold(const CompiledQuery& query, const TSQueryMatch& match,
                                  const text::Snapshot& text) noexcept;

// Copies the bytes of [begin, end) of `text` into `buffer` if they fit and returns the view, else
// returns nullopt. Ranges are clamped to text.size(). O(B log n + |range|) and allocation-free.
[[nodiscard]] std::optional<std::string_view> readSmall(const text::Snapshot& text, std::uint64_t begin,
                                                        std::uint64_t end, std::span<char> buffer) noexcept;

struct RegistryState;

} // namespace detail

// A grammar and its queries (see LanguageRegistry). The queries are written once, inside
// `once`, before `ready` is released; readers check queriesReady() (acquire) first.
class Language {
public:
    LanguageId id = LanguageId::None;
    const TSLanguage* grammar = nullptr;

    // True once both queries compiled (release store after they are written).
    [[nodiscard]] bool queriesReady() const noexcept { return m_ready.load(std::memory_order_acquire); }

    // Precondition: queriesReady().
    [[nodiscard]] const detail::CompiledQuery& highlights() const noexcept { return m_highlights; }
    [[nodiscard]] const detail::CompiledQuery& outline() const noexcept { return m_outline; }
    [[nodiscard]] const detail::CompiledQuery& folds() const noexcept { return m_folds; }

private:
    friend class LanguageRegistry;

    mutable std::once_flag m_once;
    mutable std::atomic<bool> m_ready{false};
    mutable std::optional<Error> m_error;
    mutable detail::CompiledQuery m_highlights;
    mutable detail::CompiledQuery m_outline;
    mutable detail::CompiledQuery m_folds;
};

namespace detail {

struct RegistryState {
    std::array<std::unique_ptr<Language>, kLanguageIdCount> languages;
};

} // namespace detail

} // namespace frost::syntax
