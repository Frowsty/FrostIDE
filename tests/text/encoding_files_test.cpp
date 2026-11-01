// SPDX-License-Identifier: Apache-2.0
//
// EncodingFiles: the encoding fixtures through the real file system (ADR 0018): write, load with
// loadTextFile, save with encodeForSave, write again and read back byte for byte, in a directory
// whose name is non-ASCII with a space, and beyond MAX_PATH on Windows; and the committed fixture
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
