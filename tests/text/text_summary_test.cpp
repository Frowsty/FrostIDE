// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::text::TextSummary: summarize() against the oracle's independent summary, the
// monoid laws of combine(), split invariance (exhaustive over short strings) and the CRLF, longest
// line and lone-CR rules (ADR 0012, D1 and D2).

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/text_summary.hpp"

#include "text/support/text_oracle.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::sched::detail::SplitMix64;
using frost::text::combine;
using frost::text::summarize;
using frost::text::TextSummary;
using frost::text::test::oracleSummary;

// A readable dump for failure messages.
std::string dump(const TextSummary& s) {
    return std::format("{{bytes {}, utf16 {}, chars {}, breaks {}, lf {}, first {}, last {}/{}/{}, longest {}@{}, "
                       "startsLf {}, endsCr {}}}",
                       s.bytes, s.utf16, s.chars, s.lineBreaks, s.lineFeeds, s.firstLineChars, s.lastLineChars,
                       s.lastLineBytes, s.lastLineUtf16, s.longestLineChars, s.longestLine, s.startsWithLf,
                       s.endsWithCr);
}

std::string escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto b = static_cast<unsigned char>(c);
        if (b >= 0x20 && b < 0x7F) {
            out += c;
        } else {
            out += std::format("\\x{:02X}", b);
        }
    }
    return out;
}

constexpr std::string_view kAlphabet[] = {
    "a",
    "b",
    " ",
    "\t",
    "\r",
    "\n",
    "\r\n",
    std::string_view("\0", 1),
    "\xC3\xA9",
    "\xC2\x80",
    "\xDF\xBF",
    "\xE0\xA0\x80",
    "\xE2\x82\xAC",
    "\xEF\xBF\xBF",
    "\xF0\x90\x80\x80",
    "\xF0\x9F\x98\x80",
    "\xF4\x8F\xBF\xBF",
};

std::string randomText(SplitMix64& rng, std::size_t tokens) {
    std::string text;
    for (std::size_t i = 0; i < tokens; ++i) {
        text += kAlphabet[rng.below(std::size(kAlphabet))];
    }
    return text;
}

// Every string of up to `maxTokens` tokens over {a, é, 😀, CR, LF}, with its token boundaries.
struct Exhaustive {
    std::string text;
    std::vector<std::size_t> boundaries; // Char boundaries, including 0 and text.size().
};

std::vector<Exhaustive> exhaustiveStrings(int maxTokens) {
    constexpr std::string_view kTokens[] = {"a", "\xC3\xA9", "\xF0\x9F\x98\x80", "\r", "\n"};
    std::vector<Exhaustive> out;
    std::vector<int> digits;
    for (int length = 0; length <= maxTokens; ++length) {
        digits.assign(static_cast<std::size_t>(length), 0);
        while (true) {
            Exhaustive e;
            e.boundaries.push_back(0);
            for (const int d : digits) {
                e.text += kTokens[d];
                e.boundaries.push_back(e.text.size());
            }
            out.push_back(std::move(e));
            std::size_t i = 0;
            while (i < digits.size() && ++digits[i] == 5) {
                digits[i++] = 0;
            }
            if (i == digits.size()) {
                break;
            }
        }
    }
    return out;
}

TEST(TextSummary, MatchesOracle) {
    SplitMix64 rng(1);
    for (int i = 0; i < 2000; ++i) {
        const std::string text = randomText(rng, static_cast<std::size_t>(rng.below(64)));
        const TextSummary got = summarize(text);
        const TextSummary expected = oracleSummary(text);
        ASSERT_EQ(got, expected) << "\"" << escape(text) << "\"\n got " << dump(got) << "\n expected "
                                 << dump(expected);
    }
    // Long ASCII and mixed runs exercise the 8-byte fast path and its tails.
    for (std::size_t n = 0; n < 40; ++n) {
        const std::string ascii(n, 'x');
        ASSERT_EQ(summarize(ascii), oracleSummary(ascii)) << n;
        const std::string mixed = ascii + "\xF0\x9F\x98\x80" + ascii + "\r\n" + ascii + "\xC3\xA9";
        ASSERT_EQ(summarize(mixed), oracleSummary(mixed)) << n;
    }
}

TEST(TextSummary, EmptyIsIdentity) {
    const TextSummary empty{};
    EXPECT_EQ(summarize(""), empty);
    SplitMix64 rng(2);
    for (int i = 0; i < 200; ++i) {
        const TextSummary s = summarize(randomText(rng, static_cast<std::size_t>(rng.below(20))));
        EXPECT_EQ(combine(empty, s), s);
        EXPECT_EQ(combine(s, empty), s);
    }
}

TEST(TextSummary, CombineIsAssociative) {
    SplitMix64 rng(3);
    for (int i = 0; i < 3000; ++i) {
        const TextSummary a = summarize(randomText(rng, static_cast<std::size_t>(rng.below(8))));
        const TextSummary b = summarize(randomText(rng, static_cast<std::size_t>(rng.below(8))));
        const TextSummary c = summarize(randomText(rng, static_cast<std::size_t>(rng.below(8))));
        ASSERT_EQ(combine(combine(a, b), c), combine(a, combine(b, c))) << i;
    }
    // Every 3-way split of the exhaustive strings at char boundaries.
    for (const Exhaustive& e : exhaustiveStrings(6)) {
        const std::string_view t = e.text;
        const TextSummary whole = summarize(t);
        for (std::size_t i = 0; i < e.boundaries.size(); ++i) {
            for (std::size_t j = i; j < e.boundaries.size(); ++j) {
                const std::size_t x = e.boundaries[i];
                const std::size_t y = e.boundaries[j];
                const TextSummary a = summarize(t.substr(0, x));
                const TextSummary b = summarize(t.substr(x, y - x));
                const TextSummary c = summarize(t.substr(y));
                const TextSummary left = combine(combine(a, b), c);
                const TextSummary right = combine(a, combine(b, c));
                if (!(left == whole) || !(right == whole)) {
                    FAIL() << "\"" << escape(t) << "\" split at " << x << ", " << y << ": " << dump(left) << " / "
                           << dump(right) << " vs " << dump(whole);
                }
            }
        }
    }
}

TEST(TextSummary, SplitInvariantExhaustive) {
    // Every byte position, including those inside a character: the counting rules are per byte.
    for (const Exhaustive& e : exhaustiveStrings(6)) {
        const std::string_view t = e.text;
        const TextSummary whole = summarize(t);
        if (e.boundaries.size() <= 4) {
            ASSERT_EQ(whole, oracleSummary(t)) << "\"" << escape(t) << "\"";
        }
        for (std::size_t x = 0; x <= t.size(); ++x) {
            const TextSummary joined = combine(summarize(t.substr(0, x)), summarize(t.substr(x)));
            if (!(joined == whole)) {
                FAIL() << "\"" << escape(t) << "\" split at " << x << ": " << dump(joined) << " vs " << dump(whole);
            }
        }
    }
}

TEST(TextSummary, CrLfAcrossBoundary) {
    const TextSummary cr = summarize("ab\r");
    const TextSummary lf = summarize("\ncd");
    EXPECT_TRUE(cr.endsWithCr);
    EXPECT_TRUE(lf.startsWithLf);
    const TextSummary joined = combine(cr, lf);
    EXPECT_EQ(joined, summarize("ab\r\ncd"));
    EXPECT_EQ(joined.lineBreaks, 1u);
    EXPECT_EQ(joined.lineFeeds, 1u);
    EXPECT_EQ(joined.firstLineChars, 2u);
    EXPECT_EQ(joined.lastLineChars, 2u);
    // Bare CR and LF pieces join to one break too, and the empty gap is not a row.
    const TextSummary pair = combine(summarize("\r"), summarize("\n"));
    EXPECT_EQ(pair, summarize("\r\n"));
    EXPECT_EQ(pair.lineBreaks, 1u);
    EXPECT_EQ(pair.longestLineChars, 0u);
    // A longer line after the CRLF keeps its row number.
    const TextSummary longest = combine(summarize("a\r"), summarize("\nxyz"));
    EXPECT_EQ(longest.longestLine, 1u);
    EXPECT_EQ(longest.longestLineChars, 3u);
    // LF then CR is two breaks.
    EXPECT_EQ(combine(summarize("\n"), summarize("\r")).lineBreaks, 2u);
}

TEST(TextSummary, LongestLineTiesPickFirstRow) {
    EXPECT_EQ(summarize("abc\nxyz\nab").longestLine, 0u);
    EXPECT_EQ(summarize("ab\nxyz\nabc").longestLine, 1u);
    // Ties across a combine: the left row wins, also across a joined row.
    const TextSummary s = combine(summarize("ab\ncd"), summarize("\nabcd\nx"));
    EXPECT_EQ(s.longestLineChars, 4u);
    EXPECT_EQ(s.longestLine, 2u);
    const TextSummary joinedTie = combine(summarize("ab\nc"), summarize("d"));
    EXPECT_EQ(joinedTie.longestLine, 0u);
    EXPECT_EQ(joinedTie.longestLineChars, 2u);
    // Characters, not bytes: "é" (2 bytes) is one character.
    const TextSummary chars = summarize("\xC3\xA9\xC3\xA9\nabc");
    EXPECT_EQ(chars.longestLine, 1u);
    EXPECT_EQ(chars.longestLineChars, 3u);
}

TEST(TextSummary, LineFeedsCountLoneCrs) {
    const TextSummary s = summarize("a\rb\r\nc\nd\r");
    EXPECT_EQ(s.lineBreaks, 4u);
    EXPECT_EQ(s.lineFeeds, 2u);
    EXPECT_EQ(s.lineBreaks - s.lineFeeds, 2u) << "two lone CRs";
    EXPECT_TRUE(s.endsWithCr);
    EXPECT_FALSE(s.startsWithLf);
    EXPECT_EQ(summarize("\r\r\n").lineBreaks, 2u);
    EXPECT_EQ(summarize("\r\r\n").lineFeeds, 1u);
}

TEST(TextSummary, Utf16AndChars) {
    const TextSummary s = summarize("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    EXPECT_EQ(s.bytes, 10u);
    EXPECT_EQ(s.chars, 4u);
    EXPECT_EQ(s.utf16, 5u);
    EXPECT_EQ(s.lastLineUtf16, 5u);
    EXPECT_EQ(s.lastLineBytes, 10u);
}

} // namespace
