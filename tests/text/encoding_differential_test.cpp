// SPDX-License-Identifier: Apache-2.0
//
// EncodingDifferential: the differential (two independent implementations: Microsoft NLS and
// glibc) of the 16 M1 code pages through the one shared driver (ADR 0018, D3, D4 and probe
// F1-F3). Canonical tables built only from decodeLegacy and encodeLegacy outputs are hashed into
// fingerprint lines and compared with tests/text/goldens/encodings/fingerprints.txt.
/*
 * The golden is generated on Windows (FROST_UPDATE_GOLDENS=1, a run that then fails), so the
 * Windows run is a regression check and the Linux run is the differential. Per code page:
 *   "cp932 decode units=11776 valid=9796 nort=398 fnv1a64=<hex>": every single byte 00..FF and,
 *     for DBCS pages, every lead byte x trail 40..FF, decoded alone (Final). A line per unit: the
 *     unit's hex, the scalars or "invalid", then "rt" or "nort <hex>" (the reversible encode of
 *     the decoded text, "-" when it is unencodable).
 *   "cp932 encode-sample scalars=<n> encodable=<n> fnv1a64=<hex>": every scalar the decode table
 *     produced, a fixed list of troublemakers and 2048 BMP scalars from seed 1, each encoded
 *     alone. A line per scalar: U+XXXX, then the bytes or "unencodable".
 *   "cp932 streams=4 fnv1a64=<hex>": four 16 KiB streams of the page's valid units with 2% junk
 *     bytes from fixed seeds, decoded whole; the hash covers each output and replacement count.
 * On a mismatch the tables go to FROST_ENCODING_DUMP_DIR/<cp>.<table>.<os>.txt.
 * RoundTripCounts pins the probe's numbers independently of the golden.
 */

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/encoding.hpp"
#include "frost/text/text_codec.hpp"

#include "text/support/encoding_checks.hpp"
#include "text/support/encoding_oracle.hpp"
#include "text/support/text_oracle.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::sched::detail::SplitMix64;
using frost::text::CodePageInfo;
using frost::text::DecodeStats;
using frost::text::ITextCodec;
using frost::text::test::CanonicalTable;
using frost::text::test::fnv1a64;
using frost::text::test::toHex;

// What the canonical tables of one code page found.
struct PageTables {
    std::uint16_t codePage = 0;
    std::uint64_t units = 0;
    std::uint64_t valid = 0;
    std::uint64_t nort = 0;
    std::map<unsigned, std::uint64_t> nortPerLead; // Lead byte -> non-round-trip pairs.
    std::uint64_t sampleScalars = 0;
    std::uint64_t encodable = 0;        // Within the encode sample.
    std::uint64_t decodedEncodable = 0; // Among the scalars the decode table produced (F2's count).
    std::vector<std::string> lines;     // The three fingerprint lines.
    std::vector<CanonicalTable> tables; // Their canonical tables.
};

std::string scalarList(std::string_view utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size();) {
        char32_t c = 0;
        const std::size_t length = frost::text::test::oracleDecode(utf8, i, c);
        if (length == 0) {
            return "ill-formed";
        }
        if (!out.empty()) {
            out.push_back(',');
        }
        out += std::format("U+{:04X}", static_cast<std::uint32_t>(c));
        i += length;
    }
    return out;
}

// The reversible encode of `utf8` alone: its bytes, or nullopt when it stops.
std::optional<std::string> encodeAlone(ITextCodec& codec, std::string_view utf8) {
    std::string bytes;
    auto r = frost::text::encodeLegacy(codec, utf8, bytes);
    EXPECT_TRUE(r.has_value()) << r.error().describe();
    if (!r || *r) {
        return std::nullopt;
    }
    return bytes;
}

PageTables buildTables(const CodePageInfo& info) {
    PageTables t;
    t.codePage = info.codePage;
    auto opened = frost::text::platformCodecs().open(info.codePage);
    EXPECT_TRUE(opened.has_value()) << info.codePage;
    if (!opened) {
        return t;
    }
    ITextCodec& codec = **opened;
    const std::string label = std::format("cp{}", info.codePage);

    // Decode table.
    std::vector<std::string> units;
    for (unsigned b = 0; b < 256; ++b) {
        units.emplace_back(1, static_cast<char>(b));
    }
    for (const auto& range : info.leadBytes) {
        for (unsigned lead = range.first; lead <= range.last; ++lead) {
            for (unsigned trail = 0x40; trail <= 0xFF; ++trail) {
                units.push_back({static_cast<char>(lead), static_cast<char>(trail)});
            }
        }
    }
    std::string decodeTable;
    std::set<std::string> decodedScalars; // UTF-8 of each valid unit's text.
    std::vector<std::string> validUnits;
    for (const std::string& unit : units) {
        std::string text;
        DecodeStats stats;
        auto r = frost::text::decodeLegacy(codec, unit, text, stats);
        EXPECT_TRUE(r.has_value());
        ++t.units;
        decodeTable += toHex(unit);
        if (stats.replacements != 0) {
            decodeTable += " invalid\n";
            continue;
        }
        ++t.valid;
        validUnits.push_back(unit);
        decodedScalars.insert(text);
        decodeTable += ' ' + scalarList(text);
        const auto encoded = encodeAlone(codec, text);
        if (encoded && *encoded == unit) {
            decodeTable += " rt\n";
        } else {
            ++t.nort;
            if (unit.size() == 2) {
                ++t.nortPerLead[static_cast<std::uint8_t>(unit[0])];
            }
            decodeTable += " nort " + (encoded ? toHex(*encoded) : std::string("-")) + '\n';
        }
    }
    t.lines.push_back(std::format("{} decode units={} valid={} nort={} fnv1a64={:016x}", label, t.units, t.valid,
                                  t.nort, fnv1a64(decodeTable)));
    t.tables.push_back({label + ".decode", decodeTable});

    // Encode sample: decoded scalars, troublemakers, 2048 BMP scalars from seed 1.
    std::vector<char32_t> sample;
    for (const std::string& s : decodedScalars) {
        char32_t c = 0;
        if (frost::text::test::oracleDecode(s, 0, c) == s.size()) {
            sample.push_back(c);
        }
    }
    const std::size_t decodedCount = sample.size();
    constexpr char32_t kTroublemakers[] = {0xA2,   0xA3,   0xA5,   0xAC,   0x2014, 0x2016, 0x203E, 0x2212,
                                           0x301C, 0xFF5E, 0x2170, 0x4E28, 0xFFFD, 0x20AC, 0x221E, 0xDF};
    sample.insert(sample.end(), std::begin(kTroublemakers), std::end(kTroublemakers));
    for (char32_t c = 0xF8F0; c <= 0xF8F3; ++c) {
        sample.push_back(c);
    }
    for (char32_t c = 0xF8F9; c <= 0xF8FB; ++c) {
        sample.push_back(c);
    }
    for (char32_t c = 0xF8C1; c <= 0xF8C8; ++c) {
        sample.push_back(c);
    }
    for (char32_t c = 0xF8FC; c <= 0xF8FD; ++c) {
        sample.push_back(c);
    }
    for (char32_t c = 0x80; c <= 0x9F; ++c) {
        sample.push_back(c);
    }
    SplitMix64 rng(1);
    for (int k = 0; k < 2048; ++k) {
        char32_t c = 0;
        do {
            c = static_cast<char32_t>(rng.below(0x10000));
        } while (c >= 0xD800 && c <= 0xDFFF);
        sample.push_back(c);
    }
    std::string encodeTable;
    for (std::size_t k = 0; k < sample.size(); ++k) {
        std::string utf8;
        frost::text::test::oracleAppendUtf8(sample[k], utf8);
        const auto encoded = encodeAlone(codec, utf8);
        ++t.sampleScalars;
        encodeTable += std::format("U+{:04X} ", static_cast<std::uint32_t>(sample[k]));
        if (encoded) {
            ++t.encodable;
            if (k < decodedCount) {
                ++t.decodedEncodable;
            }
            encodeTable += toHex(*encoded) + '\n';
        } else {
            encodeTable += "unencodable\n";
        }
    }
    t.lines.push_back(std::format("{} encode-sample scalars={} encodable={} fnv1a64={:016x}", label, t.sampleScalars,
                                  t.encodable, fnv1a64(encodeTable)));
    t.tables.push_back({label + ".encode-sample", encodeTable});

    // Streams of valid units with 2% junk.
    std::string streamTable;
    std::uint64_t streamHash = frost::text::test::kFnvOffsetBasis;
    for (std::uint64_t k = 0; k < 4; ++k) {
        SplitMix64 srng(info.codePage * 16ULL + k);
        std::string bytes;
        while (bytes.size() < 16 * 1024) {
            if (srng.below(50) == 0) {
                bytes.push_back(static_cast<char>(srng.below(256)));
            } else {
                bytes += validUnits[srng.below(validUnits.size())];
            }
        }
        std::string text;
        DecodeStats stats;
        auto r = frost::text::decodeLegacy(codec, bytes, text, stats);
        EXPECT_TRUE(r.has_value());
        streamHash = fnv1a64(text, streamHash);
        streamHash = fnv1a64(std::format("|{}|", stats.replacements), streamHash);
        streamTable += std::format("stream {} bytes={} replacements={} text-fnv1a64={:016x}\n", k, bytes.size(),
                                   stats.replacements, fnv1a64(text));
    }
    t.lines.push_back(std::format("{} streams=4 fnv1a64={:016x}", label, streamHash));
    t.tables.push_back({label + ".streams", streamTable});
    return t;
}

// The tables of every page, built once per process.
const std::vector<PageTables>& allTables() {
    static const std::vector<PageTables> all = [] {
        std::vector<PageTables> out;
        for (const CodePageInfo& info : frost::text::supportedCodePages()) {
            out.push_back(buildTables(info));
        }
        return out;
    }();
    return all;
}

TEST(EncodingDifferential, FingerprintsMatchGolden) {
    std::vector<std::string> lines;
    std::vector<CanonicalTable> tables;
    for (const PageTables& t : allTables()) {
        lines.insert(lines.end(), t.lines.begin(), t.lines.end());
        tables.insert(tables.end(), t.tables.begin(), t.tables.end());
    }
    const auto checked = frost::text::test::checkGolden(FROST_ENCODING_GOLDENS_DIR, FROST_ENCODING_DUMP_DIR,
                                                        "fingerprints.txt", lines, tables);
    EXPECT_TRUE(checked.has_value()) << checked.error().describe();
}

TEST(EncodingDifferential, RoundTripCounts) {
    for (const PageTables& t : allTables()) {
        const std::uint64_t expectedValid = t.codePage == 932    ? 9796
                                            : t.codePage == 874  ? 248
                                            : t.codePage == 1253 ? 253
                                            : t.codePage == 1257 ? 254
                                                                 : 256;
        EXPECT_EQ(t.valid, expectedValid) << t.codePage;
        // F2's reversible encodable counts: every reversibly encodable scalar is the text of a
        // valid unit, so counting among the decoded scalars is exact (FullBmpEncode re-checks).
        const std::uint64_t expectedEncodable = t.codePage == 932 ? 9398 : expectedValid;
        EXPECT_EQ(t.decodedEncodable, expectedEncodable) << t.codePage;
        if (t.codePage == 932) {
            EXPECT_EQ(t.units, 11776u);
            EXPECT_EQ(t.nort, 398u);
            const std::map<unsigned, std::uint64_t> perLead = {{0x87, 9}, {0xED, 188}, {0xEE, 186}, {0xFA, 15}};
            EXPECT_EQ(t.nortPerLead, perLead);
        } else {
            EXPECT_EQ(t.units, 256u) << t.codePage;
            EXPECT_EQ(t.nort, 0u) << t.codePage;
        }
    }
}

} // namespace
