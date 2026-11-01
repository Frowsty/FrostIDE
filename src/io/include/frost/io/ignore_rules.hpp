// SPDX-License-Identifier: Apache-2.0
//
/*
 * The gitignore matcher (ADR 0020): IgnoreRules, the parsed patterns of one ignore file, and IgnoreStack,
 * the rules in force at one directory of a walk (its own .gitignore over its parents' over the
 * repository-wide files). A clean-room implementation of the rules gitignore(5) describes, checked against
 * `git check-ignore` and `git ls-files` (tests/io/ignore_rules_git_test.cpp) and fuzzed
 * (tests/fuzz/io_ignore_rules_*).
 */
/*
 * The rules, as implemented:
 *   - A file is split into lines at LF; a CR before the LF is dropped, as is a UTF-8 byte-order mark
 *     at the start of the file. Blank lines and lines starting with '#' are skipped ("\#" starts a
 *     pattern with a literal '#'). Trailing spaces are dropped unless escaped with a backslash.
 *   - A leading '!' negates the pattern: a path it matches is re-included ("\!" is a literal '!').
 *   - A trailing '/' makes the pattern match directories only; it is then removed.
 *   - A pattern that still contains a '/' (at the start or in the middle) is anchored: it is matched
 *     against the path relative to the directory of its ignore file (a leading '/' is then removed).
 *     Otherwise it is matched against the last component of the path, at any depth below that
 *     directory.
 *   - '*' matches any run of characters except '/', '?' any one character except '/', and a bracket
 *     expression ("[a-z]", "[!0-9]" or "[^0-9]", "[]x]", POSIX classes such as "[[:digit:]]") one
 *     character from its set, never '/'. A backslash escapes the next character, also in brackets.
 *     Matching is byte-wise (a '?' matches one byte of a multi-byte UTF-8 character).
 *   - A run of two or more '*' that is preceded by the start or a '/' and followed by the end or a
 *     '/' is a globstar. Followed by a '/', it matches zero or more whole directories: the pattern
 *     "**", slash, "foo" matches foo at any depth, and "a", slash, "**", slash, "b" matches a/b and
 *     a/x/y/b. At the end it matches everything: "abc", slash, "**" matches everything inside abc,
 *     but not abc itself. Any other run of '*' is a single '*'. (The slashes are spelled out because
 *     the patterns would close this comment.)
 *   - Within one file the last matching pattern decides. A deeper directory's file takes precedence
 *     over its parents', which take precedence over the repository-wide files (info/exclude, then
 *     core.excludesFile) a caller puts at the bottom of the stack.
 *   - A path below an ignored directory is ignored whatever its own patterns say: git never lists an
 *     ignored directory, so no pattern can re-include something inside it (matchWithParents; the tree
 *     scan simply does not descend).
 *   - Patterns that can never match (an unterminated bracket expression, an unknown character class,
 *     a trailing backslash, a pattern that is empty after its '!' or '/' were removed) are kept as
 *     diagnostics and never match.
 * Case: exact by default; IgnoreOptions::caseInsensitive folds ASCII letters (core.ignorecase, which
 * git sets on case-insensitive file systems).
 *
 * Paths given to the matchers are relative to the walk root, '/'-separated, without a leading or
 * trailing '/' (for example "src/main.cpp"), whatever the platform's separator.
 *
 * Thread safety: both types are immutable after construction and safe to share between threads.
 * Complexity: matching one path against one pattern is O(pattern tokens x path length) (an NFA
 * simulation, no backtracking); literal patterns and "*.ext" patterns take a constant-time path.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {

struct IgnoreOptions {
    bool caseInsensitive = false; // Fold ASCII letters (core.ignorecase).
};

// One pattern line of an ignore file.
struct IgnoreRule {
    std::string text;       // The line as written, after trailing spaces were dropped (for messages).
    std::uint32_t line = 0; // 1-based line number in its file.
    bool negated = false;   // A leading '!': a match re-includes.
    bool directoryOnly = false;
    bool anchored = false; // Matched against the whole relative path (else against the last component).
    bool valid = true;     // False for the never-matching patterns listed in the header comment.
};

// A problem with one line of an ignore file (the line's pattern never matches).
struct IgnoreDiagnostic {
    std::uint32_t line = 0;
    std::string message;
};

/*
 * The parsed patterns of one ignore file. `base` is the directory the file applies to, relative to the
 * walk root ('/'-separated; "" for the root itself and for repository-wide files); `source` names the
 * file in messages (any string, typically its path). Parsing never fails: lines that cannot match are
 * reported in diagnostics(). O(text size).
 */
class IgnoreRules {
public:
    IgnoreRules() = default;
    [[nodiscard]] static IgnoreRules parse(std::string_view text, std::string base = {}, std::string source = {},
                                           IgnoreOptions options = {});

    [[nodiscard]] const std::string& base() const noexcept { return m_base; }
    [[nodiscard]] const std::string& source() const noexcept { return m_source; }
    [[nodiscard]] const std::vector<IgnoreRule>& rules() const noexcept { return m_rules; }
    [[nodiscard]] const std::vector<IgnoreDiagnostic>& diagnostics() const noexcept { return m_diagnostics; }
    [[nodiscard]] bool empty() const noexcept { return m_rules.empty(); }

    // The last rule of this file matching `path` (relative to the walk root, see the header comment), or
    // null. A path outside base() never matches. O(rules x pattern x path) worst case.
    [[nodiscard]] const IgnoreRule* lastMatch(std::string_view path, bool isDirectory) const;

    struct Compiled; // The rules' matchers (ignore_rules.cpp).

private:
    std::string m_base;
    std::string m_source;
    IgnoreOptions m_options;
    std::vector<IgnoreRule> m_rules;
    std::vector<IgnoreDiagnostic> m_diagnostics;
    std::shared_ptr<const Compiled> m_compiled; // Shared by copies; immutable.
};

// What decided a path: the deciding rule and its file, or none (not ignored).
struct IgnoreMatch {
    bool ignored = false;
    const IgnoreRule* rule = nullptr;   // Null when no rule matched; a negated rule when re-included.
    const IgnoreRules* rules = nullptr; // The file `rule` belongs to. Both live as long as the stack.
};

/*
 * The rules in force at one directory: a persistent stack of IgnoreRules, deepest first. Copies are cheap
 * (one shared pointer) and share their frames, so a parallel walk hands each directory its own stack.
 * pushed() returns a new stack; the receiver is unchanged.
 */
class IgnoreStack {
public:
    IgnoreStack() = default;

    // This stack with `rules` on top (taking precedence). Empty rule sets are not pushed. O(1).
    [[nodiscard]] IgnoreStack pushed(IgnoreRules rules) const;

    // The decision from the path's own patterns only (the deepest file with a matching rule decides).
    [[nodiscard]] IgnoreMatch match(std::string_view path, bool isDirectory) const;

    // As git decides for a walk: if a parent directory of `path` is ignored, that decides; otherwise
    // match(path, isDirectory). O(path components x match).
    [[nodiscard]] IgnoreMatch matchWithParents(std::string_view path, bool isDirectory) const;

    [[nodiscard]] bool empty() const noexcept { return m_top == nullptr; }

private:
    struct Frame;
    std::shared_ptr<const Frame> m_top;
};

} // namespace frost::io
