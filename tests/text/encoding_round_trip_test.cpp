// SPDX-License-Identifier: Apache-2.0
//
// EncodingRoundTrip: the M1 exit's byte-exact CRLF/BOM goldens at the codec level (feature matrix
// row 60's acceptance test is FixturesByteExact; ADR 0018). The committed files in
// tests/text/fixtures/encodings are loaded with each fixture's options and saved back.
/*
 * The fixture files and the text_decode fuzz seeds are binary, so they are generated from the
 * arrays in encoding_fixtures.cpp: FROST_UPDATE_GOLDENS=1 makes FixturesMatchTable write them
 * (through io::realFileSystem(): createDirectories, then writeFileAtomic) and then fail, so an
 * update run never counts as verification. Normal runs only read.
 */

#include "frost/io/file_system.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/text/text_file.hpp"

#include "text/support/encoding_checks.hpp"
#include "text/support/encoding_fixtures.hpp"
#include "text/support/encoding_oracle.hpp"
#include <gtest/gtest.h>

#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::ErrorCode;
using frost::text::LossKind;
using frost::text::SaveConsent;
using frost::text::test::EncodingFixture;
using frost::text::test::encodingFixtures;
using frost::text::test::fixture;
using frost::text::test::fromHex;
using frost::text::test::fromU8;
using frost::text::test::toHex;

frost::io::IFileSystem& fs() { return frost::io::realFileSystem(); }

std::string fixturePath(std::string_view file) { return std::format("{}/{}", FROST_ENCODING_FIXTURES_DIR, file); }

// The hand-written fuzz seeds of tests/fuzz/corpus/text_decode (besides hand-ascii, which is
// committed as text): selected fixtures, the Unicode standard's five examples, the FF FE 00 00
// sizes and Shift_JIS's invalid pairs. Each is the harness's three header bytes plus the file.
std::map<std::string, std::string> fuzzSeeds() {
    std::map<std::string, std::string> seeds;
    for (const std::string_view name :
         {"lf-utf8", "mixed-eol", "utf8-bom-crlf", "utf16le-bom", "utf16be-bom", "utf16le-nobom", "utf32le-bom",
          "cp1252", "cp1252-c1", "cp932", "cp932-nec-dup", "utf16le-lone-surrogate", "utf16be-odd"}) {
        const EncodingFixture& f = fixture(name);
        seeds["hand-" + f.name] = frost::text::test::fuzzSeedHeader(f) + f.bytes;
    }
    // Override UTF-8 (byte 0 = 1), the default sample (byte 2 = 255).
    const std::string utf8Header("\x01\x00\xFF", 3);
    seeds["hand-unicode-3-8"] = utf8Header + fromHex("c0 af e0 80 bf f0 81 82 41");
    seeds["hand-unicode-3-9"] = utf8Header + fromHex("ed a0 80 ed bf bf ed af 41");
    seeds["hand-unicode-3-10"] = utf8Header + fromHex("f4 91 92 93 ff 41 80 bf 42");
    seeds["hand-unicode-3-11"] = utf8Header + fromHex("e1 80 e2 f0 91 92 f1 bf 41");
    seeds["hand-unicode-example"] = utf8Header + fromHex("61 f1 80 80 e1 80 c2 62 80 63 80 bf 64");
    // No override, platform codecs, legacy 1252.
    const std::string detectHeader("\x00\x00\xFF", 3);
    seeds["hand-bom-fffe0000-4"] = detectHeader + fromHex("ff fe 00 00");
    seeds["hand-bom-fffe0000-6"] = detectHeader + fromHex("ff fe 00 00 41 00");
    // Override cp932 (7) with legacy 932 (bits 4-5 = 1): 85 41 and 81 0A keep their ASCII.
    seeds["hand-cp932-invalid"] = std::string("\x17\x00\xFF", 3) + fromHex("85 41 0a 81 0a 41");
    return seeds;
}

TEST(EncodingRoundTrip, FixturesMatchTable) {
    std::map<std::string, std::string> files; // File name -> bytes; entries may share a file.
    for (const EncodingFixture& f : encodingFixtures()) {
        const auto [it, fresh] = files.emplace(f.file, f.bytes);
        ASSERT_TRUE(fresh || it->second == f.bytes) << f.file << " has two different arrays";
    }
    if (frost::text::test::updatingGoldens()) {
        ASSERT_TRUE(fs().createDirectories(FROST_ENCODING_FIXTURES_DIR).has_value());
        for (const auto& [file, bytes] : files) {
            ASSERT_TRUE(fs().writeFileAtomic(fixturePath(file), bytes).has_value()) << file;
        }
        ASSERT_TRUE(fs().createDirectories(FROST_ENCODING_CORPUS_DIR).has_value());
        for (const auto& [name, bytes] : fuzzSeeds()) {
            ASSERT_TRUE(fs().writeFileAtomic(std::format("{}/{}", FROST_ENCODING_CORPUS_DIR, name), bytes).has_value());
        }
        FAIL() << "goldens rewritten (" << FROST_ENCODING_FIXTURES_DIR << " and the text_decode seeds); rerun without "
               << "FROST_UPDATE_GOLDENS";
    }
    for (const auto& [file, bytes] : files) {
        auto read = fs().readFile(fixturePath(file));
        ASSERT_TRUE(read.has_value()) << file << ": " << read.error().describe()
                                      << " (generate the fixtures with FROST_UPDATE_GOLDENS=1)";
        EXPECT_EQ(toHex(*read), toHex(bytes)) << file << " differs from its array (git -text, rsync, an editor?)";
    }
    auto listed = fs().listDirectory(FROST_ENCODING_FIXTURES_DIR);
    ASSERT_TRUE(listed.has_value());
    std::set<std::string> names;
    for (const auto& entry : *listed) {
        names.insert(entry.name);
    }
    std::set<std::string> expected;
    for (const auto& [file, bytes] : files) {
        expected.insert(file);
    }
    EXPECT_EQ(names, expected) << "every file in the fixture directory has an array";
    // The committed seeds equal their arrays too.
    for (const auto& [name, bytes] : fuzzSeeds()) {
        auto read = fs().readFile(std::format("{}/{}", FROST_ENCODING_CORPUS_DIR, name));
        ASSERT_TRUE(read.has_value()) << name;
        EXPECT_EQ(toHex(*read), toHex(bytes)) << name;
    }
}

TEST(EncodingRoundTrip, FixturesByteExact) {
    for (const EncodingFixture& f : encodingFixtures()) {
        SCOPED_TRACE(f.name);
        auto bytes = fs().readFile(fixturePath(f.file));
        ASSERT_TRUE(bytes.has_value()) << bytes.error().describe();
        auto loaded = frost::text::decodeTextFile(*bytes, frost::text::test::fixtureOptions(f));
        ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
        const frost::text::DocumentEncoding& e = loaded->encoding;
        EXPECT_EQ(e.format.encoding, f.encoding);
        EXPECT_EQ(e.format.bom, f.bom);
        EXPECT_EQ(e.detection.source, f.source);
        EXPECT_GE(e.detection.confidence, f.minConfidence);
        EXPECT_EQ(loaded->eol, f.eol);
        EXPECT_EQ(loaded->eol.mixed(), f.eol.mixed());
        EXPECT_EQ(e.format.lineSeparator, f.separator);
        EXPECT_EQ(e.fidelity.loss, f.loss);
        EXPECT_EQ(e.fidelity.replacements, f.replacements);
        EXPECT_EQ(e.fidelity.firstLossFileOffset, f.firstLossFileOffset);
        EXPECT_EQ(e.fidelity.firstLossTextOffset,
                  f.firstLossTextOffset ? std::optional<ByteOffset>(ByteOffset{*f.firstLossTextOffset}) : std::nullopt);
        EXPECT_EQ(loaded->binaryLikely, f.binaryLikely);
        const frost::text::Snapshot text = loaded->buffer.snapshot();
        EXPECT_EQ(toHex(text.text()), toHex(f.text));
        auto saved = frost::text::encodeForSave(text, e);
        if (f.loss == LossKind::None) {
            ASSERT_TRUE(saved.has_value()) << saved.error().describe();
            EXPECT_EQ(toHex(*saved), toHex(*bytes)) << "byte-exact round trip";
        } else {
            ASSERT_FALSE(saved.has_value());
            EXPECT_EQ(saved.error().code(), ErrorCode::Corrupt);
            auto consent = frost::text::encodeForSave(text, e, SaveConsent::AcceptLoss);
            ASSERT_TRUE(consent.has_value()) << consent.error().describe();
            EXPECT_EQ(toHex(*consent), toHex(f.consentBytes));
            EXPECT_NE(*consent, *bytes);
        }
    }
}

frost::text::LoadedText loadFixture(std::string_view name) {
    const EncodingFixture& f = fixture(name);
    auto loaded = frost::text::decodeTextFile(f.bytes, frost::text::test::fixtureOptions(f));
    EXPECT_TRUE(loaded.has_value());
    return std::move(*loaded);
}

TEST(EncodingRoundTrip, EditThenSave) {
    {
        frost::text::LoadedText l = loadFixture("crlf-utf8");
        const std::string added = "// new" + std::string(frost::text::separatorBytes(l.encoding.format.lineSeparator));
        ASSERT_TRUE(l.buffer.insert(l.buffer.current().size(), added).has_value());
        auto saved = frost::text::encodeForSave(l.buffer.current(), l.encoding);
        ASSERT_TRUE(saved.has_value());
        EXPECT_EQ(*saved, fixture("crlf-utf8").bytes + "// new\r\n");
        EXPECT_TRUE(saved->ends_with("}\r\n// new\r\n"));
    }
    {
        frost::text::LoadedText l = loadFixture("utf16le-bom");
        ASSERT_TRUE(l.buffer.insert(ByteOffset{0}, fromU8(u8"\U0001F600")).has_value());
        auto saved = frost::text::encodeForSave(l.buffer.current(), l.encoding);
        ASSERT_TRUE(saved.has_value());
        EXPECT_EQ(toHex(*saved), toHex(fromHex("ff fe 3d d8 00 de") + fixture("utf16le-bom").bytes.substr(2)));
    }
    {
        frost::text::LoadedText l = loadFixture("cp932");
        ASSERT_TRUE(l.buffer.insert(l.buffer.current().size(), fromU8(u8"\u30c6\u30b9\u30c8")).has_value());
        auto saved = frost::text::encodeForSave(l.buffer.current(), l.encoding);
        ASSERT_TRUE(saved.has_value()) << saved.error().describe();
        EXPECT_EQ(toHex(*saved), toHex(fixture("cp932").bytes + fromHex("83 65 83 58 83 67")));
    }
    {
        frost::text::LoadedText l = loadFixture("cp1252");
        const ByteOffset end = l.buffer.current().size();
        ASSERT_TRUE(l.buffer.insert(end, fromU8(u8"\U0001F600")).has_value());
        auto saved = frost::text::encodeForSave(l.buffer.current(), l.encoding);
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(saved.error().message().find(std::format("U+1F600 at byte {}", end.value())), std::string::npos)
            << saved.error().message();
        auto at = frost::text::firstUnencodable(l.buffer.current(), l.encoding.format.encoding);
        ASSERT_TRUE(at.has_value());
        EXPECT_EQ(*at, end);
    }
}

} // namespace
