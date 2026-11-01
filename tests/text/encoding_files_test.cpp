// SPDX-License-Identifier: Apache-2.0
//
// EncodingFiles: the encoding fixtures through the real file system (ADR 0018): write, load with
// loadTextFile, save with encodeForSave, write again and read back byte for byte, in a directory
// whose name is non-ASCII with a space, and beyond MAX_PATH on Windows; Visual Studio C++ sources
// (UTF-8 with and without a BOM, windows-1252) with production defaults; and the committed fixture
// files loaded from the source tree.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/text/text_file.hpp"

#include "support/scratch_dir.hpp"
#include "text/support/encoding_fixtures.hpp"
#include "text/support/encoding_oracle.hpp"
#include <gtest/gtest.h>

#include <format>
#include <set>
#include <string>

namespace {

using frost::text::LossKind;
using frost::text::SaveConsent;
using frost::text::test::EncodingFixture;
using frost::text::test::toHex;

frost::io::IFileSystem& fs() { return frost::io::realFileSystem(); }

void roundTripIn(const std::string& dir) {
    std::set<std::string> done;
    for (const EncodingFixture& f : frost::text::test::encodingFixtures()) {
        if (!done.insert(f.name).second) {
            continue;
        }
        SCOPED_TRACE(f.name);
        const std::string original = frost::io::joinPath(dir, "orig " + f.file).value();
        const std::string copy = frost::io::joinPath(dir, "saved " + f.file).value();
        ASSERT_TRUE(fs().writeFileAtomic(original, f.bytes).has_value());
        auto loaded = frost::text::loadTextFile(fs(), original, frost::text::test::fixtureOptions(f));
        ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
        EXPECT_EQ(loaded->encoding.format.encoding, f.encoding);
        const SaveConsent consent = f.loss == LossKind::None ? SaveConsent::None : SaveConsent::AcceptLoss;
        auto bytes = frost::text::encodeForSave(loaded->buffer.current(), loaded->encoding, consent);
        if (f.consentUnencodable) {
            // A Replaced legacy load: its U+FFFD has no mapping, so even consent cannot save it.
            ASSERT_FALSE(bytes.has_value());
            EXPECT_EQ(bytes.error().code(), frost::ErrorCode::InvalidArgument);
            continue;
        }
        ASSERT_TRUE(bytes.has_value()) << bytes.error().describe();
        ASSERT_TRUE(fs().writeFileAtomic(copy, *bytes).has_value());
        auto back = fs().readFile(copy);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(toHex(*back), toHex(f.loss == LossKind::None ? f.bytes : f.consentBytes));
    }
}

TEST(EncodingFiles, RoundTripThroughRealFileSystem) {
    const frost::test::ScratchDir scratch("enc-\xC3\xBC-\xE6\x97\xA5\xE6\x9C\xAC dir"); // "enc-ü-日本 dir"
    ASSERT_FALSE(scratch.path().empty());
    roundTripIn(scratch.path());
#if defined(_WIN32)
    // A directory beyond MAX_PATH (260 UTF-16 units): frost::io uses \\?\ paths internally.
    std::string deep = scratch.path();
    for (int depth = 0; deep.size() < 300; ++depth) {
        deep = frost::io::joinPath(deep, std::format("segment-{:02}-{}", depth, std::string(40, 'x'))).value();
    }
    ASSERT_TRUE(fs().createDirectories(deep).has_value());
    ASSERT_GT(deep.size(), 260u);
    roundTripIn(deep);
#endif
}

/*
 * The user's work files: Visual Studio C++ sources in UTF-8 with or without a BOM, or in
 * windows-1252, all with CRLF. Loaded with production defaults (platform codecs and the machine's
 * legacy page), each must be lossless and save back byte for byte, before and after an edit. The
 * late-1252 file is ASCII for longer than the detection sample, so UTF-8 fails only on the load.
 */
TEST(EncodingFiles, VisualStudioSourcesWithProductionDefaults) {
    const std::string body = "#include \"pch.h\"\r\n\r\nint main()\r\n{\r\n    return 0;\r\n}\r\n";
    const std::string padding = std::string(frost::text::kDetectSampleBytes + 100, ' ') + "\r\n";
    struct Source {
        const char* name;
        std::string bytes;
        bool bom;
        bool legacy;
    };
    const Source sources[] = {
        {"utf8-bom.cpp", "\xEF\xBB\xBF// Caf\xC3\xA9 \xC2\xA9 2026\r\n" + body, true, false},
        {"utf8.cpp", "// Caf\xC3\xA9 \xC2\xA9 2026\r\n" + body, false, false},
        {"ascii.cpp", "// plain\r\n" + body, false, false},
        {"cp1252.cpp", "// Caf\xE9 \xA9 2026 \x93quoted\x94\r\n" + body, false, true},
        {"cp1252-late.cpp", body + padding + "// na\xEFve \x80\r\n", false, true},
    };
    const frost::test::ScratchDir scratch("enc-vs-sources");
    ASSERT_FALSE(scratch.path().empty());
    for (const Source& s : sources) {
        SCOPED_TRACE(s.name);
        const std::string path = frost::io::joinPath(scratch.path(), s.name).value();
        ASSERT_TRUE(fs().writeFileAtomic(path, s.bytes).has_value());
        auto loaded = frost::text::loadTextFile(fs(), path, frost::text::LoadOptions{});
        ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
        const frost::text::DocumentEncoding& e = loaded->encoding;
        EXPECT_TRUE(e.fidelity.lossless());
        EXPECT_EQ(e.format.bom, s.bom);
        EXPECT_EQ(e.format.lineSeparator, frost::text::LineSeparator::CrLf);
        EXPECT_EQ(e.format.encoding.form == frost::text::EncodingForm::CodePage, s.legacy);
        auto saved = frost::text::encodeForSave(loaded->buffer.current(), e);
        ASSERT_TRUE(saved.has_value()) << saved.error().describe();
        EXPECT_TRUE(*saved == s.bytes) << "byte-exact save";

        // An edit with the document's separator keeps the rest of the file byte-exact.
        const std::string added = "// edited\r\n";
        ASSERT_TRUE(loaded->buffer.insert(loaded->buffer.current().size(), added).has_value());
        auto edited = frost::text::encodeForSave(loaded->buffer.current(), e);
        ASSERT_TRUE(edited.has_value()) << edited.error().describe();
        EXPECT_TRUE(*edited == s.bytes + added);
        ASSERT_TRUE(fs().writeFileAtomic(path, *edited).has_value());
        auto back = fs().readFile(path);
        ASSERT_TRUE(back.has_value());
        EXPECT_TRUE(*back == s.bytes + added);
    }
}

TEST(EncodingFiles, LoadCommittedFixture) {
    for (const auto& [name, legacy] :
         {std::pair{"cp932", std::uint16_t{932}}, std::pair{"utf16le-bom", std::uint16_t{1252}}}) {
        const EncodingFixture& f = frost::text::test::fixture(name);
        frost::text::LoadOptions options = frost::text::test::fixtureOptions(f);
        options.detect.legacyCodePage = legacy;
        auto loaded =
            frost::text::loadTextFile(fs(), std::format("{}/{}", FROST_ENCODING_FIXTURES_DIR, f.file), options);
        ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
        EXPECT_EQ(loaded->encoding.format.encoding, f.encoding) << name;
        EXPECT_EQ(loaded->buffer.current().text(), f.text) << name;
        EXPECT_TRUE(loaded->encoding.fidelity.lossless()) << name;
    }
}

} // namespace
