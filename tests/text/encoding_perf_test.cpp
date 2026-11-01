// SPDX-License-Identifier: Apache-2.0
//
// Micro-benchmarks for frost::text's encodings (label `perf`; ADR 0018, D15 and D17). The perf
// label runs in every -Quick, so Debug and sanitizer builds use small sizes and only report;
// optimised unsanitised builds (kAssertBudgets) run the full sizes and assert.
/*
 * EncodingPerf.Utf8LoadOverhead (D15, a sub-budget of B15 recorded in ADR 0018): a 64 MiB
 * code-like text (1% non-ASCII, CRLF and LF mixed; 4 MiB unless kAssertBudgets), the minimum of 5
 * runs of TextBuffer::fromUtf8(copy) against decodeTextFile(copy), copies made outside the timed
 * region. Asserts decodeTextFile <= 1.5 x fromUtf8. If it fails, add a single-validation path;
 * the bound is not to be loosened. Budget: B15.
 * EncodingPerf.DetectBounded: detectEncoding on 256 MiB (16 MiB) of code-like UTF-8 and of
 * code-like windows-1252 text, each at most 1 ms (the B15 sample bound); a dense-junk sample with
 * legacy 932 is reported only. Budget: B15.
 * EncodingPerf.DecodeThroughput: MB/s of UTF-8 lossy, UTF-16LE, windows-1252 and Shift_JIS
 * decoding, report only.
 * EncodingPerf.DenseInvalidDecode (D17): 4 MiB of 0xFD (invalid in 932) through decodeLegacy
 * with the platform 932 codec; asserts at least 2 MB/s (a test bound, not a budgets.json budget).
 */

#include "frost/io/utf.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/encoding_detection.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/text/text_codec.hpp"
#include "frost/text/text_file.hpp"
#include "frost/text/unicode_codec.hpp"

#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

using frost::sched::detail::SplitMix64;
using frost::test::kAssertBudgets;
using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

// Code-like UTF-8: C++ tokens and comments, 1% non-ASCII characters, LF and CRLF lines.
std::string codeLikeUtf8(std::size_t bytes, std::uint64_t seed) {
    constexpr std::string_view kTokens[] = {"int",  " ",      "value", "=",  "0;",    "return", "(",  ")",
                                            "{",    "}",      "//",    " x", "std::", "string", "->", "+",
                                            "for ", "while ", "if ",   "42", "auto ", "const ", "&",  ","};
    constexpr std::string_view kNonAscii[] = {"\xC3\xA9", "\xE2\x82\xAC", "\xE6\x97\xA5", "\xF0\x9F\x98\x80"};
    SplitMix64 rng(seed);
    std::string out;
    out.reserve(bytes + 64);
    std::size_t column = 0;
    while (out.size() < bytes) {
        if (rng.below(100) == 0) {
            out += kNonAscii[rng.below(std::size(kNonAscii))];
        } else {
            out += kTokens[rng.below(std::size(kTokens))];
        }
        if (++column == 12) {
            out += rng.below(2) == 0 ? "\n" : "\r\n";
            column = 0;
        }
    }
    // End on a character boundary.
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
        out.pop_back();
    }
    if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) {
        out.pop_back();
    }
    return out;
}

// Code-like windows-1252: the same with 1% bytes 0xE9/0x80/0xFC (not valid UTF-8).
std::string codeLike1252(std::size_t bytes, std::uint64_t seed) {
    std::string out = codeLikeUtf8(bytes, seed);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            c = 'e';
        }
    }
    SplitMix64 rng(seed + 1);
    for (std::size_t i = 0; i < out.size(); i += 100) {
        constexpr char kHigh[] = {'\xE9', '\x80', '\xFC'};
        out[i] = kHigh[rng.below(3)];
    }
    return out;
}

frost::text::LoadOptions loadOptions() {
    frost::text::LoadOptions o;
    o.detect.legacyCodePage = 1252;
    o.defaultSeparator = frost::text::LineSeparator::Lf;
    return o;
}

TEST(EncodingPerf, Utf8LoadOverhead) {
    const std::size_t size = kAssertBudgets ? 64u << 20 : 4u << 20;
    const std::string text = codeLikeUtf8(size, 1);
    double bestBuffer = 1e9;
    double bestDecode = 1e9;
    for (int run = 0; run < 5; ++run) {
        {
            std::string copy = text;
            const auto start = Clock::now();
            auto buffer = frost::text::TextBuffer::fromUtf8(std::move(copy));
            const double s = secondsSince(start);
            ASSERT_TRUE(buffer.has_value());
            bestBuffer = std::min(bestBuffer, s);
        }
        {
            std::string copy = text;
            const auto start = Clock::now();
            auto loaded = frost::text::decodeTextFile(std::move(copy), loadOptions());
            const double s = secondsSince(start);
            ASSERT_TRUE(loaded.has_value());
            ASSERT_EQ(loaded->encoding.format.encoding, frost::text::Encoding::utf8());
            bestDecode = std::min(bestDecode, s);
        }
    }
    // The overhead's parts, for ADR 0018: one validation pass and the line-break count.
    double bestValidate = 1e9;
    double bestEol = 1e9;
    for (int run = 0; run < 5; ++run) {
        auto start = Clock::now();
        const std::size_t valid = frost::io::validUtf8Prefix(text);
        bestValidate = std::min(bestValidate, secondsSince(start));
        ASSERT_EQ(valid, text.size());
        start = Clock::now();
        const frost::text::EolStats eol = frost::text::eolStats(text);
        bestEol = std::min(bestEol, secondsSince(start));
        ASSERT_GT(eol.total(), 0u);
    }
    const double ratio = bestDecode / bestBuffer;
    std::printf("[encoding perf] Utf8LoadOverhead (%s, %zu bytes): fromUtf8 %.2f ms, decodeTextFile %.2f ms, ratio "
                "%.3f (bound 1.5); validation %.2f ms, eolStats %.2f ms\n",
                kAssertBudgets ? "budgeted" : "report-only", text.size(), bestBuffer * 1e3, bestDecode * 1e3, ratio,
                bestValidate * 1e3, bestEol * 1e3);
    if (kAssertBudgets) {
        EXPECT_LE(ratio, 1.5) << "D15: add a single-validation path; do not loosen the bound";
    }
}

double timeDetect(std::string_view bytes, std::uint16_t legacy) {
    frost::text::DetectOptions o;
    o.legacyCodePage = legacy;
    double best = 1e9;
    for (int run = 0; run < 5; ++run) {
        const auto start = Clock::now();
        const auto d = frost::text::detectEncoding(bytes, o, frost::text::platformCodecs());
        best = std::min(best, secondsSince(start));
        EXPECT_FALSE(d.candidates.empty());
    }
    return best;
}

TEST(EncodingPerf, DetectBounded) {
    constexpr std::size_t size = kAssertBudgets ? 256u << 20 : 16u << 20;
    // One 1 MiB block repeated: detection reads only the sample, so the rest need not vary.
    const auto big = [](const std::string& block) {
        std::string out;
        out.reserve(size + block.size());
        while (out.size() < size) {
            out += block;
        }
        return out;
    };
    const double utf8 = timeDetect(big(codeLikeUtf8(1u << 20, 2)), 1252);
    const double cp1252 = timeDetect(big(codeLike1252(1u << 20, 3)), 1252);
    std::string junk(1u << 20, '\0');
    SplitMix64 rng(4);
    for (char& c : junk) {
        c = static_cast<char>(0x80 + rng.below(0x80));
    }
    const double dense = timeDetect(junk, 932);
    std::printf("[encoding perf] DetectBounded (%s, %zu bytes): UTF-8 %.3f ms, windows-1252 %.3f ms (bound 1 ms each); "
                "dense junk with legacy 932 %.3f ms (report only)\n",
                kAssertBudgets ? "budgeted" : "report-only", size, utf8 * 1e3, cp1252 * 1e3, dense * 1e3);
    if (kAssertBudgets) {
        EXPECT_LE(utf8, 1e-3);
        EXPECT_LE(cp1252, 1e-3);
    }
}

// MB/s of one decode function over `bytes` (best of 3).
template<class Fn>
double throughput(std::string_view bytes, Fn&& decode) {
    double best = 1e9;
    for (int run = 0; run < 3; ++run) {
        const auto start = Clock::now();
        decode(bytes);
        best = std::min(best, secondsSince(start));
    }
    return static_cast<double>(bytes.size()) / 1e6 / best;
}

TEST(EncodingPerf, DecodeThroughput) {
    const std::size_t size = kAssertBudgets ? 32u << 20 : 2u << 20;
    const std::string utf8 = codeLikeUtf8(size, 5);
    std::string utf16;
    frost::text::appendUtf16(utf8, frost::text::Endian::Little, utf16);
    const std::string cp1252 = codeLike1252(size, 6);
    std::string sjis;
    {
        constexpr std::string_view kUnits[] = {"\x93\xFA", "\x96\x7B",   "\x8C\xEA", "\x82\xCC",
                                               "\xB6",     "int x = 0;", "\r\n"};
        SplitMix64 rng(7);
        while (sjis.size() < size) {
            sjis += kUnits[rng.below(std::size(kUnits))];
        }
    }
    const double mbUtf8 = throughput(utf8, [](std::string_view b) {
        std::string out;
        frost::text::DecodeStats s;
        (void)frost::text::appendUtf8Lossy(b, out, s);
    });
    const double mbUtf16 = throughput(utf16, [](std::string_view b) {
        std::string out;
        frost::text::DecodeStats s;
        (void)frost::text::appendUtf16Lossy(b, frost::text::Endian::Little, out, s);
    });
    const auto legacy = [](std::uint16_t cp) {
        return [cp](std::string_view b) {
            auto codec = frost::text::platformCodecs().open(cp);
            ASSERT_TRUE(codec.has_value());
            std::string out;
            frost::text::DecodeStats s;
            ASSERT_TRUE(frost::text::decodeLegacy(**codec, b, out, s).has_value());
        };
    };
    const double mb1252 = throughput(cp1252, legacy(1252));
    const double mb932 = throughput(sjis, legacy(932));
    std::printf("[encoding perf] DecodeThroughput (%zu bytes): UTF-8 lossy %.0f MB/s, UTF-16LE %.0f MB/s, "
                "windows-1252 %.0f MB/s, Shift_JIS %.0f MB/s (report only)\n",
                size, mbUtf8, mbUtf16, mb1252, mb932);
}

TEST(EncodingPerf, DenseInvalidDecode) {
    const std::string junk(4u << 20, '\xFD');
    auto codec = frost::text::platformCodecs().open(932);
    ASSERT_TRUE(codec.has_value());
    std::string out;
    frost::text::DecodeStats stats;
    const auto start = Clock::now();
    auto r = frost::text::decodeLegacy(**codec, junk, out, stats);
    const double s = secondsSince(start);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(stats.replacements, junk.size());
    const double mbs = static_cast<double>(junk.size()) / 1e6 / s;
    std::printf("[encoding perf] DenseInvalidDecode (%s): 4 MiB of FD through Shift_JIS in %.1f ms = %.1f MB/s (bound "
                "2 MB/s)\n",
                kAssertBudgets ? "budgeted" : "report-only", s * 1e3, mbs);
    if (kAssertBudgets) {
        EXPECT_GE(mbs, 2.0) << "D17: dense errors must stay linear";
    }
}

} // namespace
