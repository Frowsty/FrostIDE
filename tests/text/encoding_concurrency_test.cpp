// SPDX-License-Identifier: Apache-2.0
//
// EncodingConcurrency: the platform provider and its codecs used from several threads at once
// (text_codec.hpp: platformCodecs() is thread-safe, each codec belongs to one thread). The TSan
// target of WSL -Full for frost::text's encodings.

#include "frost/text/text_codec.hpp"
#include "frost/text/text_file.hpp"
#include "frost/text/unicode_codec.hpp"

#include "text/support/encoding_fixtures.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

TEST(EncodingConcurrency, PlatformCodecsInParallel) {
    constexpr int kThreads = 8;
    constexpr int kIterations = 200;
    const auto& cp1252 = frost::text::test::fixture("cp1252");
    const auto& cp932 = frost::text::test::fixture("cp932");
    const auto& utf16 = frost::text::test::fixture("utf16le-bom");
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIterations; ++i) {
                for (const auto* f : {&cp1252, &cp932}) {
                    auto codec = frost::text::platformCodecs().open(f->legacyCodePage);
                    if (!codec) {
                        failures.fetch_add(1);
                        continue;
                    }
                    std::string text;
                    frost::text::DecodeStats stats;
                    auto decoded = frost::text::decodeLegacy(**codec, f->bytes, text, stats);
                    std::string bytes;
                    auto encoded = frost::text::encodeLegacy(**codec, text, bytes);
                    if (!decoded || text != f->text || stats.replacements != 0 || !encoded || *encoded ||
                        bytes != f->bytes) {
                        failures.fetch_add(1);
                    }
                }
                std::string text;
                frost::text::DecodeStats stats;
                (void)frost::text::appendUtf16Lossy(std::string_view(utf16.bytes).substr(2),
                                                    frost::text::Endian::Little, text, stats);
                std::string bytes;
                frost::text::appendUtf16(text, frost::text::Endian::Little, bytes);
                if (text != utf16.text || bytes != utf16.bytes.substr(2)) {
                    failures.fetch_add(1);
                }
                // The whole pipeline too, with detection opening codecs on every thread.
                auto loaded = frost::text::decodeTextFile(cp932.bytes, frost::text::test::fixtureOptions(cp932));
                if (!loaded || loaded->buffer.current().text() != cp932.text) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(failures.load(), 0);
}

} // namespace
