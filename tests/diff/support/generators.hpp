// SPDX-License-Identifier: Apache-2.0
//
// Seeded input generators for the frost::diff tests (ADR 0019, D16): line-pair classes modelled on
// the black-box experiments that established git parity, the pathological shapes P1-P7 of the work
// cap, and constructive three-way merges with a known ground truth. Deterministic (SplitMix64);
// no gtest, so the fuzz target can link them.
#pragma once

#include "frost/sched/detail/split_mix64.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frost::diff::test {

using frost::sched::detail::SplitMix64;

// Small: at most 2000 lines per side (unit tests, fuzz-like properties). Large: stress and perf.
enum class SizeClass : std::uint8_t { Small, Large };

struct TextPair {
    std::string a;
    std::string b;
};

// The line-pair classes. Every class except Pathological honours the size class.
enum class PairClass : std::uint8_t {
    Adversarial,    // "L<d>\n" lines over 2-12 symbols, few edits: the histogram's corner cases.
    MidFrequency,   // 90 % one line, the rest from 5 others: chains near the 64-repeat limit.
    SparseFallback, // 97 % one line ("x"), else "z": mostly fallback regions.
    DenseFallback,  // Two lines 50/50: dense fallback regions (large Myers D).
    CodeLike,       // Code-ish lines: braces, blank lines, returns, assignments; block moves.
    LoneCr,         // Lines with lone CRs, CRLF, non-ASCII and the odd invalid byte; no-final-newline cases.
};

inline constexpr PairClass kAllPairClasses[] = {PairClass::Adversarial,    PairClass::MidFrequency,
                                                PairClass::SparseFallback, PairClass::DenseFallback,
                                                PairClass::CodeLike,       PairClass::LoneCr};

// The class's name, for messages ("Adversarial").
[[nodiscard]] std::string_view pairClassName(PairClass kind) noexcept;

/*
 * One generated pair of class `kind`, drawing from `rng`. Small sizes follow the black-box
 * experiments (Adversarial: <= 30 lines and <= 6 edits; MidFrequency and SparseFallback: 70-160
 * lines; DenseFallback: 140-200; CodeLike: <= 300; LoneCr: <= 60); Large ones scale lines and edits
 * up (to 2000 lines for Small-like classes in Large mode, CodeLike to 10k).
 */
[[nodiscard]] TextPair generatePair(PairClass kind, SplitMix64& rng, SizeClass size = SizeClass::Small);

// A code-like text of `lines` lines (the CodeLike line mix), and a copy with `edits` random edits
// (deletes, inserts, replaces and block moves) applied. For perf tests.
[[nodiscard]] TextPair codeLikePair(SplitMix64& rng, std::uint32_t lines, std::uint32_t edits);

// The pathological shapes of ADR 0019, D15, at `lines` lines (P6 and P7 use `lines` as the size of
// their one line in KiB: 8192 in perf runs). P7 puts its two changes 1/8 of the line apart.
enum class Pathology : std::uint8_t {
    P1, // All-unique lines vs the reversed order.
    P2, // Two-symbol random vs mutated (a large Myers D).
    P3, // No common lines.
    P4, // N identical lines vs the same with 10 scattered insertions of that line.
    P5, // A block of lines / 100 lines repeated 100 times vs a shuffle of the blocks.
    P6, // One long line with one word changed in the middle.
    P7, // One long line with two changes 1/8 of the line apart.
};

inline constexpr Pathology kAllPathologies[] = {Pathology::P1, Pathology::P2, Pathology::P3, Pathology::P4,
                                                Pathology::P5, Pathology::P6, Pathology::P7};

[[nodiscard]] std::string_view pathologyName(Pathology kind) noexcept;
[[nodiscard]] TextPair pathologicalPair(Pathology kind, SplitMix64& rng, std::uint32_t lines);

/*
 * A constructive three-way merge (Merge3Constructive): base has unique lines "b<i>\n"; ours and
 * theirs apply known edit sets (delete, insert, replace) whose new lines are fresh and unique
 * across all three texts ("o<k>\n", "t<k>\n"). Edits of one side are separated by at least one
 * base line that side keeps. Because every common line is unique and in order, every diff
 * algorithm finds the same matching, so the expected chunks follow from the edit sets alone.
 */
struct ConstructiveMerge {
    std::string base;
    std::string ours;
    std::string theirs;

    // One expected chunk, in line coordinates of the three texts. kind: 0 Stable, 1 Ours, 2 Theirs,
    // 3 Conflict, 4 Both (fresh lines are unique, so Both arises only from the same pure deletion
    // on both sides).
    struct Chunk {
        int kind = 0;
        std::uint32_t base0 = 0, base1 = 0, ours0 = 0, ours1 = 0, theirs0 = 0, theirs1 = 0;
    };
    std::vector<Chunk> expected;
    std::uint32_t conflicts = 0;
    std::string cleanText; // The merged text when conflicts == 0.
};

[[nodiscard]] ConstructiveMerge constructiveMerge(SplitMix64& rng, SizeClass size = SizeClass::Small);

// A random triple for merge invariants: base from one of the pair classes, ours and theirs edited
// copies of it (they share edits now and then, so Both chunks occur).
struct TextTriple {
    std::string base;
    std::string ours;
    std::string theirs;
};
[[nodiscard]] TextTriple randomTriple(SplitMix64& rng, SizeClass size = SizeClass::Small);

} // namespace frost::diff::test
