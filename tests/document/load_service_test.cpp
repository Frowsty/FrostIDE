// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for frost::document's LoadService on MemFs (ADR 0028; ADR 0029 decision 7): the
// large-file guard at its boundary (a lowered limit; the real 128 MiB file is in the integration
// tests), the read-only open of a text with a line longer than the view's cap in bytes, the disk
// fingerprint, encoding overrides, and the guard on restored text.

#include "frost/document/load_service.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/text/encoding.hpp"
#include "frost/text/large_file.hpp"

#include "document_fixture.hpp"
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace frost::document::test {
namespace {

using LoadServiceTest = DocumentFixture;

// `piece` repeated `count` times.
std::string repeat(std::string_view piece, std::size_t count) {
    std::string out;
    out.reserve(piece.size() * count);
    for (std::size_t i = 0; i < count; ++i) {
        out += piece;
    }
    return out;
}

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
    // The cap is the editor view's long-line cap (ADR 0037), in bytes.
    EXPECT_EQ(kMaxEditableLineBytes, text::kLongLineCapBytes);
    EXPECT_EQ(LoadServiceOptions{}.maxEditableLineBytes, std::uint64_t{1'000'000});
    const std::size_t cap = kMaxEditableLineBytes;

    // One line of exactly the cap is editable; one byte more is not.
    writeExternally("ok.txt", std::string(cap, 'x') + "\nshort\n");
    writeExternally("long.txt", "first\n" + std::string(cap + 1, 'x') + "\n");
    FileDocument* ok = open("ok.txt");
    FileDocument* longLine = open("long.txt");
    ASSERT_TRUE(ok != nullptr && longLine != nullptr);
    EXPECT_EQ(ok->readOnly(), ReadOnlyReason::None);
    EXPECT_EQ(longLine->readOnly(), ReadOnlyReason::LongLine);

    // The cap counts bytes, not characters: two-byte characters filling exactly the cap stay
    // editable (the CRLF break does not count), and one more makes the line long although it has
    // only half the cap in characters.
    writeExternally("accents.txt", repeat("\xC3\xA9", cap / 2) + "\r\n");
    writeExternally("accents-long.txt", repeat("\xC3\xA9", cap / 2 + 1));
    FileDocument* accents = open("accents.txt");
    FileDocument* accentsLong = open("accents-long.txt");
    ASSERT_TRUE(accents != nullptr && accentsLong != nullptr);
    EXPECT_EQ(accents->readOnly(), ReadOnlyReason::None);
    EXPECT_EQ(accentsLong->readOnly(), ReadOnlyReason::LongLine);

    // The line with the most characters need not be the one with the most bytes: 600,000 ASCII
    // characters, then 400,000 three-byte characters (1,200,000 bytes).
    writeExternally("mixed.txt", std::string(600'000, 'a') + "\n" + repeat("\xE2\x82\xAC", 400'000) + "\n");
    FileDocument* mixed = open("mixed.txt");
    ASSERT_NE(mixed, nullptr);
    EXPECT_EQ(mixed->readOnly(), ReadOnlyReason::LongLine);

    // A text larger than the cap made of short lines is editable.
    writeExternally("big.txt", repeat("short line\n", cap / 5));
    FileDocument* big = open("big.txt");
    ASSERT_NE(big, nullptr);
    EXPECT_EQ(big->readOnly(), ReadOnlyReason::None);

    // Restored text (the hot-exit journal) goes through the same rule.
    const LoadService loader(fs);
    auto restored = loader.fromUtf8(std::string(cap + 1, 'z'), "untitled:1");
    ASSERT_TRUE(restored) << restored.error().describe();
    EXPECT_EQ(restored->readOnly, ReadOnlyReason::LongLine);

    // A clean reload that brings an over-long line makes the document read-only too.
    writeExternally("ok.txt", std::string(cap + 5, 'y'));
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
