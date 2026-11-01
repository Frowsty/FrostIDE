// SPDX-License-Identifier: Apache-2.0
//
// The encoding stress tests (label stress; -Full only): EncodingDifferentialStress.FullBmpEncode,
// every BMP scalar through encodeLegacy for all 16 code pages against
// tests/text/goldens/encodings/fingerprints-bmp.txt (probe F2: the differential of Microsoft NLS and
// glibc once the D4 rules apply), and EncodingStress.LosslessProperty, TextFile.LosslessIffRoundTrip
// at scale.
/*
 * FullBmpEncode uses the golden, update (FROST_UPDATE_GOLDENS=1, then fail) and dump mechanism of
 * EncodingDifferential. LosslessProperty runs 2000 seeds, or 20000 with FROST_STRESS_FULL=1 (the
 * M1 exit scale); FROST_ENCODING_SEED=<s> runs one. Pages and seeds run on up to four threads.
 */

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/encoding.hpp"
#include "frost/text/text_codec.hpp"

#include "support/test_env.hpp"
#include "text/support/encoding_checks.hpp"
#include "text/support/encoding_oracle.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using frost::text::test::CanonicalTable;

struct BmpResult {
    std::uint16_t codePage = 0;
    std::uint64_t encodable = 0;
    std::string line;
    CanonicalTable table;
};

BmpResult encodeBmp(const frost::text::CodePageInfo& info) {
    BmpResult r;
    r.codePage = info.codePage;
    auto codec = frost::text::platformCodecs().open(info.codePage);
    if (!codec) {
        r.line = std::format("cp{} unavailable: {}", info.codePage, codec.error().describe());
        return r;
    }
    std::string table;
    table.reserve(63488 * 12);
    std::uint64_t scalars = 0;
    std::string utf8;
    std::string bytes;
    for (char32_t c = 0; c <= 0xFFFF; ++c) {
        if (c >= 0xD800 && c <= 0xDFFF) {
            continue;
        }
        ++scalars;
        utf8.clear();
        bytes.clear();
        frost::text::test::oracleAppendUtf8(c, utf8);
        auto stop = frost::text::encodeLegacy(**codec, utf8, bytes);
        table += std::format("U+{:04X} ", static_cast<std::uint32_t>(c));
        if (stop && !*stop) {
            ++r.encodable;
            table += frost::text::test::toHex(bytes);
        } else {
            table += stop ? "unencodable" : "error";
        }
        table += '\n';
    }
    r.line = std::format("cp{} bmp scalars={} encodable={} fnv1a64={:016x}", info.codePage, scalars, r.encodable,
                         frost::text::test::fnv1a64(table));
    r.table = {std::format("cp{}.bmp", info.codePage), std::move(table)};
    return r;
}

TEST(EncodingDifferentialStress, FullBmpEncode) {
    const auto pages = frost::text::supportedCodePages();
    std::vector<BmpResult> results(pages.size());
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::size_t i = next.fetch_add(1); i < pages.size(); i = next.fetch_add(1)) {
                results[i] = encodeBmp(pages[i]);
            }
        });
    }
    for (std::thread& w : workers) {
        w.join();
    }
    std::vector<std::string> lines;
    std::vector<CanonicalTable> tables;
    for (const BmpResult& r : results) {
        lines.push_back(r.line);
        tables.push_back(r.table);
        // F2's reversible counts.
        const std::uint64_t expected = r.codePage == 932    ? 9398
                                       : r.codePage == 874  ? 248
                                       : r.codePage == 1253 ? 253
                                       : r.codePage == 1257 ? 254
                                                            : 256;
        EXPECT_EQ(r.encodable, expected) << r.codePage;
    }
    const auto checked = frost::text::test::checkGolden(FROST_ENCODING_GOLDENS_DIR, FROST_ENCODING_DUMP_DIR,
                                                        "fingerprints-bmp.txt", lines, tables);
    EXPECT_TRUE(checked.has_value()) << checked.error().describe();
}

TEST(EncodingStress, LosslessProperty) {
    const std::uint64_t count = frost::test::stressFull() ? 20000 : 2000;
    const auto seeds = frost::text::test::encodingSeeds(count);
    std::atomic<std::uint64_t> next{seeds.first};
    std::mutex mutex;
    std::map<std::uint64_t, std::string> failures;
    std::vector<std::thread> workers;
    const unsigned threads = seeds.single ? 1u : std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::uint64_t seed = next.fetch_add(1); seed < seeds.last; seed = next.fetch_add(1)) {
                // The same inputs as TextFile.LosslessIffRoundTrip for seeds below 200.
                frost::sched::detail::SplitMix64 rng(seed * 31 + 7);
                const std::string bytes = frost::text::test::randomEncodingInput(rng, 4096);
                const std::uint16_t legacy = seed % 2 == 0 ? 1252 : 932;
                for (unsigned index = 0; index < 8; ++index) {
                    frost::text::LoadOptions o;
                    o.detect.legacyCodePage = legacy;
                    o.detect.defaultEncoding = frost::text::Encoding::utf8();
                    o.defaultSeparator = frost::text::LineSeparator::Lf;
                    o.encoding = frost::text::test::overrideByIndex(index);
                    const auto checked =
                        frost::text::test::checkLoadAndSave(bytes, o, frost::text::platformCodecs(), false);
                    if (!checked) {
                        const std::lock_guard lock(mutex);
                        failures.emplace(seed, std::format("override {}: {}", index, checked.error().describe()));
                        break;
                    }
                }
            }
        });
    }
    for (std::thread& w : workers) {
        w.join();
    }
    if (!failures.empty()) {
        const auto& [seed, what] = *failures.begin();
        FAIL() << failures.size() << " seeds failed; the lowest: " << what << "\n"
               << frost::text::test::reproduceSeed("text_encoding_stress_tests", "EncodingStress.LosslessProperty",
                                                   seed);
    }
}

} // namespace
