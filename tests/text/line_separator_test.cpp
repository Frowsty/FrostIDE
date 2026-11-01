// SPDX-License-Identifier: Apache-2.0
//
// LineSeparators: separator names and bytes, the LineBreak conversions, line-break counts of
// strings and snapshots against a linear-scan oracle, and the dominant-separator tie rules
// (line_separator.hpp; ADR 0018, D9).

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/text_buffer.hpp"

#include "text/support/encoding_checks.hpp"
#include "text/support/encoding_oracle.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

using frost::sched::detail::SplitMix64;
using frost::text::dominantSeparator;
using frost::text::EolStats;
using frost::text::eolStats;
using frost::text::LineBreak;
using frost::text::LineSeparator;
using frost::text::test::oracleEolStats;
using frost::text::test::reproduceSeed;

constexpr std::string_view kExe = "text_encoding_tests";

static_assert(frost::text::separatorBytes(LineSeparator::CrLf) == "\r\n");
static_assert(frost::text::lineBreakOf(LineSeparator::Cr) == LineBreak::Cr);
static_assert(!frost::text::separatorOf(LineBreak::None).has_value());

TEST(LineSeparators, Names) {
    EXPECT_EQ(frost::text::separatorName(LineSeparator::Lf), "LF");
    EXPECT_EQ(frost::text::separatorName(LineSeparator::CrLf), "CRLF");
    EXPECT_EQ(frost::text::separatorName(LineSeparator::Cr), "CR");
}

TEST(LineSeparators, FindSeparator) {
    EXPECT_EQ(frost::text::findSeparator("lf"), LineSeparator::Lf);
    EXPECT_EQ(frost::text::findSeparator("CRLF"), LineSeparator::CrLf);
    EXPECT_EQ(frost::text::findSeparator("CrLf"), LineSeparator::CrLf);
    EXPECT_EQ(frost::text::findSeparator("cr"), LineSeparator::Cr);
    for (const std::string_view bad : {"", "crlf ", "\n", "lfcr", "nl", "auto"}) {
        EXPECT_EQ(frost::text::findSeparator(bad), std::nullopt) << bad;
    }
}

TEST(LineSeparators, SeparatorBytes) {
    EXPECT_EQ(frost::text::separatorBytes(LineSeparator::Lf), "\n");
    EXPECT_EQ(frost::text::separatorBytes(LineSeparator::CrLf), "\r\n");
    EXPECT_EQ(frost::text::separatorBytes(LineSeparator::Cr), "\r");
}

TEST(LineSeparators, LineBreakConversions) {
    for (const LineSeparator s : {LineSeparator::Lf, LineSeparator::CrLf, LineSeparator::Cr}) {
        EXPECT_EQ(frost::text::separatorOf(frost::text::lineBreakOf(s)), s);
    }
    EXPECT_EQ(frost::text::separatorOf(LineBreak::Lf), LineSeparator::Lf);
    EXPECT_EQ(frost::text::separatorOf(LineBreak::CrLf), LineSeparator::CrLf);
    EXPECT_EQ(frost::text::separatorOf(LineBreak::None), std::nullopt);
}

// Text with every line-break shape: "\r\r\n", "\n\r", a trailing CR, only CRs.
std::string randomBreaks(SplitMix64& rng, std::size_t maxBytes) {
    std::string out;
    const std::size_t n = static_cast<std::size_t>(rng.below(maxBytes + 1));
    while (out.size() < n) {
        switch (rng.below(6)) {
        case 0: out.push_back('\r'); break;
        case 1: out.push_back('\n'); break;
        case 2: out.append("\r\n"); break;
        case 3: out.append(rng.below(2) == 0 ? "\r\r\n" : "\n\r"); break;
        case 4: out.append("\xC3\xA9"); break;
        default: out.push_back(static_cast<char>('a' + rng.below(26))); break;
        }
    }
    return out;
}

TEST(LineSeparators, CountsMatchOracle) {
    for (const std::string_view fixed : {"", "\r", "\n", "\r\n", "\r\r\n", "\n\r", "a\r", "\r\r\r", "\n\n\r\n\r"}) {
        EXPECT_EQ(eolStats(fixed), oracleEolStats(fixed)) << fixed.size();
    }
    EXPECT_EQ(eolStats("\r\r\n"), (EolStats{0, 1, 1}));
    EXPECT_EQ(eolStats("\n\r"), (EolStats{1, 0, 1}));
    EXPECT_EQ(eolStats("a\r"), (EolStats{0, 0, 1}));
    const auto seeds = frost::text::test::encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 500);
        for (int input = 0; input < 8; ++input) {
            const std::string text = randomBreaks(rng, 300);
            ASSERT_EQ(eolStats(text), oracleEolStats(text))
                << reproduceSeed(kExe, "LineSeparators.CountsMatchOracle", seed);
        }
    }
}

TEST(LineSeparators, SnapshotMatchesString) {
    const auto seeds = frost::text::test::encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 900);
        const std::string text = randomBreaks(rng, 200);
        for (const std::uint32_t piece : {4u, 5u, 7u}) {
            frost::text::TextBufferOptions options;
            options.maxPieceBytes = piece;
            auto buffer = frost::text::TextBuffer::fromUtf8(text, options);
            ASSERT_TRUE(buffer.has_value());
            // A few edits so that pieces come from several storages, with CRLFs across them.
            for (int edit = 0; edit < 4; ++edit) {
                const std::uint64_t size = buffer->current().size().value();
                std::uint64_t at = rng.below(size + 1);
                while (!buffer->current().isCharBoundary(frost::ByteOffset{at})) {
                    --at;
                }
                auto inserted = buffer->insert(frost::ByteOffset{at}, rng.below(2) == 0 ? "\n" : "\r");
                ASSERT_TRUE(inserted.has_value());
            }
            const std::string now = buffer->current().text();
            ASSERT_EQ(eolStats(buffer->current()), oracleEolStats(now))
                << "piece " << piece << "; " << reproduceSeed(kExe, "LineSeparators.SnapshotMatchesString", seed);
            ASSERT_EQ(eolStats(buffer->current()), eolStats(now));
        }
    }
}

TEST(LineSeparators, DominantTieRules) {
    // A 1:1 tie with the fallback among the largest gives the fallback.
    EXPECT_EQ(dominantSeparator({1, 1, 0}, LineSeparator::Lf), LineSeparator::Lf);
    EXPECT_EQ(dominantSeparator({1, 1, 0}, LineSeparator::CrLf), LineSeparator::CrLf);
    // A tie without the fallback gives the order Lf, CrLf, Cr.
    EXPECT_EQ(dominantSeparator({1, 1, 0}, LineSeparator::Cr), LineSeparator::Lf);
    EXPECT_EQ(dominantSeparator({0, 2, 2}, LineSeparator::Lf), LineSeparator::CrLf);
    // No breaks: the fallback.
    EXPECT_EQ(dominantSeparator({0, 0, 0}, LineSeparator::Cr), LineSeparator::Cr);
    // The largest count wins.
    EXPECT_EQ(dominantSeparator({1, 3, 1}, LineSeparator::Lf), LineSeparator::CrLf);
    EXPECT_EQ(dominantSeparator({0, 0, 1}, LineSeparator::CrLf), LineSeparator::Cr);
}

TEST(LineSeparators, Mixed) {
    EXPECT_FALSE((EolStats{}).mixed());
    EXPECT_FALSE((EolStats{5, 0, 0}).mixed());
    EXPECT_TRUE((EolStats{1, 1, 0}).mixed());
    EXPECT_TRUE((EolStats{0, 1, 1}).mixed());
    EXPECT_TRUE((EolStats{1, 3, 1}).mixed());
    EXPECT_EQ((EolStats{1, 3, 1}).total(), 5u);
    EXPECT_TRUE(eolStats("a\r\nb\r\nc\nd\re\r\n").mixed());
}

} // namespace
