// SPDX-License-Identifier: Apache-2.0
//
// The S1 text corpus: a large, deterministic, C++-like ASCII buffer, a tiny
// lexer that assigns each character a highlight class, the TextModel both spike
// editor views (raster and QRhi) draw, and the fling curve the harness scrolls
// along. Both views draw the same lines with the same classes, so their costs
// are comparable. Qt-free.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frost::s1 {

// Highlight classes; each has one colour in the theme.
enum class TokenClass : std::uint8_t { Text, Keyword, Type, Number, String, Comment, Punctuation, Preprocessor };
inline constexpr std::size_t kTokenClasses = 8;

// Generates `lineCount` lines of plausible C++ (nested blocks, comments, string
// literals, about 3 % long lines of 250 to 500 characters). The same seed always
// yields the same text. Lines contain printable ASCII and spaces only. O(n).
[[nodiscard]] std::vector<std::string> makeCorpus(std::size_t lineCount, std::uint64_t seed);

// Classifies every character of `line` into `out` (resized to line.size()).
// Single-line lexing: // comments, "..." strings, numbers, keywords, a few type
// names, # directives and punctuation; everything else is Text. O(line length).
void classifyLine(std::string_view line, std::vector<TokenClass>& out);

// The buffer both views draw: lines, their highlight classes (lexed once up front,
// so frame costs are rendering only) and a per-line revision that caches key on.
// Not thread-safe; the UI thread owns it.
class TextModel {
public:
    explicit TextModel(std::vector<std::string> lines);

    [[nodiscard]] std::size_t lineCount() const noexcept { return m_lines.size(); }
    // Precondition: line < lineCount().
    [[nodiscard]] std::string_view line(std::size_t line) const noexcept { return m_lines[line]; }
    [[nodiscard]] const std::vector<TokenClass>& classes(std::size_t line) const noexcept { return m_classes[line]; }
    [[nodiscard]] std::uint32_t revision(std::size_t line) const noexcept { return m_revisions[line]; }

    // Inserts `c` at `column` (clamped to the line length), relexes the line and
    // bumps its revision. Returns the column after the inserted character.
    // Precondition: line < lineCount(). O(line length).
    std::size_t insert(std::size_t line, std::size_t column, char c);

private:
    std::vector<std::string> m_lines;
    std::vector<std::vector<TokenClass>> m_classes;
    std::vector<std::uint32_t> m_revisions;
};

// Kinetic scroll offset of the S1 fling: a release at `velocityPxPerS` decaying
// exponentially with time constant `tauS`, the usual model for touchpad and
// wheel flings. offsetPx(t) = v0 * tau * (1 - exp(-t / tau)).
struct Fling {
    double velocityPxPerS = 24000.0;
    double tauS = 3.0;

    [[nodiscard]] double offsetPx(double seconds) const noexcept;
    [[nodiscard]] double velocityAt(double seconds) const noexcept;
};

} // namespace frost::s1
