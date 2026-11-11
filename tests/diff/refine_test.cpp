// SPDX-License-Identifier: Apache-2.0
//
// Refinement tests (ADR 0019, D8): word and character token goldens (multibyte, invalid bytes,
// CRLF), minimality against the DP oracle over independent tokenisers (Word and Char), the apply
// property of the refined pairs, the windowed scan against the full scan (random bytes and runs of
// continuation bytes), a long line differing near both ends (the edge-only coarse path), trimming
// of a long line, and the coarse results at each limit.

#include "frost/diff/refine.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/support/checks.hpp"
#include "diff/support/generators.hpp"
#include "engine.hpp"
#include <gtest/gtest.h>

#include <map>

namespace frost::diff::test {
namespace {

Hunk wholeHunk(const LineSequence& a, const LineSequence& b) {
    return Hunk{LineRange{LineIndex{0}, LineIndex{a.size()}}, LineRange{LineIndex{0}, LineIndex{b.size()}}};
}

std::string pairsOf(const Refinement& refinement) {
    std::string out;
    for (const RangePair& pair : refinement.changes) {
        out += std::format("[{},{})->[{},{}) ", pair.a.begin.value(), pair.a.end.value(), pair.b.begin.value(),
                           pair.b.end.value());
    }
    if (refinement.coarse) {
        out += "coarse";
    }
    return out;
}

std::string refineWhole(std::string a, std::string b, Granularity granularity, const RefineLimits& limits = {}) {
    const LineSequence sa = makeSequence(std::move(a));
    const LineSequence sb = makeSequence(std::move(b));
    auto refined = refineHunk(sa, sb, wholeHunk(sa, sb), granularity, limits);
    EXPECT_TRUE(refined.has_value());
    return refined ? pairsOf(*refined) : std::string();
}

TEST(Refine, WordTokenGoldens) {
    EXPECT_EQ(refineWhole("int foo = 1;\n", "int bar = 1;\n", Granularity::Word), "[4,7)->[4,7) ");
    // Non-ASCII and invalid bytes are word characters: one token each word.
    EXPECT_EQ(refineWhole("caf\xC3\xA9 ok\n", "cafe ok\n", Granularity::Word), "[0,5)->[0,4) ");
    EXPECT_EQ(refineWhole("x\xFFy z\n", "x\xFEy z\n", Granularity::Word), "[0,3)->[0,3) ");
    // A line break is one token; CRLF vs LF.
    EXPECT_EQ(refineWhole("a\r\n", "a\n", Granularity::Word), "[1,3)->[1,2) ");
    // Blank runs are one token; an inserted word with its blank.
    EXPECT_EQ(refineWhole("a b\n", "a b b\n", Granularity::Word), "[3,3)->[3,5) ");
    EXPECT_EQ(refineWhole("f(x,  y)\n", "f(x, y)\n", Granularity::Word), "[4,6)->[4,5) ");
    // Two separate changes over two lines.
    EXPECT_EQ(refineWhole("alpha beta\ngamma\n", "alpha BETA\ngamma delta\n", Granularity::Word),
              "[6,10)->[6,10) [16,16)->[16,22) ");
    // Punctuation bytes are single tokens.
    EXPECT_EQ(refineWhole("a+b\n", "a-b\n", Granularity::Word), "[1,2)->[1,2) ");
}

TEST(Refine, CharTokenGoldens) {
    EXPECT_EQ(refineWhole("caf\xC3\xA9\n", "cafe\n", Granularity::Char), "[3,5)->[3,4) ");
    // A truncated three-byte sequence is two single-byte tokens; the euro sign is one.
    EXPECT_EQ(refineWhole("a\xE2\x82"
                          "b\n",
                          "a\xE2\x82\xAC"
                          "b\n",
                          Granularity::Char),
              "[1,3)->[1,4) ");
    // Surrogate code points (ED A0..BF) are ill-formed: three byte tokens.
    EXPECT_EQ(refineWhole("\xED\xA0\x80\n", "\xED\x9F\xBF\n", Granularity::Char), "[0,3)->[0,3) ");
    EXPECT_EQ(refineWhole("x\r\n", "x\r", Granularity::Char), "[1,3)->[1,2) ");
    EXPECT_EQ(refineWhole("kitten\n", "sitting\n", Granularity::Char), "[0,1)->[0,1) [4,5)->[4,5) [6,6)->[6,7) ");
}

// An independent tokeniser of the Word rule, for the DP oracle.
std::vector<std::string> wordTokens(std::string_view s) {
    const auto word = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
    };
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = i + 1;
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == '\r' && j < s.size() && s[j] == '\n') {
            j = i + 2;
        } else if (word(c)) {
            while (j < s.size() && word(static_cast<unsigned char>(s[j]))) {
                ++j;
            }
        } else if (c == ' ' || c == '\t') {
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) {
                ++j;
            }
        }
        tokens.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return tokens;
}

TEST(Refine, MinimalVsDp) {
    SplitMix64 rng(0xD1FF0030);
    static constexpr std::string_view kPieces[] = {"a", "b", "c", " ", "  ", "(", ")", ",", "\t"};
    for (std::uint32_t c = 0; c < scaled(5000, 2000, 800); ++c) {
        const auto make = [&] {
            std::string s;
            const std::uint64_t n = rng.below(30);
            for (std::uint64_t k = 0; k < n; ++k) {
                s += kPieces[rng.below(std::size(kPieces))];
            }
            return s + "\n";
        };
        const std::string a = make();
        const std::string b = make();
        const std::vector<std::string> ta = wordTokens(a);
        const std::vector<std::string> tb = wordTokens(b);
        if (ta.size() > 60 || tb.size() > 60) {
            continue;
        }
        std::vector<std::uint32_t> ia;
        std::vector<std::uint32_t> ib;
        naiveIds(ta, tb, ia, ib);
        const LineSequence sa = makeSequence(a);
        const LineSequence sb = makeSequence(b);
        auto refined = refineHunk(sa, sb, wholeHunk(sa, sb), Granularity::Word);
        ASSERT_TRUE(refined.has_value());
        ASSERT_FALSE(refined->coarse);
        std::uint64_t changed = 0;
        for (const RangePair& pair : refined->changes) {
            changed += wordTokens(a.substr(pair.a.begin.value(), pair.a.size())).size();
            changed += wordTokens(b.substr(pair.b.begin.value(), pair.b.size())).size();
        }
        ASSERT_EQ(changed, editDistance(ia, ib)) << printable(a) << " vs " << printable(b) << ": " << pairsOf(*refined);
        ASSERT_TRUE(refinedApplies(sa, sb, wholeHunk(sa, sb), *refined));
    }
}

// An independent tokeniser of the Char rule for well-formed UTF-8 without lone CRs: a CRLF, or
// one code point by its lead byte.
std::vector<std::string> charTokens(std::string_view s) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t length = 1;
        if ((c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') || (c >= 0xC0 && c < 0xE0)) {
            length = 2;
        } else if (c >= 0xF0) {
            length = 4;
        } else if (c >= 0xE0) {
            length = 3;
        }
        tokens.emplace_back(s.substr(i, length));
        i += length;
    }
    return tokens;
}

TEST(Refine, MinimalVsDpChar) {
    // Pieces that are one Char token each and never merge with a neighbour (no lone CR or LF), so
    // the oracle's token lists are the piece lists.
    SplitMix64 rng(0xD1FF0034);
    static constexpr std::string_view kPieces[] = {"a", "b", "c", " ", "\xC3\xA9", "\xE2\x82\xAC", "\r\n", "("};
    std::uint32_t checked = 0;
    for (std::uint32_t c = 0; c < scaled(5000, 2000, 800); ++c) {
        std::vector<std::string> ta;
        std::vector<std::string> tb;
        const auto make = [&](std::vector<std::string>& tokens) {
            std::string s;
            for (std::uint64_t n = rng.below(40); n > 0; --n) {
                tokens.emplace_back(kPieces[rng.below(std::size(kPieces))]);
                s += tokens.back();
            }
            tokens.emplace_back("\n");
            return s + "\n";
        };
        const std::string a = make(ta);
        const std::string b = make(tb);
        ASSERT_EQ(charTokens(a), ta);
        std::vector<std::uint32_t> ia;
        std::vector<std::uint32_t> ib;
        naiveIds(ta, tb, ia, ib);
        const LineSequence sa = makeSequence(a);
        const LineSequence sb = makeSequence(b);
        auto refined = refineHunk(sa, sb, wholeHunk(sa, sb), Granularity::Char);
        ASSERT_TRUE(refined.has_value());
        ASSERT_FALSE(refined->coarse);
        std::uint64_t changed = 0;
        for (const RangePair& pair : refined->changes) {
            changed += charTokens(a.substr(pair.a.begin.value(), pair.a.size())).size();
            changed += charTokens(b.substr(pair.b.begin.value(), pair.b.size())).size();
        }
        ASSERT_EQ(changed, editDistance(ia, ib)) << printable(a) << " vs " << printable(b) << ": " << pairsOf(*refined);
        ASSERT_TRUE(refinedApplies(sa, sb, wholeHunk(sa, sb), *refined));
        ++checked;
    }
    EXPECT_GT(checked, 0u);
}

TEST(Refine, ApplyRefinedTurnsOldIntoNew) {
    SplitMix64 rng(0xD1FF0031);
    std::uint32_t refinedHunks = 0;
    for (std::uint32_t c = 0; c < scaled(3000, 800, 400); ++c) {
        TextPair pair = generatePair(kAllPairClasses[c % std::size(kAllPairClasses)], rng);
        const LineSequence a = makeSequence(pair.a);
        const LineSequence b = makeSequence(pair.b);
        auto diff = diffLines(a, b);
        ASSERT_TRUE(diff.has_value());
        for (const Hunk& hunk : diff->hunks) {
            for (const Granularity granularity : {Granularity::Word, Granularity::Char}) {
                auto refined = refineHunk(a, b, hunk, granularity);
                ASSERT_TRUE(refined.has_value());
                ASSERT_TRUE(refinedApplies(a, b, hunk, *refined)) << printable(pair.a) << " vs " << printable(pair.b);
                ++refinedHunks;
            }
        }
    }
    EXPECT_GT(refinedHunks, 0u);
}

// The windowed scan (refine.cpp) gives exactly the full scan's result: random common prefixes and
// suffixes around random middles, over bytes that stress token boundaries at the window edges
// (word and blank runs, CR LF, UTF-8 leads, continuation and invalid bytes), both granularities,
// with default and tiny limits.
TEST(Refine, WindowEqualsFullScan) {
    SplitMix64 rng(0xD1FF0033);
    static constexpr char kBytes[] = {'a',    'b',    ' ',    '\t',   '\r',   '\n',   '(',    ',',    '\xC3', '\xA9',
                                      '\xE2', '\x82', '\xAC', '\x80', '\xBF', '\xFF', '\xED', '\xA0', '\xF0', '\x9F'};
    const auto randomBytes = [&](std::uint64_t maxLength) {
        std::string s;
        for (std::uint64_t n = rng.below(maxLength + 1); n > 0; --n) {
            s += kBytes[rng.below(std::size(kBytes))];
        }
        return s;
    };
    std::uint32_t compared = 0;
    for (std::uint32_t c = 0; c < scaled(40000, 8000, 3000); ++c) {
        const std::string head = randomBytes(24);
        const std::string tail = randomBytes(24);
        std::string a = head + randomBytes(6) + tail;
        std::string b = head + randomBytes(6) + tail;
        if (rng.below(4) == 0) {
            const std::uint64_t cut = rng.below(a.size() + 1);
            const std::string inserted = randomBytes(3);
            b = a.substr(0, cut) + inserted + a.substr(rng.below(a.size() + 1));
        }
        a += '\n';
        b += '\n';
        const LineSequence sa = makeSequence(a);
        const LineSequence sb = makeSequence(b);
        RefineLimits limits;
        if (rng.below(2) == 0) {
            // Tiny limits: middles over maxBytes take the edge-only coarse path.
            limits.maxBytes = static_cast<std::uint32_t>(rng.below(8));
            limits.maxWork = rng.below(64);
        }
        for (const Granularity granularity : {Granularity::Word, Granularity::Char}) {
            auto window =
                detail::refineHunkWith(sa, sb, wholeHunk(sa, sb), granularity, limits, {}, detail::RefineScan::Window);
            auto full = detail::refineHunkWith(sa, sb, wholeHunk(sa, sb), granularity, limits, {},
                                               detail::RefineScan::FullScan);
            ASSERT_TRUE(window.has_value() && full.has_value());
            ASSERT_EQ(pairsOf(*window), pairsOf(*full))
                << printable(a) << " vs " << printable(b) << (granularity == Granularity::Word ? " (Word)" : " (Char)");
            ++compared;
        }
    }
    EXPECT_GT(compared, 0u);
}

// Runs of continuation bytes (one Char token each) around a change, with a lead byte, an ASCII
// byte or a CR at every distance from the run's edges: certainStart's three-byte rule for
// continuation bytes (ADR 0019, D8) keeps the window and its edge-only coarse path equal to the
// full scan, at both window edges and both granularities.
TEST(Refine, ContinuationRunsWindow) {
    static constexpr std::string_view kInserts[] = {"", "\xC3", "\xE2", "\xF0", "\xED", "\xF4", "a", "\r"};
    static constexpr std::string_view kMiddles[] = {"x", "\xC3\xA9", "\xBF", "", "\xE2\x82", "\x80\x80\x80\x80"};
    RefineLimits tiny;
    tiny.maxBytes = 0;
    std::uint32_t compared = 0;
    for (std::size_t run = 0; run <= 6; ++run) {
        for (const std::string_view insert : kInserts) {
            for (std::size_t place = 0; place <= run; ++place) {
                std::string common(run, '\x80');
                common.insert(place, insert);
                for (const std::string_view middleA : kMiddles) {
                    for (const std::string_view middleB : kMiddles) {
                        const LineSequence a = makeSequence(common + std::string(middleA) + common + "\n");
                        const LineSequence b = makeSequence(common + std::string(middleB) + common + "\n");
                        for (const Granularity granularity : {Granularity::Char, Granularity::Word}) {
                            for (const RefineLimits& limits : {RefineLimits{}, tiny}) {
                                const auto agreed = checkRefineScansAgree(a, b, wholeHunk(a, b), granularity, limits);
                                ASSERT_TRUE(agreed.has_value())
                                    << printable(a.bytes()) << " vs " << printable(b.bytes()) << ": "
                                    << agreed.error().describe();
                                ++compared;
                            }
                        }
                    }
                }
            }
        }
    }
    EXPECT_GT(compared, 0u);
}

// A long line that differs near both ends: the middle exceeds maxBytes, and only its edges are
// tokenised. The result equals the full scan's: a coarse pair from the first to the last change,
// or a pure deletion when the shorter side has no middle.
TEST(Refine, CoarseAtBothEnds) {
    SplitMix64 rng(0xD1FF0035);
    std::string filler;
    while (filler.size() < 256 * 1024) {
        filler += std::format(" w{}", rng.below(1000));
        if (rng.below(16) == 0) {
            filler += "(\xC3\xA9,\t";
        }
    }
    struct Case {
        std::string a;
        std::string b;
        std::size_t begin; // Expected middle on A: [begin, a.size() - end).
        std::size_t end;
        std::size_t bBegin; // On B.
        std::size_t bEnd;
        bool coarse;
    };
    const std::vector<Case> cases = {
        // One word changed at each end.
        {"alpha" + filler + " omega\n", "ALPHA" + filler + " OMEGA\n", 0, 1, 0, 1, true},
        // Changes one token in from each end.
        {"x alpha" + filler + " omega y\n", "x beta" + filler + " gamma y\n", 2, 3, 2, 3, true},
        // The probe's shape: one byte at each end, the line break common.
        {"A" + filler + "A\n", "B" + filler + "B\n", 0, 1, 0, 1, true},
        // The middle deleted between common words: a pure deletion, not coarse. The common token
        // suffix stops at B's prefix ("tail\n"; the blank before it belongs to the prefix).
        {"head" + filler + " tail\n", "head tail\n", 5, 5, 5, 5, false},
    };
    for (const Case& item : cases) {
        const LineSequence a = makeSequence(item.a);
        const LineSequence b = makeSequence(item.b);
        for (const Granularity granularity : {Granularity::Word, Granularity::Char}) {
            auto refined = refineHunk(a, b, wholeHunk(a, b), granularity);
            ASSERT_TRUE(refined.has_value());
            const auto agreed = checkRefineScansAgree(a, b, wholeHunk(a, b), granularity, RefineLimits{});
            ASSERT_TRUE(agreed.has_value()) << agreed.error().describe();
            EXPECT_EQ(refined->coarse, item.coarse);
            ASSERT_EQ(refined->changes.size(), 1u);
            if (granularity == Granularity::Word) {
                EXPECT_EQ(refined->changes[0].a.begin.value(), item.begin);
                EXPECT_EQ(refined->changes[0].a.end.value(), item.a.size() - item.end);
                EXPECT_EQ(refined->changes[0].b.begin.value(), item.bBegin);
                EXPECT_EQ(refined->changes[0].b.end.value(), item.b.size() - item.bEnd);
            }
            EXPECT_TRUE(refinedApplies(a, b, wholeHunk(a, b), *refined));
        }
    }
}

TEST(Refine, TrimLongLine) {
    std::string line;
    SplitMix64 rng(0xD1FF0032);
    while (line.size() < (1u << 20)) {
        line += std::format("w{} ", rng.below(1000));
    }
    std::string changed = line;
    changed.replace(line.size() / 2, 1, "#");
    line += '\n';
    changed += '\n';
    const LineSequence a = makeSequence(line);
    const LineSequence b = makeSequence(changed);
    for (const Granularity granularity : {Granularity::Word, Granularity::Char}) {
        auto refined = refineHunk(a, b, wholeHunk(a, b), granularity);
        ASSERT_TRUE(refined.has_value());
        EXPECT_FALSE(refined->coarse);
        ASSERT_FALSE(refined->changes.empty());
        EXPECT_LE(refined->changes.front().a.size(), 16u);
        EXPECT_TRUE(refinedApplies(a, b, wholeHunk(a, b), *refined));
    }
}

TEST(Refine, CoarseOnLimits) {
    const std::string a = "one two three four five six\n";
    const std::string b = "one TWO three four FIVE six\n";
    // maxScanBytes: not tokenised at all; one pair over the whole hunk.
    RefineLimits scan;
    scan.maxScanBytes = 10;
    EXPECT_EQ(refineWhole(a, b, Granularity::Word, scan), "[0,28)->[0,28) coarse");
    // maxBytes after trimming: the middle "two three four five" is 19 bytes.
    RefineLimits bytes;
    bytes.maxBytes = 18;
    EXPECT_EQ(refineWhole(a, b, Granularity::Word, bytes), "[4,23)->[4,23) coarse");
    bytes.maxBytes = 19;
    EXPECT_EQ(refineWhole(a, b, Granularity::Word, bytes), "[4,7)->[4,7) [19,23)->[19,23) ");
    // maxWork: the token diff runs out.
    RefineLimits work;
    work.maxWork = 2;
    EXPECT_EQ(refineWhole(a, b, Granularity::Word, work), "[4,23)->[4,23) coarse");
    // A hunk with an empty side is one pair and never coarse.
    const LineSequence sa = makeSequence("x\n");
    const LineSequence sb = makeSequence("x\ny\n");
    auto insert = refineHunk(sa, sb, Hunk{LineRange{LineIndex{1}, LineIndex{1}}, LineRange{LineIndex{1}, LineIndex{2}}},
                             Granularity::Char, scan);
    ASSERT_TRUE(insert.has_value());
    EXPECT_EQ(pairsOf(*insert), "[2,2)->[2,4) ");
    // Cancellation.
    std::stop_source source;
    source.request_stop();
    auto cancelled = refineHunk(makeSequence(a), makeSequence(b), wholeHunk(makeSequence(a), makeSequence(b)),
                                Granularity::Word, {}, source.get_token());
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
}

} // namespace
} // namespace frost::diff::test
