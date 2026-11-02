// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for frost::document's LoadService on MemFs (ADR 0028; ADR 0029 decision 7): the
// large-file guard at its boundary (a lowered limit; the real 128 MiB file is in the integration
// tests), the read-only open of a text whose longest line exceeds the view's cap, the disk
// fingerprint, encoding overrides, and the guard on restored text.

#include "frost/document/load_service.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/text/encoding.hpp"

#include "document_fixture.hpp"
#include <gtest/gtest.h>

#include <string>

namespace frost::document::test {
namespace {

using LoadServiceTest = DocumentFixture;

TEST_F(LoadServiceTest, RefusesFilesAtTheSizeLimit) {
    LoadServiceOptions limits;
    limits.maxFileBytes = 1000;
    const LoadService loader(fs, limits);
    writeExternally("below.txt", std::string(999, 'a'));
    writeExternally("at.txt", std::string(1000, 'a'));
    auto below = loader.load(path("below.txt"));
    ASSERT_TRUE(below) << below.error().describe();
    EXPECT_EQ(below->buffer.snapshot().size(), ByteOffset{999});
    ASSERT_TRUE(below->disk.has_value());
    EXPECT_EQ(below->disk->size, 999U);
    EXPECT_EQ(below->disk->fingerprint, storage::xxh3Hash128(std::string(999, 'a')));

    auto at = loader.load(path("at.txt"));
    ASSERT_FALSE(at);
    EXPECT_EQ(at.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(at.error().describe().find("large-file mode"), std::string::npos) << at.error().describe();

    // Restored text (the hot-exit journal) goes through the same guard.
    EXPECT_TRUE(loader.fromUtf8(std::string(999, 'b'), "untitled:1"));
    EXPECT_EQ(loader.fromUtf8(std::string(1000, 'b'), "untitled:1").error().code(), ErrorCode::OutOfRange);

    // The default is 128 MiB.
    EXPECT_EQ(LoadServiceOptions{}.maxFileBytes, std::uint64_t{128} << 20);
    EXPECT_EQ(kMaxOpenFileBytes, std::uint64_t{128} << 20);
}

TEST_F(LoadServiceTest, OverlongLinesOpenReadOnly) {
    // One line of exactly the cap is editable; one character more is not.
    writeExternally("ok.txt", std::string(kMaxEditableLineChars, 'x') + "\nshort\n");
    writeExternally("long.txt", "first\n" + std::string(kMaxEditableLineChars + 1, 'x') + "\n");
    FileDocument* ok = open("ok.txt");
    FileDocument* longLine = open("long.txt");
    ASSERT_TRUE(ok != nullptr && longLine != nullptr);
    EXPECT_EQ(ok->readOnly(), ReadOnlyReason::None);
    EXPECT_EQ(longLine->readOnly(), ReadOnlyReason::LongLine);

    // The cap counts characters, not bytes: two-byte characters up to the cap stay editable.
    std::string twoByte;
    for (std::uint32_t i = 0; i < kMaxEditableLineChars; ++i) {
        twoByte += "\xC3\xA9";
    }
    writeExternally("accents.txt", twoByte);
    FileDocument* accents = open("accents.txt");
    ASSERT_NE(accents, nullptr);
    EXPECT_EQ(accents->readOnly(), ReadOnlyReason::None);

    // A clean reload that brings an over-long line makes the document read-only too.
    writeExternally("ok.txt", std::string(kMaxEditableLineChars + 5, 'y'));
    EXPECT_EQ(manager().checkDisk(*ok).kind, ExternalChangeKind::Reloaded);
    EXPECT_EQ(ok->readOnly(), ReadOnlyReason::LongLine);
}

TEST_F(LoadServiceTest, EncodingOverrideAndErrors) {
    writeExternally("legacy.txt", "na\xEFve\n");
    const LoadService loader(fs);
    auto forced = loader.load(path("legacy.txt"), text::Encoding::legacy(1252));
    ASSERT_TRUE(forced) << forced.error().describe();
    EXPECT_EQ(forced->buffer.snapshot().text(), "na\xC3\xAFve\n");
    EXPECT_EQ(forced->encoding.detection.source, text::DetectionSource::UserOverride);

    EXPECT_EQ(loader.load(path("missing.txt")).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(loader.load(workDirectory()).error().code(), ErrorCode::InvalidArgument);

    // The manager keeps the override for reloads ("Reload in" stays in force).
    auto opened = manager().open(path("legacy.txt"), text::Encoding::legacy(1252));
    ASSERT_TRUE(opened);
    writeExternally("legacy.txt", "caf\xE9\n");
    EXPECT_EQ(manager().checkDisk(**opened).kind, ExternalChangeKind::Reloaded);
    EXPECT_EQ(textOf(**opened), "caf\xC3\xA9\n");
}

} // namespace
} // namespace frost::document::test
